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

/* ── Registration ─────────────────────────────────────────────────── */

void test_mail_client(void) {
    RUN_TEST(test_mc_connect_null);
    RUN_TEST(test_mc_connect_imap_no_host);
    RUN_TEST(test_mc_connect_gmail_no_token);
    RUN_TEST(test_mc_free_null);
    RUN_TEST(test_mc_uses_labels_null);
}
