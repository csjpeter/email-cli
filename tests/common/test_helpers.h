/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-24 */
/* Assertion and test-running macros of the unit test framework. */
#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "test_scratch.h"

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

/* Optional substring filter and list mode, set by the runner's command line.
 * The filter selects at the level of the runner: a suite function whose name
 * contains g_test_filter runs, with every test inside it.  Nested RUN_TEST
 * calls are not filtered again. */
extern const char *g_test_filter;
extern int g_test_list_only;
extern int g_tests_selected;
extern int g_test_depth;

#define RUN_TEST(test_func) do { \
    if (g_test_depth == 0) { \
        if (g_test_filter && !strstr(#test_func, g_test_filter)) break; \
        g_tests_selected++; \
        if (g_test_list_only) { printf("%s\n", #test_func); break; } \
    } \
    printf("Running %s...\n", #test_func); \
    g_test_depth++; \
    test_func(); \
    g_test_depth--; \
} while(0)

#endif // TEST_HELPERS_H
