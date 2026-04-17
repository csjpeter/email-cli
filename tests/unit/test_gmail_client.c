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

/* ── base64url encode/decode ───────────────────────────────────────── */

static void test_b64_roundtrip(void) {
    const char *orig = "Hello, Gmail API!";
    char *enc = gmail_base64url_encode((const unsigned char *)orig, strlen(orig));
    ASSERT(enc != NULL, "b64 encode: not NULL");

    size_t dec_len = 0;
    char *dec = gmail_base64url_decode(enc, strlen(enc), &dec_len);
    ASSERT(dec != NULL, "b64 decode: not NULL");
    ASSERT(dec_len == strlen(orig), "b64 roundtrip: length matches");
    ASSERT(memcmp(dec, orig, dec_len) == 0, "b64 roundtrip: content matches");
    free(enc);
    free(dec);
}

static void test_b64_empty(void) {
    char *enc = gmail_base64url_encode((const unsigned char *)"", 0);
    ASSERT(enc != NULL, "b64 encode empty: not NULL");
    ASSERT(enc[0] == '\0', "b64 encode empty: empty string");

    size_t dec_len = 0;
    char *dec = gmail_base64url_decode("", 0, &dec_len);
    ASSERT(dec != NULL, "b64 decode empty: not NULL");
    ASSERT(dec_len == 0, "b64 decode empty: zero length");
    free(enc);
    free(dec);
}

static void test_b64_known_vector(void) {
    /* "Man" → TWFu in standard base64, same in base64url */
    size_t len = 0;
    char *dec = gmail_base64url_decode("TWFu", 4, &len);
    ASSERT(dec != NULL && len == 3, "b64 known: length=3");
    ASSERT(memcmp(dec, "Man", 3) == 0, "b64 known: Man");
    free(dec);
}

static void test_b64_url_chars(void) {
    /* Verify - and _ (base64url) instead of + and / */
    unsigned char data[] = {0xfb, 0xff, 0xfe};
    char *enc = gmail_base64url_encode(data, 3);
    ASSERT(enc != NULL, "b64url chars: not NULL");
    ASSERT(strchr(enc, '+') == NULL, "b64url: no +");
    ASSERT(strchr(enc, '/') == NULL, "b64url: no /");
    ASSERT(strchr(enc, '=') == NULL, "b64url: no padding");
    free(enc);
}

/* ── Registration ─────────────────────────────────────────────────── */

void test_gmail_client(void) {
    RUN_TEST(test_connect_not_gmail);
    RUN_TEST(test_connect_no_token);
    RUN_TEST(test_disconnect_null);
    RUN_TEST(test_b64_roundtrip);
    RUN_TEST(test_b64_empty);
    RUN_TEST(test_b64_known_vector);
    RUN_TEST(test_b64_url_chars);
}
