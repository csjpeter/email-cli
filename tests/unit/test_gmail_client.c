#include "test_helpers.h"
#include "gmail_client.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>

/* ── gmail_connect — error paths ──────────────────────────────────── */

static void test_connect_not_gmail(void) {
    Config cfg = {0};
    /* gmail_mode is 0 → should fail */
    GmailClient *c = gmail_connect(&cfg);
    ASSERT(c == NULL, "connect: fails for non-Gmail account");
}

static void test_connect_no_token(void) {
    Config cfg = {0};
    cfg.gmail_mode = 1;
    /* No refresh_token → auth_refresh fails → connect fails */
    GmailClient *c = gmail_connect(&cfg);
    ASSERT(c == NULL, "connect: fails with no refresh_token");
}

static void test_disconnect_null(void) {
    /* Should not crash */
    gmail_disconnect(NULL);
    ASSERT(1, "disconnect NULL: no crash");
}

/* ── Registration ─────────────────────────────────────────────────── */

void test_gmail_client(void) {
    RUN_TEST(test_connect_not_gmail);
    RUN_TEST(test_connect_no_token);
    RUN_TEST(test_disconnect_null);
}
