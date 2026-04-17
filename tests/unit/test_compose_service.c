#include "test_helpers.h"
#include "compose_service.h"
#include <stdlib.h>
#include <string.h>

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

void test_compose_service(void) {
    RUN_TEST(test_build_null_params);
    RUN_TEST(test_build_missing_fields);
    RUN_TEST(test_build_null_out);
    RUN_TEST(test_build_basic);
    RUN_TEST(test_build_reply);
    RUN_TEST(test_build_empty_body);
    RUN_TEST(test_build_lf_to_crlf);
    RUN_TEST(test_extract_null);
    RUN_TEST(test_extract_basic);
    RUN_TEST(test_extract_re_dedup);
    RUN_TEST(test_extract_reply_to_header);
}
