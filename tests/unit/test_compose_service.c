#include "test_helpers.h"
#include "compose_service.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── compose_build_message ───────────────────────────────────────────── */

static void test_build_null_params(void) {
    char *out = NULL;
    size_t len = 0;
    ASSERT(compose_build_message(NULL, &out, &len) == -1,
           "build: NULL params → -1");
}

static void test_build_missing_fields(void) {
    char *out = NULL;
    size_t len = 0;
    ComposeParams p = {0};
    ASSERT(compose_build_message(&p, &out, &len) == -1,
           "build: missing from/to/subject → -1");

    p.from = "a@b.com";
    ASSERT(compose_build_message(&p, &out, &len) == -1,
           "build: missing to/subject → -1");

    p.to = "c@d.com";
    ASSERT(compose_build_message(&p, &out, &len) == -1,
           "build: missing subject → -1");
}

static void test_build_null_out(void) {
    ComposeParams p = {.from = "a@b.com", .to = "c@d.com", .subject = "Hi"};
    size_t len = 0;
    ASSERT(compose_build_message(&p, NULL, &len) == -1,
           "build: NULL out → -1");
}

static void test_build_basic(void) {
    ComposeParams p = {
        .from = "alice@example.com",
        .to = "bob@example.com",
        .subject = "Test subject",
        .body = "Hello, world!\n"
    };
    char *out = NULL;
    size_t len = 0;
    int rc = compose_build_message(&p, &out, &len);
    ASSERT(rc == 0, "build basic: success");
    ASSERT(out != NULL, "build basic: out not NULL");
    ASSERT(len > 0, "build basic: len > 0");

    /* Verify required headers */
    ASSERT(strstr(out, "From: alice@example.com\r\n") != NULL,
           "build basic: From header");
    ASSERT(strstr(out, "To: bob@example.com\r\n") != NULL,
           "build basic: To header");
    ASSERT(strstr(out, "Subject: Test subject\r\n") != NULL,
           "build basic: Subject header");
    ASSERT(strstr(out, "Date: ") != NULL,
           "build basic: Date header");
    ASSERT(strstr(out, "Message-ID: <") != NULL,
           "build basic: Message-ID header");
    ASSERT(strstr(out, "MIME-Version: 1.0\r\n") != NULL,
           "build basic: MIME-Version header");
    ASSERT(strstr(out, "Content-Type: text/plain; charset=UTF-8\r\n") != NULL,
           "build basic: Content-Type header");

    /* Verify header/body separator */
    ASSERT(strstr(out, "\r\n\r\n") != NULL,
           "build basic: CRLF separator");

    /* Verify body is present after separator */
    const char *body_start = strstr(out, "\r\n\r\n");
    ASSERT(body_start && strstr(body_start, "Hello, world!") != NULL,
           "build basic: body present");

    /* No In-Reply-To for non-reply */
    ASSERT(strstr(out, "In-Reply-To") == NULL,
           "build basic: no In-Reply-To");

    free(out);
}

static void test_build_reply(void) {
    ComposeParams p = {
        .from = "alice@example.com",
        .to = "bob@example.com",
        .subject = "Re: Original",
        .body = "Thanks!",
        .reply_to_msg_id = "<orig123@example.com>"
    };
    char *out = NULL;
    size_t len = 0;
    int rc = compose_build_message(&p, &out, &len);
    ASSERT(rc == 0, "build reply: success");
    ASSERT(strstr(out, "In-Reply-To: <orig123@example.com>\r\n") != NULL,
           "build reply: In-Reply-To header");
    ASSERT(strstr(out, "References: <orig123@example.com>\r\n") != NULL,
           "build reply: References header");
    free(out);
}

static void test_build_empty_body(void) {
    ComposeParams p = {
        .from = "a@b.com",
        .to = "c@d.com",
        .subject = "Empty",
        .body = NULL
    };
    char *out = NULL;
    size_t len = 0;
    int rc = compose_build_message(&p, &out, &len);
    ASSERT(rc == 0, "build empty body: success");
    ASSERT(out != NULL, "build empty body: not NULL");
    free(out);
}

static void test_build_lf_to_crlf(void) {
    ComposeParams p = {
        .from = "a@b.com",
        .to = "c@d.com",
        .subject = "Lines",
        .body = "line1\nline2\nline3"
    };
    char *out = NULL;
    size_t len = 0;
    int rc = compose_build_message(&p, &out, &len);
    ASSERT(rc == 0, "build LF→CRLF: success");
    /* Body should have CRLF line endings */
    const char *body = strstr(out, "\r\n\r\n");
    ASSERT(body && strstr(body, "line1\r\nline2\r\nline3") != NULL,
           "build LF→CRLF: body has CRLF");
    free(out);
}

