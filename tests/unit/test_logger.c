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
    const char *test_log_dir = "/tmp/email-cli-log-test";
    const char *test_log_file = "/tmp/email-cli-log-test/test.log";
    const char *rotated_log_file = "/tmp/email-cli-log-test/test.log.1";

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
        RAII_FILE FILE *f = fopen("/tmp/email-cli-log-test/session.log.old", "w");
        if (f) {
            fprintf(f, "old data");
        }
    }
    res = logger_clean_logs(test_log_dir);
    ASSERT(res == 0, "logger_clean_logs should return 0");
    ASSERT(access("/tmp/email-cli-log-test/session.log.old", F_OK) == -1,
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

    // Manual cleanup
    unlink(test_log_file);
    unlink(rotated_log_file);
    rmdir(test_log_dir);
}
