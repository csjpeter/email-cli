#include <stdio.h>
#include <stdlib.h>
#include "test_helpers.h"

// Globals defined in test_helpers.h
int g_tests_run = 0;
int g_tests_failed = 0;

// Forward declarations of test suites
void test_fs_util(void);
void test_config_store(void);
void test_logger(void);
void test_wizard(void);
void test_curl_adapter(void);

int main() {
    printf("--- email-cli Unit Test Suite ---\n\n");

    RUN_TEST(test_fs_util);
    RUN_TEST(test_config_store);
    RUN_TEST(test_logger);
    RUN_TEST(test_wizard);
    RUN_TEST(test_curl_adapter);

    printf("\n--- Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    if (g_tests_failed > 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