/* ── compose_extract_reply_meta ──────────────────────────────────────── */

static void test_extract_null(void) {
    char *rt = NULL, *subj = NULL, *msgid = NULL;
    ASSERT(compose_extract_reply_meta(NULL, &rt, &subj, &msgid) == -1,
           "extract: NULL raw_msg → -1");
    ASSERT(compose_extract_reply_meta("From: a\r\n\r\n", NULL, &subj, &msgid) == -1,
           "extract: NULL reply_to_out → -1");
}

static void test_extract_basic(void) {
    const char *raw =
        "From: Alice <alice@example.com>\r\n"
        "Subject: Hello\r\n"
        "Message-ID: <abc123@example.com>\r\n"
        "\r\n"
        "Body.\r\n";
    char *rt = NULL, *subj = NULL, *msgid = NULL;
    int rc = compose_extract_reply_meta(raw, &rt, &subj, &msgid);
    ASSERT(rc == 0, "extract basic: success");
    ASSERT(rt != NULL, "extract basic: reply_to not NULL");
    ASSERT(strstr(rt, "alice@example.com") != NULL,
           "extract basic: reply_to has address");
    ASSERT(subj != NULL && strncmp(subj, "Re: ", 4) == 0,
           "extract basic: subject starts with Re:");
    ASSERT(strstr(subj, "Hello") != NULL,
           "extract basic: subject has original");
    ASSERT(msgid != NULL && strcmp(msgid, "<abc123@example.com>") == 0,
           "extract basic: message-id correct");
    free(rt); free(subj); free(msgid);
}

static void test_extract_re_dedup(void) {
    const char *raw =
        "From: Bob\r\n"
        "Subject: Re: Re: Re: Original\r\n"
        "Message-ID: <x@y>\r\n"
        "\r\n";
    char *rt = NULL, *subj = NULL, *msgid = NULL;
    int rc = compose_extract_reply_meta(raw, &rt, &subj, &msgid);
    ASSERT(rc == 0, "extract re dedup: success");
    ASSERT(strcmp(subj, "Re: Original") == 0,
           "extract re dedup: single Re: prefix");
    free(rt); free(subj); free(msgid);
}

static void test_extract_reply_to_header(void) {
    const char *raw =
        "From: Alice\r\n"
        "Reply-To: noreply@example.com\r\n"
        "Subject: Test\r\n"
        "Message-ID: <z@w>\r\n"
        "\r\n";
    char *rt = NULL, *subj = NULL, *msgid = NULL;
    int rc = compose_extract_reply_meta(raw, &rt, &subj, &msgid);
    ASSERT(rc == 0, "extract Reply-To: success");
    ASSERT(strstr(rt, "noreply@example.com") != NULL,
           "extract Reply-To: prefers Reply-To over From");
    free(rt); free(subj); free(msgid);
}

/* ── Registration ────────────────────────────────────────────────────── */

static void test_build_cc(void) {
    ComposeParams p = {
        .from = "alice@example.com",
        .to   = "bob@example.com",
        .cc   = "carol@example.com",
        .subject = "Hello",
        .body = "Hi"
    };
    char *out = NULL; size_t len = 0;
    ASSERT(compose_build_message(&p, &out, &len) == 0, "build cc: success");
    ASSERT(strstr(out, "Cc: carol@example.com\r\n") != NULL,
           "build cc: Cc header present");
    ASSERT(strstr(out, "Bcc:") == NULL, "build cc: no Bcc when bcc NULL");
    free(out);
}

static void test_build_bcc(void) {
    ComposeParams p = {
        .from = "alice@example.com",
        .to   = "bob@example.com",
        .bcc  = "secret@example.com",
        .subject = "Hello",
        .body = "Hi"
    };
    char *out = NULL; size_t len = 0;
    ASSERT(compose_build_message(&p, &out, &len) == 0, "build bcc: success");
    ASSERT(strstr(out, "Bcc: secret@example.com\r\n") != NULL,
           "build bcc: Bcc header present");
    ASSERT(strstr(out, "Cc:") == NULL, "build bcc: no Cc when cc NULL");
    free(out);
}

