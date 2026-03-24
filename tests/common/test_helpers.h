#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>
#include <stdbool.h>

/**
 * @file test_helpers.h
 * @brief Minimal custom unit test assertion macros.
 */

extern int g_tests_run;
extern int g_tests_failed;

#define ASSERT(condition, message) do { \
    g_tests_run++; \
    if (!(condition)) { \
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, message); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define RUN_TEST(test_func) do { \
    printf("Running %s...\n", #test_func); \
    test_func(); \
} while(0)

#endif // TEST_HELPERS_H
