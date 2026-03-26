#include "test_helpers.h"
#include "mime_util.h"
#include "raii.h"
#include <string.h>
#include <stdlib.h>

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
}