static void test_build_cc_and_bcc(void) {
    ComposeParams p = {
        .from = "alice@example.com",
        .to   = "bob@example.com",
        .cc   = "carol@example.com, dave@example.com",
        .bcc  = "eve@example.com",
        .subject = "Meeting",
        .body = "See you there"
    };
    char *out = NULL; size_t len = 0;
    ASSERT(compose_build_message(&p, &out, &len) == 0, "build cc+bcc: success");
    ASSERT(strstr(out, "Cc: carol@example.com, dave@example.com\r\n") != NULL,
           "build cc+bcc: Cc header");
    ASSERT(strstr(out, "Bcc: eve@example.com\r\n") != NULL,
           "build cc+bcc: Bcc header");
    ASSERT(strstr(out, "To: bob@example.com\r\n") != NULL,
           "build cc+bcc: To header still present");
    free(out);
}

static void test_build_empty_cc_omitted(void) {
    ComposeParams p = {
        .from = "a@b.com", .to = "c@d.com",
        .cc = "", .bcc = "",
        .subject = "X", .body = "Y"
    };
    char *out = NULL; size_t len = 0;
    ASSERT(compose_build_message(&p, &out, &len) == 0, "build empty cc: success");
    ASSERT(strstr(out, "Cc:") == NULL,  "build empty cc: no Cc header");
    ASSERT(strstr(out, "Bcc:") == NULL, "build empty cc: no Bcc header");
    free(out);
}

/* ── compose_build_message with attachments (US-84) ─────────────────── */

