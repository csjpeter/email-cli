#include <stdio.h>
#include <stdlib.h>
#include "test_helpers.h"
#include "logger.h"

// Globals defined in test_helpers.h
int g_tests_run = 0;
int g_tests_failed = 0;

// Forward declarations of test suites
void test_fs_util(void);
void test_config_store(void);
void test_logger(void);
void test_wizard(void);
void test_curl_adapter(void);
void test_mime_util(void);
void test_local_msg_store(void);
void test_local_hdr_evict(void);
void test_local_index(void);
void test_manifest(void);
void test_ui_prefs(void);
void test_imap_util(void);
void test_platform(void);
void test_email_service(void);
void test_html_parser(void);
void test_html_render(void);
void test_html_render_style_balance(void);
void test_html_render_parent_close(void);
void test_html_render_color_filter(void);
void test_html_render_url_isolation(void);

int main() {
    printf("--- email-cli Unit Test Suite ---\n\n");

    // Suppress mirror of ERROR logs to stderr during testing
    logger_set_stderr(0);

    RUN_TEST(test_fs_util);
    RUN_TEST(test_config_store);
    RUN_TEST(test_logger);
    RUN_TEST(test_wizard);
    RUN_TEST(test_curl_adapter);
    RUN_TEST(test_mime_util);
    RUN_TEST(test_local_msg_store);
    RUN_TEST(test_local_hdr_evict);
    RUN_TEST(test_local_index);
    RUN_TEST(test_manifest);
    RUN_TEST(test_ui_prefs);
    RUN_TEST(test_imap_util);
    RUN_TEST(test_platform);
    RUN_TEST(test_email_service);
    RUN_TEST(test_html_parser);
    RUN_TEST(test_html_render);
    RUN_TEST(test_html_render_style_balance);
    RUN_TEST(test_html_render_parent_close);
    RUN_TEST(test_html_render_color_filter);
    RUN_TEST(test_html_render_url_isolation);

    printf("\n--- Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    if (g_tests_failed > 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
