#include "test_helpers.h"
#include "mime_util.h"
#include "raii.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

void test_mime_util(void) {

    /* ── mime_get_header ───────────────────────────────────────────── */

    /* Basic header extraction */
    const char *msg1 =
        "From: Alice <alice@example.com>\r\n"
        "Subject: Hello World\r\n"
        "Date: Mon, 25 Mar 2026 10:00:00 +0000\r\n"
        "\r\n"
        "Body text here.";

    {
        RAII_STRING char *from = mime_get_header(msg1, "From");
        ASSERT(from != NULL, "mime_get_header: From should not be NULL");
        ASSERT(strcmp(from, "Alice <alice@example.com>") == 0, "From header value mismatch");
    }

    {
        RAII_STRING char *subject = mime_get_header(msg1, "Subject");
        ASSERT(subject != NULL, "mime_get_header: Subject should not be NULL");
        ASSERT(strcmp(subject, "Hello World") == 0, "Subject header value mismatch");
    }

    /* Header folding */
    {
        const char *folded =
            "Subject: This is a very\r\n"
            " long subject\r\n"
            "\r\n";
        RAII_STRING char *subj = mime_get_header(folded, "Subject");
        ASSERT(subj != NULL, "Folded subject should not be NULL");
        ASSERT(strcmp(subj, "This is a very long subject") == 0, "Folded subject mismatch");
    }

    /* Case-insensitive lookup */
    {
        RAII_STRING char *date = mime_get_header(msg1, "date");
        ASSERT(date != NULL, "mime_get_header: case-insensitive lookup should work");
    }

    /* Missing header returns NULL */
    {
        RAII_STRING char *cc = mime_get_header(msg1, "Cc");
        ASSERT(cc == NULL, "mime_get_header: missing header should return NULL");
    }

    /* NULL inputs */
    ASSERT(mime_get_header(NULL, "From") == NULL, "NULL msg should return NULL");
    ASSERT(mime_get_header(msg1, NULL) == NULL, "NULL name should return NULL");

    /* Headers stop at blank line — body should not be searched */
    {
        const char *msg2 =
            "Subject: Real\r\n"
            "\r\n"
            "Fake: Header\r\n";
        RAII_STRING char *fake = mime_get_header(msg2, "Fake");
        ASSERT(fake == NULL, "mime_get_header: should not find headers in body");
    }

    /* ── mime_get_text_body — plain text ───────────────────────────── */

    {
        const char *plain =
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Simple body.";
        RAII_STRING char *body = mime_get_text_body(plain);
        ASSERT(body != NULL, "mime_get_text_body: plain should not be NULL");
        ASSERT(strstr(body, "Simple body.") != NULL, "Plain body content mismatch");
    }

    /* No Content-Type defaults to text/plain */
    {
        const char *no_ct =
            "Subject: test\r\n"
            "\r\n"
            "Hello!";
        RAII_STRING char *body = mime_get_text_body(no_ct);
        ASSERT(body != NULL, "mime_get_text_body: no Content-Type should default to plain");
        ASSERT(strstr(body, "Hello!") != NULL, "Default plain body content mismatch");
    }

    /* ── mime_get_text_body — base64 ───────────────────────────────── */

    /* "Hello, base64!" base64-encoded */
    {
        const char *b64msg =
            "Content-Type: text/plain\r\n"
            "Content-Transfer-Encoding: base64\r\n"
            "\r\n"
            "SGVsbG8sIGJhc2U2NCE=\r\n";
        RAII_STRING char *body = mime_get_text_body(b64msg);
        ASSERT(body != NULL, "mime_get_text_body: base64 should not be NULL");
        ASSERT(strstr(body, "Hello, base64!") != NULL, "Base64 decoded content mismatch");
    }

    /* ── mime_get_text_body — quoted-printable ─────────────────────── */

    {
        const char *qpmsg =
            "Content-Type: text/plain\r\n"
            "Content-Transfer-Encoding: quoted-printable\r\n"
            "\r\n"
            "Hello=2C=20QP=21\r\n";  /* "Hello, QP!" */
        RAII_STRING char *body = mime_get_text_body(qpmsg);
        ASSERT(body != NULL, "mime_get_text_body: QP should not be NULL");
        ASSERT(strstr(body, "Hello, QP!") != NULL, "QP decoded content mismatch");
    }

    /* ── mime_get_text_body — HTML fallback ────────────────────────── */

    {
        const char *html =
            "Content-Type: text/html\r\n"
            "\r\n"
            "<html><body><p>HTML body</p></body></html>";
        RAII_STRING char *body = mime_get_text_body(html);
        ASSERT(body != NULL, "mime_get_text_body: HTML fallback should not be NULL");
        ASSERT(strstr(body, "HTML body") != NULL, "HTML stripped content mismatch");
    }

    /* ── mime_get_text_body — multipart ────────────────────────────── */

    {
        const char *mp =
            "Content-Type: multipart/mixed; boundary=\"BOUND\"\r\n"
            "\r\n"
            "--BOUND\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Multipart plain text\r\n"
            "--BOUND--\r\n";
        RAII_STRING char *body = mime_get_text_body(mp);
        ASSERT(body != NULL, "mime_get_text_body: multipart should not be NULL");
        ASSERT(strstr(body, "Multipart plain text") != NULL, "Multipart body content mismatch");
    }

    /* NULL input */
    ASSERT(mime_get_text_body(NULL) == NULL, "NULL msg should return NULL");

    /* ── mime_decode_words ──────────────────────────────────────────── */

    /* Plain string — no encoded words, returned verbatim */
    {
        RAII_STRING char *r = mime_decode_words("Hello World");
        ASSERT(r != NULL, "mime_decode_words: plain should not be NULL");
        ASSERT(strcmp(r, "Hello World") == 0, "plain string should be unchanged");
    }

    /* UTF-8 Q-encoding: Bí-Bor-Ász Kft. - Borászati Szaküzlet */
    {
        RAII_STRING char *r = mime_decode_words(
            "=?utf-8?Q?B=C3=AD-Bor-=C3=81sz_Kft=2E_-_Bor=C3=A1szati"
            "_Szak=C3=BCzlet?=");
        ASSERT(r != NULL, "mime_decode_words: Q UTF-8 should not be NULL");
        ASSERT(strcmp(r, "B\xc3\xad-Bor-\xc3\x81sz Kft. - Bor\xc3\xa1szati"
                         " Szak\xc3\xbczlet") == 0,
               "Q UTF-8 decode mismatch");
    }

    /* UTF-8 B-encoding: "Hello" → base64 "SGVsbG8=" */
    {
        RAII_STRING char *r = mime_decode_words("=?utf-8?B?SGVsbG8=?=");
        ASSERT(r != NULL, "mime_decode_words: B UTF-8 should not be NULL");
        ASSERT(strcmp(r, "Hello") == 0, "B UTF-8 decode mismatch");
    }

    /* Multiple encoded words: whitespace between them must be stripped */
    {
        RAII_STRING char *r = mime_decode_words(
            "=?utf-8?Q?foo?= =?utf-8?Q?bar?=");
        ASSERT(r != NULL, "mime_decode_words: multi-word should not be NULL");
        ASSERT(strcmp(r, "foobar") == 0,
               "whitespace between encoded words should be stripped");
    }

    /* Mixed: encoded word followed by literal suffix */
    {
        RAII_STRING char *r = mime_decode_words(
            "=?utf-8?Q?Hello?= <user@example.com>");
        ASSERT(r != NULL, "mime_decode_words: mixed should not be NULL");
        ASSERT(strcmp(r, "Hello <user@example.com>") == 0,
               "mixed encoded + literal mismatch");
    }

    /* NULL input */
    ASSERT(mime_decode_words(NULL) == NULL,
           "mime_decode_words: NULL input should return NULL");

    /* ── mime_extract_imap_literal ──────────────────────────────────── */

    {
        const char *imap_resp =
            "* 1 FETCH (BODY[HEADER] {23}\r\n"
            "Subject: Test\r\n"
            "\r\n"
            ")\r\n"
            "A1 OK FETCH completed\r\n";
        RAII_STRING char *lit = mime_extract_imap_literal(imap_resp);
        ASSERT(lit != NULL, "mime_extract_imap_literal: should not be NULL");
        ASSERT(strncmp(lit, "Subject: Test", 13) == 0, "Literal content mismatch");
    }

    /* No literal in response */
    {
        RAII_STRING char *no_lit = mime_extract_imap_literal("* OK no literal here\r\n");
        ASSERT(no_lit == NULL, "No literal should return NULL");
    }

    /* NULL input */
    ASSERT(mime_extract_imap_literal(NULL) == NULL,
           "NULL response should return NULL");

    /* ── mime_format_date ───────────────────────────────────────────── */

    /* Force UTC so the expected output is timezone-independent. */
    const char *saved_tz = getenv("TZ");
    setenv("TZ", "UTC", 1);
    tzset();

    /* Standard RFC 2822 with weekday, UTC offset */
    {
        RAII_STRING char *r = mime_format_date("Tue, 10 Mar 2026 15:07:40 +0000");
        ASSERT(r != NULL, "mime_format_date: should not return NULL");
        ASSERT(strcmp(r, "2026-03-10 15:07") == 0, "UTC date format mismatch");
    }

    /* Date with +0100 offset: local (UTC) output should subtract 1 hour */
    {
        RAII_STRING char *r = mime_format_date("Thu, 26 Mar 2026 12:00:00 +0100");
        ASSERT(r != NULL, "mime_format_date: offset date should not return NULL");
        ASSERT(strcmp(r, "2026-03-26 11:00") == 0, "Offset date format mismatch");
    }

    /* Trailing timezone comment in parentheses */
    {
        RAII_STRING char *r = mime_format_date("Mon, 1 Jan 2026 00:00:00 +0000 (UTC)");
        ASSERT(r != NULL, "mime_format_date: comment date should not return NULL");
        ASSERT(strcmp(r, "2026-01-01 00:00") == 0, "Date with comment format mismatch");
    }

    /* Without day-of-week */
    {
        RAII_STRING char *r = mime_format_date("1 Jan 2026 10:30:00 +0000");
        ASSERT(r != NULL, "mime_format_date: no-weekday date should not return NULL");
        ASSERT(strcmp(r, "2026-01-01 10:30") == 0, "No-weekday date format mismatch");
    }

    /* Timezone name instead of numeric offset */
    {
        RAII_STRING char *r = mime_format_date("Tue, 24 Mar 2026 16:38:21 GMT");
        ASSERT(r != NULL, "mime_format_date: GMT date should not return NULL");
        ASSERT(strcmp(r, "2026-03-24 16:38") == 0, "GMT timezone date format mismatch");
    }

    /* Unparseable input: returns a copy of the raw string */
    {
        RAII_STRING char *r = mime_format_date("not a date");
        ASSERT(r != NULL, "mime_format_date: bad date should return raw copy");
        ASSERT(strcmp(r, "not a date") == 0, "Bad date should return raw input");
    }

    /* NULL input */
    ASSERT(mime_format_date(NULL) == NULL, "mime_format_date: NULL should return NULL");

    /* Restore original TZ */
    if (saved_tz)
        setenv("TZ", saved_tz, 1);
    else
        unsetenv("TZ");
    tzset();

    /* ── QP soft line break (=\r\n) ────────────────────────────────── */

    {
        /* "=" followed by \r\n is a soft break: the line ending is removed */
        const char *qp_soft =
            "Content-Type: text/plain\r\n"
            "Content-Transfer-Encoding: quoted-printable\r\n"
            "\r\n"
            "First=\r\n"
            "Second";
        RAII_STRING char *body = mime_get_text_body(qp_soft);
        ASSERT(body != NULL, "mime_get_text_body: QP soft break should not return NULL");
        ASSERT(strstr(body, "FirstSecond") != NULL,
               "QP soft line break should be removed");
    }

    /* ── body_start: LF-only separator ─────────────────────────────── */

    {
        /* Message with \n\n instead of \r\n\r\n as header/body separator */
        const char *lf_msg =
            "Content-Type: text/plain\n"
            "\n"
            "LF-only body";
        RAII_STRING char *body = mime_get_text_body(lf_msg);
        ASSERT(body != NULL, "mime_get_text_body: LF-only separator should work");
        ASSERT(strstr(body, "LF-only body") != NULL, "LF-only body content mismatch");
    }

    /* ── body_start: no separator → returns NULL ────────────────────── */

    {
        /* No blank line at all → body_start() returns NULL */
        RAII_STRING char *body = mime_get_text_body("Subject: no body");
        ASSERT(body == NULL,
               "mime_get_text_body: message without body separator should return NULL");
    }

    /* ── extract_charset: unquoted value ────────────────────────────── */

    {
        const char *ct_plain =
            "Content-Type: text/plain; charset=utf-8\r\n"
            "\r\n"
            "explicit UTF-8";
        RAII_STRING char *body = mime_get_text_body(ct_plain);
        ASSERT(body != NULL, "mime_get_text_body: unquoted charset=utf-8 should work");
        ASSERT(strstr(body, "explicit UTF-8") != NULL,
               "unquoted charset body mismatch");
    }

    /* ── extract_charset: quoted value ──────────────────────────────── */

    {
        const char *ct_quoted =
            "Content-Type: text/plain; charset=\"utf-8\"\r\n"
            "\r\n"
            "quoted charset";
        RAII_STRING char *body = mime_get_text_body(ct_quoted);
        ASSERT(body != NULL, "mime_get_text_body: quoted charset should work");
        ASSERT(strstr(body, "quoted charset") != NULL,
               "quoted charset body mismatch");
    }

    /* ── extract_charset: empty quoted value → NULL (p == start) ────── */

    {
        /* charset="" → extract_charset returns NULL → charset_to_utf8 is
         * called with NULL charset and simply returns strdup(body). */
        const char *ct_empty =
            "Content-Type: text/plain; charset=\"\"\r\n"
            "\r\n"
            "empty charset";
        RAII_STRING char *body = mime_get_text_body(ct_empty);
        ASSERT(body != NULL, "mime_get_text_body: empty charset should not crash");
        ASSERT(strstr(body, "empty charset") != NULL,
               "empty charset body mismatch");
    }

    /* ── charset_to_utf8: ISO-8859-1 body via iconv ──────────────────── */

    {
        /* \xE9 = 'é' in ISO-8859-1; UTF-8 encoding: \xC3\xA9 */
        const char *iso_msg =
            "Content-Type: text/plain; charset=iso-8859-1\r\n"
            "\r\n"
            "\xE9t\xE9";   /* "été" in ISO-8859-1 */
        RAII_STRING char *body = mime_get_text_body(iso_msg);
        ASSERT(body != NULL,
               "mime_get_text_body: iso-8859-1 should not return NULL");
        ASSERT(strstr(body, "\xC3\xA9t\xC3\xA9") != NULL,
               "ISO-8859-1 to UTF-8 body conversion mismatch");
    }

    /* ── text_from_multipart: unquoted boundary ──────────────────────── */

    {
        const char *mp_unquoted =
            "Content-Type: multipart/mixed; boundary=NOBOUND\r\n"
            "\r\n"
            "--NOBOUND\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Unquoted boundary text\r\n"
            "--NOBOUND--\r\n";
        RAII_STRING char *body = mime_get_text_body(mp_unquoted);
        ASSERT(body != NULL,
               "mime_get_text_body: unquoted boundary should work");
        ASSERT(strstr(body, "Unquoted boundary text") != NULL,
               "Unquoted boundary multipart content mismatch");
    }

    /* ── text_from_multipart: two non-text parts → exercises loop-continue
     *    path (lines 256-259) and closing-boundary break (line 257 final),
     *    then returns NULL (line 261). ──────────────────────────────────── */

    {
        /* Both parts are application/octet-stream → text_from_part returns NULL
         * for each.  After the first part the loop continues (lines 258-259),
         * then the second delimiter turns out to be the closing "--B3--" →
         * break → return NULL. */
        const char *mp_none =
            "Content-Type: multipart/mixed; boundary=B3\r\n"
            "\r\n"
            "--B3\r\n"
            "Content-Type: application/octet-stream\r\n"
            "\r\n"
            "binary1\r\n"
            "--B3\r\n"
            "Content-Type: application/octet-stream\r\n"
            "\r\n"
            "binary2\r\n"
            "--B3--\r\n";
        RAII_STRING char *body = mime_get_text_body(mp_none);
        ASSERT(body == NULL,
               "mime_get_text_body: all-binary multipart should return NULL");
    }

    /* ── mime_decode_words: ISO-8859-1 encoded word via iconv ─────────── */

    {
        /* "été": \xE9=é, \xE9=é in ISO-8859-1 Q-encoding */
        RAII_STRING char *r = mime_decode_words("=?iso-8859-1?Q?=E9t=E9?=");
        ASSERT(r != NULL,
               "mime_decode_words: iso-8859-1 word should not return NULL");
        ASSERT(strcmp(r, "\xC3\xA9t\xC3\xA9") == 0,
               "ISO-8859-1 Q-encoded word UTF-8 decode mismatch");
    }

    /* ── mime_decode_words: unknown charset → raw bytes fallback ─────── */

    {
        /* iconv_open fails for unknown charset: raw decoded bytes returned */
        RAII_STRING char *r = mime_decode_words("=?x-unknown-charset?Q?hello?=");
        ASSERT(r != NULL,
               "mime_decode_words: unknown charset should not return NULL");
        ASSERT(strcmp(r, "hello") == 0,
               "Unknown charset encoded word should pass through raw bytes");
    }

    /* ── mime_extract_imap_literal: content shorter than claimed size ── */

    {
        /* {100} claims 100 bytes but only 5 are present */
        const char *trunc = "* FETCH {100}\r\nhello";
        RAII_STRING char *lit = mime_extract_imap_literal(trunc);
        ASSERT(lit != NULL,
               "mime_extract_imap_literal: truncated should not return NULL");
        ASSERT(strcmp(lit, "hello") == 0,
               "Truncated literal should return all available bytes");
    }

    /* ── mime_get_header: long value triggers realloc (>512 bytes) ───── */

    {
        /* 580 Z's exceed the initial 512-byte buffer → realloc required */
        char big_msg[700];
        strcpy(big_msg, "X-Big: ");
        memset(big_msg + 7, 'Z', 580);
        strcpy(big_msg + 587, "\r\n\r\n");
        RAII_STRING char *val = mime_get_header(big_msg, "X-Big");
        ASSERT(val != NULL,
               "mime_get_header: 580-char value should not return NULL");
        ASSERT(strlen(val) == 580, "Long header value length mismatch");
    }

    /* ── mime_get_header: folded long value triggers realloc ─────────── */

    {
        /* 511 A's fill the buffer, then a folded continuation adds space+X.
         * When the fold handler tries to add the separator space, n+1==512==cap
         * → realloc is triggered inside the fold branch. */
        char fold_msg[700];
        strcpy(fold_msg, "X-Fold: ");       /* 8 chars */
        memset(fold_msg + 8, 'A', 511);     /* 511 A's */
        strcpy(fold_msg + 519, "\r\n X\r\n\r\n");
        RAII_STRING char *val = mime_get_header(fold_msg, "X-Fold");
        ASSERT(val != NULL,
               "mime_get_header: folded long value should not return NULL");
        ASSERT(strlen(val) == 513,
               "Folded long header value length mismatch (511 A + space + X)");
    }

    /* ── mime_get_html_part ─────────────────────────────────────────── */

    /* HTML-only message */
    {
        const char *html_msg =
            "Content-Type: text/html\r\n"
            "\r\n"
            "<html><body><b>Bold</b></body></html>";
        RAII_STRING char *html = mime_get_html_part(html_msg);
        ASSERT(html != NULL, "mime_get_html_part: html-only should not be NULL");
        ASSERT(strstr(html, "<b>Bold</b>") != NULL,
               "mime_get_html_part: html content present");
    }

    /* Plain-only message → NULL */
    {
        const char *plain_msg =
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Plain only";
        RAII_STRING char *html = mime_get_html_part(plain_msg);
        ASSERT(html == NULL, "mime_get_html_part: plain-only should return NULL");
    }

    /* NULL input → NULL */
    ASSERT(mime_get_html_part(NULL) == NULL,
           "mime_get_html_part: NULL should return NULL");

    /* multipart/alternative with html part */
    {
        const char *alt_msg =
            "Content-Type: multipart/alternative; boundary=\"ALT\"\r\n"
            "\r\n"
            "--ALT\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Plain fallback\r\n"
            "--ALT\r\n"
            "Content-Type: text/html\r\n"
            "\r\n"
            "<p>HTML part</p>\r\n"
            "--ALT--\r\n";
        RAII_STRING char *html = mime_get_html_part(alt_msg);
        ASSERT(html != NULL, "mime_get_html_part: multipart/alt html should not be NULL");
        ASSERT(strstr(html, "<p>HTML part</p>") != NULL,
               "mime_get_html_part: multipart html content present");
    }

    /* multipart with unquoted boundary (covers html_from_multipart unquoted path) */
    {
        const char *unquoted_msg =
            "Content-Type: multipart/alternative; boundary=UNQUOTED\r\n"
            "\r\n"
            "--UNQUOTED\r\n"
            "Content-Type: text/html\r\n"
            "\r\n"
            "<b>unquoted</b>\r\n"
            "--UNQUOTED--\r\n";
        RAII_STRING char *html = mime_get_html_part(unquoted_msg);
        ASSERT(html != NULL, "mime_get_html_part: unquoted boundary not NULL");
        ASSERT(strstr(html, "<b>unquoted</b>") != NULL,
               "mime_get_html_part: unquoted boundary content present");
    }

    /* multipart with no HTML parts → NULL (covers html_from_multipart return NULL) */
    {
        const char *no_html_msg =
            "Content-Type: multipart/mixed; boundary=\"NOHTML\"\r\n"
            "\r\n"
            "--NOHTML\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "plain only\r\n"
            "--NOHTML--\r\n";
        RAII_STRING char *html = mime_get_html_part(no_html_msg);
        ASSERT(html == NULL, "mime_get_html_part: no-html multipart should return NULL");
    }
}
