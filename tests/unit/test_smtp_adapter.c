#include "test_helpers.h"
#include "smtp_adapter.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>

/* ── smtp_send error paths ───────────────────────────────────────────── */

static void test_send_null_cfg(void) {
    ASSERT(smtp_send(NULL, "a@b.com", "c@d.com", "msg", 3) == -1,
           "send: NULL cfg → -1");
}

static void test_send_null_from(void) {
    Config cfg = {0};
    ASSERT(smtp_send(&cfg, NULL, "c@d.com", "msg", 3) == -1,
           "send: NULL from → -1");
}

static void test_send_null_to(void) {
    Config cfg = {0};
    ASSERT(smtp_send(&cfg, "a@b.com", NULL, "msg", 3) == -1,
           "send: NULL to → -1");
}

static void test_send_null_message(void) {
    Config cfg = {0};
    ASSERT(smtp_send(&cfg, "a@b.com", "c@d.com", NULL, 0) == -1,
           "send: NULL message → -1");
}

static void test_send_rejects_insecure_url(void) {
    /* smtp:// without ssl_no_verify should be rejected */
    Config cfg = {0};
    cfg.smtp_host = "smtp://insecure.example.com";
    cfg.ssl_no_verify = 0;
    int rc = smtp_send(&cfg, "a@b.com", "c@d.com",
                        "From: a\r\n\r\nbody\r\n", 20);
    ASSERT(rc == -1, "send: insecure smtp:// rejected");
}

static void test_send_rejects_derived_insecure(void) {
    /* Derived from imap:// (not imaps://) → smtp:// → rejected */
    Config cfg = {0};
    cfg.host = "imap://imap.example.com";
    cfg.ssl_no_verify = 0;
    int rc = smtp_send(&cfg, "a@b.com", "c@d.com",
                        "From: a\r\n\r\nbody\r\n", 20);
    ASSERT(rc == -1, "send: derived smtp:// rejected");
}

/* ── Registration ────────────────────────────────────────────────────── */

void test_smtp_adapter(void) {
    RUN_TEST(test_send_null_cfg);
    RUN_TEST(test_send_null_from);
    RUN_TEST(test_send_null_to);
    RUN_TEST(test_send_null_message);
    RUN_TEST(test_send_rejects_insecure_url);
    RUN_TEST(test_send_rejects_derived_insecure);
}
