/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-25 */
/* Unit tests of the logger. */
#include "test_helpers.h"
#include "logger.h"
#include "fs_util.h"
#include "raii.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

void test_logger(void) {
    const char *test_log_dir = TP("email-cli-log-test");
    const char *test_log_file = TP("email-cli-log-test/test.log");
    const char *rotated_log_file = TP("email-cli-log-test/test.log.1");

    // 1. Prepare directory
    fs_mkdir_p(test_log_dir, 0700);

    // 2. Test Init
    int res = logger_init(test_log_file, LOG_DEBUG);
    ASSERT(res == 0, "logger_init should return 0");

    // 3. Test Logging at various levels
    logger_log(LOG_DEBUG, "Test debug message");
    logger_log(LOG_INFO, "Test info message");
    logger_log(LOG_WARN, "Test warn message");
    logger_log(LOG_ERROR, "Test error message");

    // 4. Verify file exists and has content
    struct stat st;
    ASSERT(stat(test_log_file, &st) == 0, "Log file should exist");
    ASSERT(st.st_size > 0, "Log file should not be empty");

    // 5. Test level filtering: reinit with LOG_ERROR, lower-level messages suppressed
    logger_close();
    res = logger_init(test_log_file, LOG_ERROR);
    ASSERT(res == 0, "logger_init with LOG_ERROR should succeed");
    logger_log(LOG_DEBUG, "This should be filtered out");
    logger_log(LOG_INFO,  "This should be filtered out");
    logger_log(LOG_WARN,  "This should be filtered out");

    // 6. Test Clean Logs
    {
        RAII_FILE FILE *f = fopen(TP("email-cli-log-test/session.log.old"), "w");
        if (f) {
            fprintf(f, "old data");
        }
    }
    res = logger_clean_logs(test_log_dir);
    ASSERT(res == 0, "logger_clean_logs should return 0");
    ASSERT(access(TP("email-cli-log-test/session.log.old"), F_OK) == -1,
           "Old log should be deleted");

    // 7. Close and verify logger_log with NULL fp does not crash
    logger_close();
    logger_log(LOG_ERROR, "Should not crash with NULL fp");

    // 8. Test logger_init with invalid (non-existent) path
    res = logger_init("/nonexistent/dir/path/test.log", LOG_INFO);
    ASSERT(res == -1, "logger_init should return -1 for invalid path");

    // 9. Test logger_clean_logs with non-existent directory
    res = logger_clean_logs("/nonexistent/dir/path");
    ASSERT(res == -1, "logger_clean_logs should return -1 for non-existent dir");

    // 10. Test log rotation: create a file > 5MB, then init should rotate it
    unlink(test_log_file);
    {
        RAII_FILE FILE *big = fopen(test_log_file, "wb");
        if (big) {
            fseek(big, 5 * 1024 * 1024, SEEK_SET);
            fputc('\0', big);
        }
    }
    res = logger_init(test_log_file, LOG_INFO);
    ASSERT(res == 0, "logger_init should succeed after rotating oversized log");
    ASSERT(stat(test_log_file, &st) == 0, "Log file should exist after rotation");
    ASSERT(st.st_size < 5 * 1024 * 1024,
           "Current log should be small after rotation");
    ASSERT(access(rotated_log_file, F_OK) == 0,
           "Rotated log file should exist");
    logger_close();

    // 11. Test stderr mirror path (lines 100-104): re-enable stderr, log error
    {
        logger_init(test_log_file, LOG_DEBUG);
        logger_set_stderr(1);  /* re-enable stderr mirror */
        unsigned before = logger_stderr_error_count();
        logger_log(LOG_WARN, "a warning is not counted");
        ASSERT(logger_stderr_error_count() == before, "WARN does not count as a stderr error");
        logger_log(LOG_ERROR, "stderr mirror test");
        ASSERT(logger_stderr_error_count() == before + 1, "an ERROR written to stderr is counted");
        logger_set_stderr(0);  /* restore for subsequent tests */
        logger_note_stderr_error();
        ASSERT(logger_stderr_error_count() == before + 2, "an error the caller wrote itself can be noted");
        logger_log(LOG_ERROR, "not mirrored, not counted");
        ASSERT(logger_stderr_error_count() == before + 2, "an ERROR not mirrored to stderr is not counted");
        logger_close();
    }

    // 12. Unknown log level → level_to_str returns "UNKNOWN"
    {
        logger_init(test_log_file, LOG_DEBUG);
        logger_log(99, "unknown level test");  /* level 99 → "UNKNOWN" */
        logger_close();
    }

    // Manual cleanup
    unlink(test_log_file);
    unlink(rotated_log_file);
    rmdir(test_log_dir);
}

/* EMAIL-118: a program and libemail-client (one copy of the logger when
 * linked static) both open the log: the second open of the same file keeps
 * the stream and does not write the banner again; another path switches. */
static int count_lines(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f)) if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

void test_logger_double_init(void) {
    const char *dir = TP("email-cli-log-double");
    const char *a = TP("email-cli-log-double/a.log");
    const char *b = TP("email-cli-log-double/b.log");
    fs_mkdir_p(dir, 0700);
    unlink(a);
    unlink(b);

    ASSERT(logger_init(a, LOG_INFO) == 0, "double init: first open");
    ASSERT(logger_init(a, LOG_DEBUG) == 0, "double init: second open of the same file");
    logger_log(LOG_DEBUG, "debug after the second open");
    ASSERT(count_lines(a, "Logging initialized") == 1,
           "double init: the banner is written once");
    ASSERT(count_lines(a, "debug after the second open") == 1,
           "double init: the second call's level applies");

    ASSERT(logger_init(b, LOG_INFO) == 0, "double init: another file");
    logger_log(LOG_INFO, "into b");
    logger_close();
    ASSERT(count_lines(b, "into b") == 1, "double init: another path switches the log");
    ASSERT(count_lines(a, "into b") == 0, "double init: the old file gets nothing more");

    unlink(a);
    unlink(b);
    rmdir(dir);
}
