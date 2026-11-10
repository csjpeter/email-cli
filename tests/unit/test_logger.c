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

/** Number of lines of @p path that contain @p needle (-1: no such file). */
static int count_lines(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[8192];
    int n = 0;
    while (fgets(line, sizeof(line), f)) if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

void test_logger(void) {
    const char *test_log_dir = TP("email-cli-log-test");
    const char *test_log_file = TP("email-cli-log-test/test.log");
    const char *rotated_log_file = TP("email-cli-log-test/test.log.1");

    // 1. Prepare directory
    fs_mkdir_p(test_log_dir, 0700);
    unlink(test_log_file);

    // 2. Init makes the file (0600) and a traffic line reaches it
    int res = logger_init(test_log_file);
    ASSERT(res == 0, "logger_init should return 0");
    ASSERT(logger_traffic_enabled(), "the traffic log is open");
    logger_traffic("IMAP [OUT] %s %s", "a1", "NOOP");
    struct stat st;
    ASSERT(stat(test_log_file, &st) == 0, "Log file should exist");
    ASSERT(st.st_size > 0, "Log file should not be empty");
    ASSERT((st.st_mode & 0777) == 0600, "the traffic log is 0600");
    ASSERT(count_lines(test_log_file, "IMAP [OUT] a1 NOOP") == 1, "the traffic line is written");

    // 3. Clean Logs
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

    // 4. Close: traffic with no file does not crash and writes nothing
    logger_close();
    ASSERT(!logger_traffic_enabled(), "closed: the traffic log is off");
    logger_traffic("IMAP [OUT] a2 NOOP");
    ASSERT(count_lines(test_log_file, "a2 NOOP") == 0, "nothing is written once closed");

    // 5. logger_init with invalid (non-existent) path
    res = logger_init("/nonexistent/dir/path/test.log");
    ASSERT(res == -1, "logger_init should return -1 for invalid path");

    // 6. logger_clean_logs with non-existent directory
    res = logger_clean_logs("/nonexistent/dir/path");
    ASSERT(res == -1, "logger_clean_logs should return -1 for non-existent dir");

    // 7. Log rotation: a file > 5MB is rotated by init
    unlink(test_log_file);
    {
        RAII_FILE FILE *big = fopen(test_log_file, "wb");
        if (big) {
            fseek(big, 5 * 1024 * 1024, SEEK_SET);
            fputc('\0', big);
        }
    }
    res = logger_init(test_log_file);
    ASSERT(res == 0, "logger_init should succeed after rotating oversized log");
    ASSERT(stat(test_log_file, &st) == 0, "Log file should exist after rotation");
    ASSERT(st.st_size < 5 * 1024 * 1024,
           "Current log should be small after rotation");
    ASSERT(access(rotated_log_file, F_OK) == 0,
           "Rotated log file should exist");
    logger_close();

    // 8. Five old files are kept, the sixth goes
    for (int round = 0; round < 7; round++) {
        {
            RAII_FILE FILE *big = fopen(test_log_file, "wb");
            if (big) { fseek(big, 5 * 1024 * 1024, SEEK_SET); fputc('\0', big); }
        }
        ASSERT(logger_init(test_log_file) == 0, "init while rotating repeatedly");
        logger_close();
    }
    ASSERT(access(TP("email-cli-log-test/test.log.5"), F_OK) == 0, "the fifth old file is kept");
    ASSERT(access(TP("email-cli-log-test/test.log.6"), F_OK) == -1, "no sixth old file");

    // Manual cleanup
    unlink(test_log_file);
    for (int i = 1; i <= 5; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s.%d", test_log_file, i);
        unlink(p);
    }
    rmdir(test_log_dir);
}

/* EMAIL-284: no kind of credential reaches the traffic file.  Each secret is
 * fed in the shape the protocol modules use; the file is searched for it. */
void test_logger_masks_credentials(void) {
    const char *dir = TP("email-cli-log-mask");
    const char *f = TP("email-cli-log-mask/mask.log");
    fs_mkdir_p(dir, 0700);
    unlink(f);
    ASSERT(logger_init(f) == 0, "mask: log opens");

    logger_traffic("IMAP [OUT] A001 LOGIN alice@example.org hunter2PASSWORD");
    logger_traffic("IMAP [OUT] A002 LOGIN \"alice@example.org\" \"hunter3PASSWORD\"");
    logger_traffic("IMAP [OUT] A003 AUTHENTICATE XOAUTH2 dXNlcj1hbGljZQFhdXRoPUJlYXJlciB5YTI5LlNFQ1JFVFRPS0VOAQE=");
    logger_traffic("IMAP [OUT] A004 authenticate xoauth2 BEARERSECRETdata");
    logger_traffic("SMTP [OUT] AUTH PLAIN AGFsaWNlAFNNVFBTRUNSRVRQQVNT");
    logger_traffic("SMTP [OUT] AUTH LOGIN");
    logger_traffic("HTTP [OUT] Authorization: Bearer ya29.GMAILBEARERTOKEN");
    logger_traffic("HTTP [OUT] authorization: Basic QkFTSUNTRUNSRVQ=");
    logger_traffic("HTTP [ IN] {\"access_token\": \"ya29.NEWACCESSTOKEN\", \"expires_in\": 3599}");
    logger_traffic("HTTP [OUT] refresh_token=1//REFRESHSECRET&client_secret=CLIENTSECRETVALUE&grant_type=refresh_token");
    logger_traffic("IMAP [ IN] * OK ready");
    logger_close();

    static const char *const secrets[] = {
        "hunter2PASSWORD", "hunter3PASSWORD", "dXNlcj1hbGljZQFh", "BEARERSECRETdata",
        "AGFsaWNlAFNNVFBT", "GMAILBEARERTOKEN", "QkFTSUNTRUNSRVQ", "NEWACCESSTOKEN",
        "REFRESHSECRET", "CLIENTSECRETVALUE",
    };
    for (size_t i = 0; i < sizeof(secrets) / sizeof(secrets[0]); i++)
        ASSERT(count_lines(f, secrets[i]) == 0, secrets[i]);
    ASSERT(count_lines(f, "LOGIN alice@example.org xxxxx") == 1, "mask: LOGIN keeps the user");
    ASSERT(count_lines(f, "AUTHENTICATE XOAUTH2 xxxxx") == 1, "mask: AUTHENTICATE keeps the mechanism");
    ASSERT(count_lines(f, "AUTH PLAIN xxxxx") == 1, "mask: SMTP AUTH PLAIN");
    ASSERT(count_lines(f, "AUTH LOGIN") == 1, "mask: AUTH LOGIN has nothing to mask");
    ASSERT(count_lines(f, "Bearer xxxxx") == 1, "mask: the Authorization scheme stays");
    ASSERT(count_lines(f, "expires_in") == 1, "mask: the rest of the JSON stays");
    ASSERT(count_lines(f, "grant_type=refresh_token") == 1, "mask: the rest of the form stays");
    ASSERT(count_lines(f, "IMAP [ IN] * OK ready") == 1, "mask: ordinary lines are untouched");

    /* a short secret still fits the 5-character mark; a tiny buffer is bounded */
    char tiny[16] = "A1 LOGIN u pw";
    logger_mask_secret(tiny, sizeof(tiny));
    ASSERT(strstr(tiny, "pw") == NULL && strlen(tiny) < sizeof(tiny), "mask: short secret, bounded");

    unlink(f);
    rmdir(dir);
}

/* EMAIL-284: nothing opens a file unless logger_init is called. */
void test_logger_no_file_without_init(void) {
    const char *dir = TP("email-cli-log-none");
    const char *f = TP("email-cli-log-none/none.log");
    fs_mkdir_p(dir, 0700);
    unlink(f);
    logger_close();
    logger_traffic("IMAP [OUT] a1 NOOP");
    logger_error("not logged to a file");
    ASSERT(access(f, F_OK) == -1, "no traffic file without logger_init");
    ASSERT(!logger_traffic_enabled(), "the traffic log is off");
    rmdir(dir);
}

/* EMAIL-118: a program and libemail-client (one copy of the logger when
 * linked static) both open the log: the second open of the same file keeps
 * the stream and does not write the banner again; another path switches. */
void test_logger_double_init(void) {
    const char *dir = TP("email-cli-log-double");
    const char *a = TP("email-cli-log-double/a.log");
    const char *b = TP("email-cli-log-double/b.log");
    fs_mkdir_p(dir, 0700);
    unlink(a);
    unlink(b);

    ASSERT(logger_init(a) == 0, "double init: first open");
    ASSERT(logger_init(a) == 0, "double init: second open of the same file");
    logger_traffic("IMAP [OUT] after the second open");
    ASSERT(count_lines(a, "after the second open") == 1,
           "double init: the stream is kept, the line written once");

    ASSERT(logger_init(b) == 0, "double init: another file");
    logger_traffic("IMAP [OUT] into b");
    logger_close();
    ASSERT(count_lines(b, "into b") == 1, "double init: another path switches the log");
    ASSERT(count_lines(a, "into b") == 0, "double init: the old file gets nothing more");

    unlink(a);
    unlink(b);
    rmdir(dir);
}

void test_logger_debug_requested(void) {
    char a0[] = "prog", a1[] = "list", a2[] = "--debug", a3[] = "-q";
    char *argv[] = { a0, a1, a2, a3, NULL };
    int argc = 4;

    unsetenv("EMAIL_CLI_DEBUG");
    ASSERT(logger_debug_requested(&argc, argv) == 1, "--debug asks for the trace");
    ASSERT(argc == 3 && strcmp(argv[2], "-q") == 0 && argv[3] == NULL,
           "--debug is removed from argv");

    char b0[] = "prog", b1[] = "list";
    char *argv2[] = { b0, b1, NULL };
    argc = 2;
    ASSERT(logger_debug_requested(&argc, argv2) == 0, "off by default");
    ASSERT(argc == 2, "nothing removed without --debug");

    /* the environment variable is gone: only the option asks (EMAIL-284) */
    setenv("EMAIL_CLI_DEBUG", "1", 1);
    ASSERT(logger_debug_requested(&argc, argv2) == 0, "EMAIL_CLI_DEBUG=1 does not ask for the trace");
    unsetenv("EMAIL_CLI_DEBUG");
}

void test_logger_stderr_without_file(void) {
    /* No log file open (the default, EMAIL-240): an error still reaches stderr
     * and is counted, so the user sees it. */
    logger_close();
    logger_set_stderr(1);
    logger_stderr_only();
    unsigned before = logger_stderr_error_count();
    int saved = dup(2);
    const char *cap = TP("email-cli-log-nofile.err");
    FILE *f = freopen(cap, "w", stderr);
    ASSERT(f != NULL, "stderr captured");
    logger_error("no file %d", 7);
    logger_traffic("IMAP [OUT] not an error");
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    clearerr(stderr);
    logger_set_stderr(1);
    logger_close();   /* ends the stderr-only mode */
    ASSERT(logger_stderr_error_count() == before + 1, "the error is counted");
    ASSERT(count_lines(cap, "ERROR: no file 7") == 1, "the error is written to stderr");
    ASSERT(count_lines(cap, "not an error") == 0, "traffic lines are not mirrored to stderr");
    unlink(cap);
}
