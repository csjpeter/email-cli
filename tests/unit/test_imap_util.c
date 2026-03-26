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
}
