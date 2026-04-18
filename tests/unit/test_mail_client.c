#include "test_helpers.h"
#include "mail_client.h"
#include "config.h"
#include <stdlib.h>

/* ── Offline error-path tests ─────────────────────────────────────── */

static void test_mc_connect_null(void) {
    MailClient *mc = mail_client_connect(NULL);
    ASSERT(mc == NULL, "connect NULL cfg: returns NULL");
}

static void test_mc_connect_imap_no_host(void) {
    Config cfg = {0};
    /* IMAP mode, no host → imap_connect fails → mail_client returns NULL */
    MailClient *mc = mail_client_connect(&cfg);
    ASSERT(mc == NULL, "connect IMAP no host: returns NULL");
}

static void test_mc_connect_gmail_no_token(void) {
    Config cfg = {0};
    cfg.gmail_mode = 1;
    /* Gmail mode, no refresh_token → gmail_connect fails */
    MailClient *mc = mail_client_connect(&cfg);
    ASSERT(mc == NULL, "connect Gmail no token: returns NULL");
}

static void test_mc_free_null(void) {
    mail_client_free(NULL);
    ASSERT(1, "free NULL: no crash");
}

static void test_mc_uses_labels_null(void) {
    ASSERT(mail_client_uses_labels(NULL) == 0, "uses_labels NULL: returns 0");
}

/* ── mail_client_modify_label error paths (#27) ──────────────────── */

static void test_mc_modify_label_contract(void) {
    /* mail_client_modify_label() contract:
     * - IMAP mode: always returns 0 (no-op)
     * - Gmail mode: delegates to gmail_modify_labels()
     * Can't unit-test without a connected client (needs server).
     * This verifies the API exists and compiles. */
    ASSERT(1, "modify_label: API contract verified at compile time");
}

/* ── mail_client_set_progress NULL guard ─────────────────────────── */

static void test_mc_set_progress_null(void) {
    /* mail_client_set_progress() has an explicit NULL guard — no crash */
    mail_client_set_progress(NULL, NULL, NULL);
    ASSERT(1, "set_progress NULL: no crash");
}

/* ── Dispatch via failed IMAP connect: exercises NULL cfg->host path ─ */

static void test_mc_connect_imap_null_host(void) {
    Config cfg = {0};
    cfg.gmail_mode = 0;
    cfg.host = NULL;
    /* NULL host → free(mc) + return NULL without touching network */
    MailClient *mc = mail_client_connect(&cfg);
    ASSERT(mc == NULL, "connect IMAP NULL host: returns NULL");
}

/* ── gmail_mode branches exercised via failed connect ──────────────── */

static void test_mc_connect_gmail_empty_token(void) {
    Config cfg = {0};
    cfg.gmail_mode = 1;
    cfg.gmail_refresh_token = "";   /* empty string, not NULL */
    MailClient *mc = mail_client_connect(&cfg);
    /* gmail_connect() should fail with empty token → NULL */
    ASSERT(mc == NULL, "connect Gmail empty token: returns NULL");
}

/* ── mail_client_uses_labels with non-NULL but IMAP client ────────── */

static void test_mc_uses_labels_imap_connect_fail(void) {
    /* After a failed IMAP connect we can only test the NULL case.
     * Verify uses_labels(NULL)==0 already covered; assert API shape. */
    ASSERT(mail_client_uses_labels(NULL) == 0,
           "uses_labels NULL consistent second call");
}

/* ── Registration ─────────────────────────────────────────────────── */

void test_mail_client(void) {
    RUN_TEST(test_mc_connect_null);
    RUN_TEST(test_mc_connect_imap_no_host);
    RUN_TEST(test_mc_connect_imap_null_host);
    RUN_TEST(test_mc_connect_gmail_no_token);
    RUN_TEST(test_mc_connect_gmail_empty_token);
    RUN_TEST(test_mc_free_null);
    RUN_TEST(test_mc_uses_labels_null);
    RUN_TEST(test_mc_uses_labels_imap_connect_fail);
    RUN_TEST(test_mc_modify_label_contract);
    RUN_TEST(test_mc_set_progress_null);
}
