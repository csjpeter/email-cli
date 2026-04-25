#ifndef PTY_ASSERT_H
#define PTY_ASSERT_H

/**
 * @file pty_assert.h
 * @brief Convenience assertion macros for PTY-based terminal tests.
 *
 * These require g_tests_run and g_tests_failed to be defined (same convention
 * as the email-cli test framework).
 */

#include "ptytest.h"
#include <stdio.h>

#ifndef ASSERT
#define ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
        g_tests_failed++; \
        return; \
    } \
} while(0)
#endif

/** @brief Assert that a screen row contains the given text. */
#define ASSERT_ROW_CONTAINS(s, row, text) do { \
    g_tests_run++; \
    if (!pty_row_contains(s, row, text)) { \
        char _buf[256]; \
        pty_row_text(s, row, _buf, sizeof(_buf)); \
        printf("  [FAIL] %s:%d: row %d should contain \"%s\" but got \"%s\"\n", \
               __FILE__, __LINE__, row, text, _buf); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** @brief Assert that a cell has a specific attribute set. */
#define ASSERT_CELL_ATTR(s, row, col, attr_flag) do { \
    g_tests_run++; \
    int _a = pty_cell_attr(s, row, col); \
    if (!(_a & (attr_flag))) { \
        printf("  [FAIL] %s:%d: cell (%d,%d) attr=0x%x, expected 0x%x set\n", \
               __FILE__, __LINE__, row, col, _a, attr_flag); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** @brief Assert that the full screen contains the given text. */
#define ASSERT_SCREEN_CONTAINS(s, text) do { \
    g_tests_run++; \
    if (!pty_screen_contains(s, text)) { \
        printf("  [FAIL] %s:%d: screen should contain \"%s\"\n", \
               __FILE__, __LINE__, text); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** @brief Assert that the full screen does NOT contain the given text. */
#define ASSERT_SCREEN_NOT_CONTAINS(s, text) do { \
    g_tests_run++; \
    if (pty_screen_contains(s, text)) { \
        printf("  [FAIL] %s:%d: screen should NOT contain \"%s\"\n", \
               __FILE__, __LINE__, text); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

/** @brief Assert that pty_wait_for succeeds within the timeout. */
#define ASSERT_WAIT_FOR(s, text, timeout_ms) do { \
    g_tests_run++; \
    if (pty_wait_for(s, text, timeout_ms) != 0) { \
        printf("  [FAIL] %s:%d: wait_for(\"%s\", %d ms) timed out\n", \
               __FILE__, __LINE__, text, timeout_ms); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#endif /* PTY_ASSERT_H */
