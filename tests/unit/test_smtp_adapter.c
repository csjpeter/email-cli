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

static void test_send_url_from_host_no_scheme(void) {
    /* cfg->host without scheme → url = strdup(cfg->host) path in build_smtp_url;
     * then ssl check fails (not smtps://) → -1 */
    Config cfg = {0};
    cfg.host = "mail.example.com";   /* no :// — hits the else/strdup branch */
    cfg.ssl_no_verify = 0;
    int rc = smtp_send(&cfg, "a@b.com", "c@d.com",
                        "From: a\r\n\r\nbody\r\n", 20);
    ASSERT(rc == -1, "send: bare host URL rejected (not smtps://)");
}

static void test_send_url_fallback_localhost(void) {
    /* No smtp_host, no host → url = "smtp://localhost";
     * ssl_no_verify=0 → rejected (not smtps://) */
    Config cfg = {0};
    cfg.ssl_no_verify = 0;
    int rc = smtp_send(&cfg, "a@b.com", "c@d.com",
                        "From: a\r\n\r\nbody\r\n", 20);
    ASSERT(rc == -1, "send: fallback smtp://localhost rejected");
}

static void test_send_url_fallback_localhost_ssl_skip(void) {
    /* No smtp_host, no host → url = "smtp://localhost";
     * ssl_no_verify=1 → passes TLS check, reaches curl, curl fails to connect → -1 */
    Config cfg = {0};
    cfg.ssl_no_verify = 1;
    int rc = smtp_send(&cfg, "a@b.com", "c@d.com",
                        "From: a\r\n\r\nbody\r\n", 20);
    ASSERT(rc == -1, "send: smtp://localhost + ssl_no_verify reaches curl (connection refused)");
}

static void test_send_smtp_host_with_embedded_port(void) {
    /* smtp_host already contains a port → build_smtp_url uses strdup branch */
    Config cfg = {0};
    cfg.smtp_host = "smtps://smtp.example.com:465";
    cfg.smtp_port = 587;  /* ignored because port already in URL */
    cfg.ssl_no_verify = 1;
    /* smtps:// passes TLS check; curl will fail to connect → -1 */
    int rc = smtp_send(&cfg, "a@b.com", "c@d.com",
                        "From: a\r\n\r\nbody\r\n", 20);
    ASSERT(rc == -1, "send: smtp_host with embedded port (strdup path), curl fails");
}

static void test_send_smtp_host_port_appended(void) {
    /* smtp_port set, no port in smtp_host → asprintf path */
    Config cfg = {0};
    cfg.smtp_host = "smtps://smtp.example.com";
    cfg.smtp_port = 465;
    cfg.ssl_no_verify = 1;
    /* smtps:// passes TLS check; curl will fail to connect → -1 */
    int rc = smtp_send(&cfg, "a@b.com", "c@d.com",
                        "From: a\r\n\r\nbody\r\n", 20);
    ASSERT(rc == -1, "send: smtp_port appended to smtp_host URL, curl fails");
}

static void test_send_display_name_stripping(void) {
    /* from/to with display names → angle-bracket stripping in envelope */
    Config cfg = {0};
    cfg.smtp_host = "smtps://smtp.example.com";
    cfg.ssl_no_verify = 1;
    /* The display name parsing runs before curl; curl fails → -1 */
    int rc = smtp_send(&cfg,
                       "Alice Smith <alice@example.com>",
                       "Bob Jones <bob@example.com>",
                       "From: Alice Smith <alice@example.com>\r\nTo: Bob Jones <bob@example.com>\r\n\r\nHello\r\n",
                       80);
    ASSERT(rc == -1, "send: display name stripping reaches curl (connection refused)");
}

static void test_send_smtps_derived_from_imaps(void) {
    /* Derive from imaps:// → smtps:// passes TLS check; curl fails → -1 */
    Config cfg = {0};
    cfg.host = "imaps://imap.example.com";
    cfg.ssl_no_verify = 1;
    int rc = smtp_send(&cfg, "a@b.com", "c@d.com",
                        "From: a\r\n\r\nbody\r\n", 20);
    ASSERT(rc == -1, "send: smtps derived from imaps + ssl_no_verify reaches curl");
}

/* ── Registration ────────────────────────────────────────────────────── */

void test_smtp_adapter(void) {
    RUN_TEST(test_send_null_cfg);
    RUN_TEST(test_send_null_from);
    RUN_TEST(test_send_null_to);
    RUN_TEST(test_send_null_message);
    RUN_TEST(test_send_rejects_insecure_url);
    RUN_TEST(test_send_rejects_derived_insecure);
    RUN_TEST(test_send_url_from_host_no_scheme);
    RUN_TEST(test_send_url_fallback_localhost);
    RUN_TEST(test_send_url_fallback_localhost_ssl_skip);
    RUN_TEST(test_send_smtp_host_with_embedded_port);
    RUN_TEST(test_send_smtp_host_port_appended);
    RUN_TEST(test_send_display_name_stripping);
    RUN_TEST(test_send_smtps_derived_from_imaps);
}
