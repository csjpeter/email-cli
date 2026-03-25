#include "test_helpers.h"
#include "logger.h"
#include "fs_util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

void test_logger(void) {
    const char *test_log_dir = "/tmp/email-cli-log-test";
    const char *test_log_file = "/tmp/email-cli-log-test/test.log";

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

    // 5. Test Clean Logs
    // Create a dummy log-like file
    FILE *f = fopen("/tmp/email-cli-log-test/session.log.old", "w");
    if (f) {
        fprintf(f, "old data");
        fclose(f);
    }
    
    res = logger_clean_logs(test_log_dir);
    ASSERT(res == 0, "logger_clean_logs should return 0");
    
    ASSERT(access("/tmp/email-cli-log-test/session.log.old", F_OK) == -1, "Old log should be deleted");

    // 6. Test Rotation Logic (Simulated)
    // Since MAX_LOG_SIZE is 5MB, we won't easily hit it in a unit test without 
    // writing 5MB. But we can test if the function handles existing files.
    logger_close();
    
    // Manual cleanup
    unlink(test_log_file);
    rmdir(test_log_dir);
}
