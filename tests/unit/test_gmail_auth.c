#include "test_helpers.h"
#include "gmail_auth.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>

/* ── gmail_auth_device_flow — error paths (no network needed) ─────── */

static void test_device_flow_no_client_id(void) {
    Config cfg = {0};
    /* No client_id configured and default is empty → should fail */
    int rc = gmail_auth_device_flow(&cfg);
    ASSERT(rc == -1, "device_flow: fails with no client_id");
}

/* ── gmail_auth_refresh — error paths (no network needed) ─────────── */

static void test_refresh_no_token(void) {
    Config cfg = {0};
    /* No refresh_token → should return NULL */
    char *tok = gmail_auth_refresh(&cfg);
    ASSERT(tok == NULL, "refresh: returns NULL with no refresh_token");
}

static void test_refresh_empty_token(void) {
    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("");
    char *tok = gmail_auth_refresh(&cfg);
    ASSERT(tok == NULL, "refresh: returns NULL with empty refresh_token");
    free(cfg.gmail_refresh_token);
}

/* ── Registration ─────────────────────────────────────────────────── */

void test_gmail_auth(void) {
    RUN_TEST(test_device_flow_no_client_id);
    RUN_TEST(test_refresh_no_token);
    RUN_TEST(test_refresh_empty_token);
}