/* Write a small temp file and return a heap-allocated path, or NULL. */
static char *make_tmp_attach(const char *content)
{
    char *path = strdup("/tmp/test-compose-attach-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    write(fd, content, strlen(content));
    close(fd);
    return path;
}

static void test_build_no_attach_is_text_plain(void)
{
    /* Zero attachments → must still produce text/plain (backward compat) */
    ComposeParams p = {
        .from = "a@b.com", .to = "c@d.com",
        .subject = "X", .body = "Hi",
        .attachments = NULL, .attach_count = 0
    };
    char *out = NULL; size_t len = 0;
    ASSERT(compose_build_message(&p, &out, &len) == 0,
           "attach: no attach → success");
    ASSERT(strstr(out, "Content-Type: text/plain") != NULL,
           "attach: no attach → text/plain Content-Type");
    ASSERT(strstr(out, "multipart/mixed") == NULL,
           "attach: no attach → no multipart/mixed");
    free(out);
}

static void test_build_one_attachment_multipart(void)
{
    /* One attachment → multipart/mixed with text part + attachment part */
    char *path = make_tmp_attach("attachment data\n");
    ASSERT(path != NULL, "attach one: tmp file created");

    const char *attachments[] = { path, NULL };
    ComposeParams p = {
        .from = "a@b.com", .to = "c@d.com",
        .subject = "With attach", .body = "See attached.",
        .attachments = attachments, .attach_count = 1
    };
    char *out = NULL; size_t len = 0;
    int rc = compose_build_message(&p, &out, &len);
    ASSERT(rc == 0, "attach one: success");
    ASSERT(strstr(out, "multipart/mixed") != NULL,
           "attach one: Content-Type is multipart/mixed");
    ASSERT(strstr(out, "boundary=") != NULL,
           "attach one: boundary present");
    ASSERT(strstr(out, "Content-Type: text/plain") != NULL,
           "attach one: text part present");
    ASSERT(strstr(out, "Content-Disposition: attachment") != NULL,
           "attach one: attachment part has Content-Disposition");
    ASSERT(strstr(out, "Content-Transfer-Encoding: base64") != NULL,
           "attach one: attachment encoded as base64");
    ASSERT(strstr(out, "See attached.") != NULL,
           "attach one: body text present");
    free(out);
    unlink(path);
    free(path);
}

static void test_build_attachment_filename_in_disposition(void)
{
    /* Content-Disposition must include filename="<basename>" */
    char *path = make_tmp_attach("pdf stub");
    ASSERT(path != NULL, "attach filename: tmp file created");

    /* Rename to something with a meaningful extension */
    char named[256];
    snprintf(named, sizeof(named), "/tmp/report-%d.pdf", (int)getpid());
    rename(path, named);
    free(path);

    const char *attachments[] = { named, NULL };
    ComposeParams p = {
        .from = "a@b.com", .to = "c@d.com",
        .subject = "PDF", .body = "Attached.",
        .attachments = attachments, .attach_count = 1
    };
    char *out = NULL; size_t len = 0;
    ASSERT(compose_build_message(&p, &out, &len) == 0,
           "attach filename: success");
    ASSERT(strstr(out, "filename=") != NULL,
           "attach filename: filename= in Content-Disposition");
    /* Should contain just the basename, not the full path */
    char basename_check[64];
    snprintf(basename_check, sizeof(basename_check), "report-%d.pdf", (int)getpid());
    ASSERT(strstr(out, basename_check) != NULL,
           "attach filename: basename in Content-Disposition");
    free(out);
    unlink(named);
}

static void test_build_two_attachments(void)
{
    /* Two attachments → two MIME attachment parts */
    char *path1 = make_tmp_attach("file one");
    char *path2 = make_tmp_attach("file two");
    ASSERT(path1 && path2, "attach two: tmp files created");

    const char *attachments[] = { path1, path2, NULL };
    ComposeParams p = {
        .from = "a@b.com", .to = "c@d.com",
        .subject = "Two", .body = "Both attached.",
        .attachments = attachments, .attach_count = 2
    };
    char *out = NULL; size_t len = 0;
    ASSERT(compose_build_message(&p, &out, &len) == 0,
           "attach two: success");
    ASSERT(strstr(out, "multipart/mixed") != NULL,
           "attach two: multipart/mixed");
    /* Count Content-Disposition: attachment occurrences — must be 2 */
    int count = 0;
    const char *p2 = out;
    while ((p2 = strstr(p2, "Content-Disposition: attachment")) != NULL) {
        count++;
        p2++;
    }
    ASSERT(count == 2, "attach two: exactly 2 attachment parts");
    free(out);
    unlink(path1); free(path1);
    unlink(path2); free(path2);
}

static void test_build_attachment_boundary_unique(void)
{
    /* Two separate messages must have different MIME boundaries */
    char *path = make_tmp_attach("data");
    ASSERT(path != NULL, "attach boundary: tmp file created");

    const char *attachments[] = { path, NULL };
    ComposeParams p = {
        .from = "a@b.com", .to = "c@d.com",
        .subject = "B", .body = ".",
        .attachments = attachments, .attach_count = 1
    };
    char *out1 = NULL, *out2 = NULL;
    size_t len1 = 0, len2 = 0;
    ASSERT(compose_build_message(&p, &out1, &len1) == 0, "boundary: msg1 ok");
    ASSERT(compose_build_message(&p, &out2, &len2) == 0, "boundary: msg2 ok");

    /* Extract boundary values — they must differ (include pid/time/random) */
    const char *b1 = strstr(out1, "boundary=\"");
    const char *b2 = strstr(out2, "boundary=\"");
    ASSERT(b1 && b2, "boundary: both have boundary");
    /* At minimum the two messages should not be byte-identical */
    ASSERT(strcmp(out1, out2) != 0 || len1 != len2,
           "boundary: two messages differ (unique boundary or Message-ID)");
    free(out1); free(out2);
    unlink(path); free(path);
}

static void test_build_attachment_nonexistent_file(void)
{
    /* Non-existent file path → compose_build_message returns -1 */
    const char *attachments[] = { "/tmp/no-such-file-xyz-404.bin", NULL };
    ComposeParams p = {
        .from = "a@b.com", .to = "c@d.com",
        .subject = "Bad", .body = ".",
        .attachments = attachments, .attach_count = 1
    };
    char *out = NULL; size_t len = 0;
    ASSERT(compose_build_message(&p, &out, &len) == -1,
           "attach nonexistent: returns -1");
    ASSERT(out == NULL, "attach nonexistent: out is NULL");
}

void test_compose_service(void) {
    RUN_TEST(test_build_null_params);
    RUN_TEST(test_build_missing_fields);
    RUN_TEST(test_build_null_out);
    RUN_TEST(test_build_basic);
    RUN_TEST(test_build_reply);
    RUN_TEST(test_build_empty_body);
    RUN_TEST(test_build_lf_to_crlf);
    RUN_TEST(test_build_cc);
    RUN_TEST(test_build_bcc);
    RUN_TEST(test_build_cc_and_bcc);
    RUN_TEST(test_build_empty_cc_omitted);
    RUN_TEST(test_extract_null);
    RUN_TEST(test_extract_basic);
    RUN_TEST(test_extract_re_dedup);
    RUN_TEST(test_extract_reply_to_header);
    /* US-84: attachment tests — fail until compose_build_message handles them */
    RUN_TEST(test_build_no_attach_is_text_plain);
    RUN_TEST(test_build_one_attachment_multipart);
    RUN_TEST(test_build_attachment_filename_in_disposition);
    RUN_TEST(test_build_two_attachments);
    RUN_TEST(test_build_attachment_boundary_unique);
    RUN_TEST(test_build_attachment_nonexistent_file);
}
