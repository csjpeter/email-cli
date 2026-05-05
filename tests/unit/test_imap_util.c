#include "test_helpers.h"
#include "imap_util.h"
#include "raii.h"
#include <string.h>
#include <stdlib.h>

void test_imap_util(void) {

    /* Pure ASCII — no encoding, output identical to input */
    {
        RAII_STRING char *r = imap_utf7_decode("INBOX");
        ASSERT(r != NULL, "imap_utf7_decode: ASCII should not return NULL");
        ASSERT(strcmp(r, "INBOX") == 0, "ASCII passthrough mismatch");
    }

    /* Literal ampersand: "&-" → "&" */
    {
        RAII_STRING char *r = imap_utf7_decode("foo&-bar");
        ASSERT(r != NULL, "imap_utf7_decode: literal & should not return NULL");
        ASSERT(strcmp(r, "foo&bar") == 0, "Literal & decoding mismatch");
    }

    /* Single accented character: é = U+00E9 → "&AOk-" */
    {
        RAII_STRING char *r = imap_utf7_decode("&AOk-");
        ASSERT(r != NULL, "imap_utf7_decode: single char should not return NULL");
        ASSERT(strcmp(r, "\xC3\xA9") == 0, "é (U+00E9) decoding mismatch");
    }

    /* Hungarian pangram: árvíztűrőtükörfúrógép */
    {
        /* Modified UTF-7 encoded version of the pangram */
        const char *encoded =
            "&AOE-rv&AO0-zt&AXE-r&AVE-t&APw-k&APY-rf&APo-r&APM-g&AOk-p";
        RAII_STRING char *r = imap_utf7_decode(encoded);
        ASSERT(r != NULL, "imap_utf7_decode: pangram should not return NULL");
        ASSERT(strcmp(r, "\xC3\xA1"          /* á */
                        "rv"
                        "\xC3\xAD"           /* í */
                        "zt"
                        "\xC5\xB1"           /* ű */
                        "r"
                        "\xC5\x91"           /* ő */
                        "t"
                        "\xC3\xBC"           /* ü */
                        "k"
                        "\xC3\xB6"           /* ö */
                        "rf"
                        "\xC3\xBA"           /* ú */
                        "r"
                        "\xC3\xB3"           /* ó */
                        "g"
                        "\xC3\xA9"           /* é */
                        "p") == 0,
               "Hungarian pangram decoding mismatch");
    }

    /* Mixed ASCII and encoded segments (folder path) */
    {
        RAII_STRING char *r = imap_utf7_decode("INBOX.&AOk-rtes&AO0-t&AOk-s");
        ASSERT(r != NULL, "imap_utf7_decode: mixed path should not return NULL");
        ASSERT(strcmp(r, "INBOX."
                         "\xC3\xA9"   /* é */
                         "rtes"
                         "\xC3\xAD"   /* í */
                         "t"
                         "\xC3\xA9"   /* é */
                         "s") == 0,
               "Mixed folder path decoding mismatch");
    }

    /* Empty string */
    {
        RAII_STRING char *r = imap_utf7_decode("");
        ASSERT(r != NULL, "imap_utf7_decode: empty string should not return NULL");
        ASSERT(strcmp(r, "") == 0, "Empty string result mismatch");
    }

    /* NULL input */
    {
        char *r = imap_utf7_decode(NULL);
        ASSERT(r == NULL, "imap_utf7_decode: NULL input should return NULL");
    }

    /* mod64 '+' (62) and ',' (63) characters: U+FBF0 → "&+,A-" */
    {
        /* UTF-16BE: FB F0 → 6-bit groups: 111110(+) 111111(,) 000000(A) */
        RAII_STRING char *r = imap_utf7_decode("&+,A-");
        ASSERT(r != NULL, "imap_utf7_decode: '+' ',' in base64 should not return NULL");
        ASSERT(strcmp(r, "\xEF\xAF\xB0") == 0, "U+FBF0 via '+' and ',' decoding mismatch");
    }

    /* Invalid character in base64 run: covers mod64_value() return -1 path */
    {
        RAII_STRING char *r = imap_utf7_decode("&!-");
        ASSERT(r != NULL, "imap_utf7_decode: invalid base64 char should not return NULL");
        ASSERT(strcmp(r, "") == 0, "Invalid base64 segment should produce empty output");
    }

    /* ASCII codepoint via encoded path: U+0041 'A' → "&AEE-"
     * Tests utf8_encode() cp < 0x80 branch (1-byte output). */
    {
        /* UTF-16BE: 00 41 → base64: A(0) E(4) E(4), decodes to 0x00 0x41 = U+0041 */
        RAII_STRING char *r = imap_utf7_decode("&AEE-");
        ASSERT(r != NULL, "imap_utf7_decode: ASCII-via-encoding should not return NULL");
        ASSERT(strcmp(r, "A") == 0, "U+0041 via encoded path mismatch");
    }

    /* CJK 3-byte UTF-8: U+4E2D (中) → "&Ti0-"
     * Tests utf8_encode() 0x800 <= cp < 0x10000 branch. */
    {
        /* UTF-16BE: 4E 2D → base64: T(19) i(34) 0(52) */
        RAII_STRING char *r = imap_utf7_decode("&Ti0-");
        ASSERT(r != NULL, "imap_utf7_decode: CJK should not return NULL");
        ASSERT(strcmp(r, "\xE4\xB8\xAD") == 0, "U+4E2D (middle) CJK decoding mismatch");
    }

    /* UTF-16BE surrogate pair: U+10000 (𐀀) → "&2ADcAA-"
     * Tests utf8_encode() cp >= 0x10000 branch and surrogate-pair reassembly. */
    {
        /* High=0xD800, Low=0xDC00; UTF-16BE: D8 00 DC 00 → 2ADcAA */
        RAII_STRING char *r = imap_utf7_decode("&2ADcAA-");
        ASSERT(r != NULL, "imap_utf7_decode: surrogate pair should not return NULL");
        ASSERT(strcmp(r, "\xF0\x90\x80\x80") == 0, "U+10000 surrogate pair decoding mismatch");
    }

    /* Unpaired high surrogate followed by non-surrogate: covers cp=unit pass-through path */
    {
        /* UTF-16BE: D8 00 (high surrogate) 00 41 (U+0041, not a low surrogate)
         * → base64: 2AAAQQ; result is U+D800 (invalid UTF-8) + 'A' */
        RAII_STRING char *r = imap_utf7_decode("&2AAAQQ-");
        ASSERT(r != NULL, "imap_utf7_decode: unpaired high surrogate must not crash");
        ASSERT(strcmp(r, "\xED\xA0\x80" "A") == 0,
               "Unpaired high surrogate pass-through mismatch");
    }

    /* ── imap_utf7_encode tests ──────────────────────────────────────────── */

    /* NULL input → NULL */
    {
        char *r = imap_utf7_encode(NULL);
        ASSERT(r == NULL, "imap_utf7_encode: NULL input returns NULL");
    }

    /* Empty string → empty string */
    {
        RAII_STRING char *r = imap_utf7_encode("");
        ASSERT(r != NULL, "imap_utf7_encode: empty string must not return NULL");
        ASSERT(strcmp(r, "") == 0, "imap_utf7_encode: empty string round-trip");
    }

    /* Pure ASCII passthrough (no encoding needed) */
    {
        RAII_STRING char *r = imap_utf7_encode("INBOX");
        ASSERT(r != NULL, "imap_utf7_encode: ASCII must not return NULL");
        ASSERT(strcmp(r, "INBOX") == 0, "imap_utf7_encode: ASCII passthrough");
    }

    /* Literal '&' → "&-" */
    {
        RAII_STRING char *r = imap_utf7_encode("foo&bar");
        ASSERT(r != NULL, "imap_utf7_encode: '&' must not return NULL");
        ASSERT(strcmp(r, "foo&-bar") == 0, "imap_utf7_encode: literal & escaping");
    }

    /* Roundtrip: encode then decode → original UTF-8 string */
    /* é (U+00E9, 2-byte UTF-8) */
    {
        RAII_STRING char *encoded = imap_utf7_encode("\xC3\xA9");
        ASSERT(encoded != NULL, "imap_utf7_encode: é must not return NULL");
        RAII_STRING char *decoded = imap_utf7_decode(encoded);
        ASSERT(decoded != NULL, "imap_utf7_encode/decode: é roundtrip must not be NULL");
        ASSERT(strcmp(decoded, "\xC3\xA9") == 0, "imap_utf7_encode/decode: é roundtrip");
    }

    /* CJK U+4E2D (中, 3-byte UTF-8) roundtrip */
    {
        RAII_STRING char *encoded = imap_utf7_encode("\xE4\xB8\xAD");
        ASSERT(encoded != NULL, "imap_utf7_encode: CJK must not return NULL");
        RAII_STRING char *decoded = imap_utf7_decode(encoded);
        ASSERT(decoded != NULL, "imap_utf7_encode/decode: CJK roundtrip must not be NULL");
        ASSERT(strcmp(decoded, "\xE4\xB8\xAD") == 0, "imap_utf7_encode/decode: CJK roundtrip");
    }

    /* Supplementary plane U+10000 (4-byte UTF-8) roundtrip (surrogate pair) */
    {
        RAII_STRING char *encoded = imap_utf7_encode("\xF0\x90\x80\x80");
        ASSERT(encoded != NULL, "imap_utf7_encode: U+10000 must not return NULL");
        RAII_STRING char *decoded = imap_utf7_decode(encoded);
        ASSERT(decoded != NULL, "imap_utf7_encode/decode: U+10000 roundtrip must not be NULL");
        ASSERT(strcmp(decoded, "\xF0\x90\x80\x80") == 0,
               "imap_utf7_encode/decode: U+10000 surrogate-pair roundtrip");
    }

    /* Mixed ASCII and non-ASCII (folder path with accented chars) */
    {
        /* "INBOX.étest" — é is U+00E9 */
        RAII_STRING char *encoded = imap_utf7_encode("INBOX.\xC3\xA9test");
        ASSERT(encoded != NULL, "imap_utf7_encode: mixed path must not return NULL");
        RAII_STRING char *decoded = imap_utf7_decode(encoded);
        ASSERT(decoded != NULL, "imap_utf7_encode/decode: mixed path must not be NULL");
        ASSERT(strcmp(decoded, "INBOX.\xC3\xA9test") == 0,
               "imap_utf7_encode/decode: mixed path roundtrip");
    }

    /* Multiple non-ASCII chars in a single run */
    {
        /* "éü" — two accented chars back to back, one encoded run */
        RAII_STRING char *encoded = imap_utf7_encode("\xC3\xA9\xC3\xBC");
        ASSERT(encoded != NULL, "imap_utf7_encode: two-char run must not return NULL");
        RAII_STRING char *decoded = imap_utf7_decode(encoded);
        ASSERT(decoded != NULL, "imap_utf7_encode/decode: two-char run roundtrip must not be NULL");
        ASSERT(strcmp(decoded, "\xC3\xA9\xC3\xBC") == 0,
               "imap_utf7_encode/decode: two-char run roundtrip");
    }

    /* Non-ASCII immediately followed by '&' (edge: run flush then '&' escape) */
    {
        /* "é&" */
        RAII_STRING char *encoded = imap_utf7_encode("\xC3\xA9&");
        ASSERT(encoded != NULL, "imap_utf7_encode: non-ASCII+'&' must not return NULL");
        RAII_STRING char *decoded = imap_utf7_decode(encoded);
        ASSERT(decoded != NULL,
               "imap_utf7_encode/decode: non-ASCII+'&' roundtrip must not be NULL");
        ASSERT(strcmp(decoded, "\xC3\xA9&") == 0,
               "imap_utf7_encode/decode: non-ASCII+'&' roundtrip");
    }

    /* Invalid UTF-8 continuation byte at start (0x80) → replaced with U+FFFD */
    {
        /* 0x80 is a bare continuation byte — decoder maps it to U+FFFD */
        RAII_STRING char *encoded = imap_utf7_encode("\x80");
        ASSERT(encoded != NULL, "imap_utf7_encode: invalid UTF-8 must not return NULL");
        /* encoded should be a non-ASCII run; decode it back and compare with U+FFFD */
        RAII_STRING char *decoded = imap_utf7_decode(encoded);
        ASSERT(decoded != NULL,
               "imap_utf7_encode/decode: invalid UTF-8 decoded must not be NULL");
        /* U+FFFD in UTF-8 is EF BF BD */
        ASSERT(strcmp(decoded, "\xEF\xBF\xBD") == 0,
               "imap_utf7_encode: invalid UTF-8 byte encodes as U+FFFD");
    }

    /* Known encode value: é → "&AOk-" (same as decode test, verify direction) */
    {
        RAII_STRING char *r = imap_utf7_encode("\xC3\xA9");
        ASSERT(r != NULL, "imap_utf7_encode: é known value must not be NULL");
        ASSERT(strcmp(r, "&AOk-") == 0, "imap_utf7_encode: é encodes to &AOk-");
    }

    /* Known encode value: ASCII 'A' is passed through (not encoded) */
    {
        RAII_STRING char *r = imap_utf7_encode("A");
        ASSERT(r != NULL, "imap_utf7_encode: 'A' must not be NULL");
        ASSERT(strcmp(r, "A") == 0, "imap_utf7_encode: printable ASCII 'A' passthrough");
    }

    /* Printable ASCII boundaries: space (0x20) and tilde (0x7E) pass through */
    {
        RAII_STRING char *r = imap_utf7_encode(" ~");
        ASSERT(r != NULL, "imap_utf7_encode: space+tilde must not be NULL");
        ASSERT(strcmp(r, " ~") == 0, "imap_utf7_encode: space+tilde passthrough");
    }

    /* ── imap_uid_set_expand tests ──────────────────────────────────── */

    /* NULL input → count 0, NULL array */
    {
        char (*uids)[17] = NULL;
        int cnt = 0;
        int rc = imap_uid_set_expand(NULL, &uids, &cnt);
        ASSERT(rc == 0,     "uid_set_expand: NULL set rc=0");
        ASSERT(cnt == 0,    "uid_set_expand: NULL set count=0");
        ASSERT(uids == NULL, "uid_set_expand: NULL set array=NULL");
    }

    /* Empty string → count 0 */
    {
        char (*uids)[17] = NULL;
        int cnt = 0;
        int rc = imap_uid_set_expand("", &uids, &cnt);
        ASSERT(rc == 0,  "uid_set_expand: empty set rc=0");
        ASSERT(cnt == 0, "uid_set_expand: empty set count=0");
        free(uids);
    }

    /* Single UID */
    {
        char (*uids)[17] = NULL;
        int cnt = 0;
        int rc = imap_uid_set_expand("5", &uids, &cnt);
        ASSERT(rc == 0,   "uid_set_expand: single UID rc=0");
        ASSERT(cnt == 1,  "uid_set_expand: single UID count=1");
        ASSERT(strcmp(uids[0], "0000000000000005") == 0,
               "uid_set_expand: single UID value");
        free(uids);
    }

    /* Contiguous range 1:3 → 3 entries */
    {
        char (*uids)[17] = NULL;
        int cnt = 0;
        int rc = imap_uid_set_expand("1:3", &uids, &cnt);
        ASSERT(rc == 0,   "uid_set_expand: range rc=0");
        ASSERT(cnt == 3,  "uid_set_expand: range count=3");
        ASSERT(strcmp(uids[0], "0000000000000001") == 0, "uid_set_expand: range[0]");
        ASSERT(strcmp(uids[2], "0000000000000003") == 0, "uid_set_expand: range[2]");
        free(uids);
    }

    /* Multiple comma-separated UIDs */
    {
        char (*uids)[17] = NULL;
        int cnt = 0;
        int rc = imap_uid_set_expand("1,3,7", &uids, &cnt);
        ASSERT(rc == 0,   "uid_set_expand: multi-uid rc=0");
        ASSERT(cnt == 3,  "uid_set_expand: multi-uid count=3");
        ASSERT(strcmp(uids[1], "0000000000000003") == 0, "uid_set_expand: multi-uid[1]");
        free(uids);
    }

    /* Mix of single and range: "2,5:7" → 4 entries */
    {
        char (*uids)[17] = NULL;
        int cnt = 0;
        int rc = imap_uid_set_expand("2,5:7", &uids, &cnt);
        ASSERT(rc == 0,  "uid_set_expand: mixed rc=0");
        ASSERT(cnt == 4, "uid_set_expand: mixed count=4");
        free(uids);
    }

    /* Large range that triggers realloc (initial cap=32) */
    {
        char (*uids)[17] = NULL;
        int cnt = 0;
        int rc = imap_uid_set_expand("1:40", &uids, &cnt);
        ASSERT(rc == 0,   "uid_set_expand: large range rc=0");
        ASSERT(cnt == 40, "uid_set_expand: large range count=40");
        free(uids);
    }

    /* Non-numeric input → parser stops immediately → 0 UIDs */
    {
        char (*uids)[17] = NULL;
        int cnt = 0;
        int rc = imap_uid_set_expand("abc", &uids, &cnt);
        ASSERT(rc == 0,  "uid_set_expand: non-numeric rc=0");
        ASSERT(cnt == 0, "uid_set_expand: non-numeric count=0");
        free(uids);
    }

    /* Malformed range "5:" → treats hi=lo */
    {
        char (*uids)[17] = NULL;
        int cnt = 0;
        int rc = imap_uid_set_expand("5:", &uids, &cnt);
        ASSERT(rc == 0,  "uid_set_expand: malformed range rc=0");
        ASSERT(cnt == 1, "uid_set_expand: malformed range count=1");
        ASSERT(strcmp(uids[0], "0000000000000005") == 0,
               "uid_set_expand: malformed range value");
        free(uids);
    }
}
