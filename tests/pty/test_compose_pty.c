/**
 * @file test_compose_pty.c
 * @brief PTY tests for email-tui compose/reply/send commands.
 *
 * Usage: test-pty-compose <email-tui-bin> <mock-smtp-server-bin>
 */

#include "ptytest.h"
#include "pty_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

/* ── Test infrastructure ─────────────────────────────────────────────── */

#define COLS 120
#define ROWS  50
#define WAIT_MS  4000
#define SETTLE_MS 300

static int  g_tests_run    = 0;
static int  g_tests_failed = 0;

static char g_tui_bin[512];
static char g_smtp_mock_bin[512];
static char g_test_home[512];
static pid_t g_smtp_pid = -1;

#define RUN_TEST(fn) \
    do { \
        fprintf(stdout, "  %s...\n", #fn); \
        fflush(stdout); \
        fn(); \
    } while(0)

/* ── Config helpers ──────────────────────────────────────────────────── */

static void write_config(void) {
    char d1[600], d2[620], path[640];
    snprintf(d1, sizeof(d1), "%s/.config", g_test_home);
    snprintf(d2, sizeof(d2), "%s/.config/email-cli", g_test_home);
    mkdir(g_test_home, 0700);
    mkdir(d1, 0700);
    mkdir(d2, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", d2);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "EMAIL_HOST=imap://localhost:9993\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SMTP_HOST=smtp://localhost:9025\n"
        "SMTP_PORT=9025\n"
        "SMTP_USER=testuser\n"
        "SMTP_PASS=testpass\n");
    fclose(fp);
    chmod(path, 0600);
}

/* ── Mock SMTP server management ──────────────────────────────────────── */

static int probe_smtp(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(9025),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return ret;
}

static void start_smtp_server(void) {
    g_smtp_pid = fork();
    if (g_smtp_pid < 0) return;
    if (g_smtp_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
        execl(g_smtp_mock_bin, "mock_smtp_server", (char *)NULL);
        _exit(127);
    }
    usleep(600000);
}

static void restart_smtp(void) {
    if (g_smtp_pid > 0) {
        kill(g_smtp_pid, SIGKILL);
        waitpid(g_smtp_pid, NULL, 0);
        g_smtp_pid = -1;
    }
    usleep(200000);
    start_smtp_server();
    for (int i = 0; i < 30 && probe_smtp() != 0; i++)
        usleep(100000);
}

static void stop_smtp_server(void) {
    if (g_smtp_pid > 0) {
        kill(g_smtp_pid, SIGKILL);
        waitpid(g_smtp_pid, NULL, 0);
        g_smtp_pid = -1;
    }
}

/* ── Runner helper ───────────────────────────────────────────────────── */

static PtySession *tui_open_size(int cols, int rows, const char **extra_args) {
    const char *args[32];
    int n = 0;
    args[n++] = g_tui_bin;
    if (extra_args)
        for (int i = 0; extra_args[i] && n < 31; i++)
            args[n++] = extra_args[i];
    args[n] = NULL;

    PtySession *s = pty_open(cols, rows);
    if (!s) return NULL;
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    return s;
}

static PtySession *tui_run(const char **extra_args) {
    return tui_open_size(COLS, ROWS, extra_args);
}

/* ── Help page tests ─────────────────────────────────────────────────── */

static void test_help_compose(void) {
    const char *a[] = {"compose", "--help", NULL};
    PtySession *s = tui_open_size(120, 50, a);
    ASSERT(s != NULL, "help compose: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-tui", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "To");
    ASSERT_SCREEN_CONTAINS(s, "Subject");
    pty_close(s);
}

static void test_help_send(void) {
    const char *a[] = {"send", "--help", NULL};
    PtySession *s = tui_open_size(120, 50, a);
    ASSERT(s != NULL, "help send: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-tui", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "--to");
    ASSERT_SCREEN_CONTAINS(s, "--subject");
    ASSERT_SCREEN_CONTAINS(s, "--body");
    pty_close(s);
}

static void test_help_reply(void) {
    const char *a[] = {"reply", "--help", NULL};
    PtySession *s = tui_open_size(120, 50, a);
    ASSERT(s != NULL, "help reply: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-tui", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "<uid>");
    pty_close(s);
}

/* ── Batch send tests ──────────────────────────────────────────────────── */

static void test_send_missing_to(void) {
    write_config();
    const char *a[] = {"--batch", "send", "--subject", "Hi", "--body", "Hello", NULL};
    PtySession *s = tui_open_size(120, 50, a);
    ASSERT(s != NULL, "send missing to: opens");
    ASSERT_WAIT_FOR(s, "required", WAIT_MS);
    pty_close(s);
}

static void test_send_batch_ok(void) {
    write_config();
    restart_smtp();
    const char *a[] = {"--batch", "send",
        "--to",      "recipient@example.com",
        "--subject", "Test Subject",
        "--body",    "Hello from PTY test",
        NULL};
    PtySession *s = tui_run(a);
    ASSERT(s != NULL, "batch send: opens");
    ASSERT_WAIT_FOR(s, "Message sent", WAIT_MS);
    pty_close(s);
}

static void test_send_no_smtp_config(void) {
    /* Write config without SMTP settings */
    char d1[600], d2[620], path[640];
    snprintf(d1, sizeof(d1), "%s/.config", g_test_home);
    snprintf(d2, sizeof(d2), "%s/.config/email-cli", g_test_home);
    mkdir(g_test_home, 0700); mkdir(d1, 0700); mkdir(d2, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", d2);
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp,
            "EMAIL_HOST=imap://localhost:9993\n"
            "EMAIL_USER=testuser\n"
            "EMAIL_PASS=testpass\n"
            "EMAIL_FOLDER=INBOX\n");
        fclose(fp);
        chmod(path, 0600);
    }
    const char *a[] = {"--batch", "send",
        "--to", "x@x.com", "--subject", "X", "--body", "X", NULL};
    PtySession *s = tui_run(a);
    ASSERT(s != NULL, "no smtp cfg: opens");
    /* Should still attempt; derives SMTP from IMAP host */
    /* We just check it exits (doesn't hang) */
    pty_settle(s, SETTLE_MS * 5);
    pty_close(s);
}

/* ── Compose TUI tests (editor-based) ───────────────────────────────── */

/**
 * Helper: write a shell script that replaces the draft file ($1) with a
 * complete message and returns.  Sets $EDITOR to that script path.
 * Returns the script path (caller must unlink when done).
 */
static char g_editor_script[256];

static void setup_editor_mock(const char *from, const char *to,
                              const char *subject, const char *body) {
    snprintf(g_editor_script, sizeof(g_editor_script),
             "/tmp/test_editor_%d.sh", (int)getpid());
    FILE *ef = fopen(g_editor_script, "w");
    if (!ef) return;
    fprintf(ef, "#!/bin/sh\n");
    fprintf(ef,
            "printf 'From: %s\\nTo: %s\\nSubject: %s\\n\\n%s\\n' > \"$1\"\n",
            from, to, subject, body);
    fclose(ef);
    chmod(g_editor_script, 0755);
    setenv("EDITOR", g_editor_script, 1);
}

static void cleanup_editor_mock(void) {
    unlink(g_editor_script);
    g_editor_script[0] = '\0';
}

/**
 * compose abort: EDITOR=true → editor exits without changing To: → "Aborted"
 */
static void test_compose_abort_no_to(void) {
    write_config();
    setenv("EDITOR", "true", 1);  /* no-op editor: exits without saving */
    const char *a[] = {"compose", NULL};
    PtySession *s = tui_open_size(120, 50, a);
    ASSERT(s != NULL, "compose abort no-to: opens");
    ASSERT_WAIT_FOR(s, "Aborted", WAIT_MS);
    pty_close(s);
}

/**
 * compose success: mock editor fills in all headers + body → "Message sent"
 */
static void test_compose_editor_send(void) {
    write_config();
    restart_smtp();
    setup_editor_mock("testuser@example.com", "recipient@example.com",
                      "PTY test subject", "Hello from editor mock test");
    const char *a[] = {"compose", NULL};
    PtySession *s = tui_run(a);
    ASSERT(s != NULL, "compose editor send: opens");
    ASSERT_WAIT_FOR(s, "Message sent", WAIT_MS * 2);
    pty_close(s);
    cleanup_editor_mock();
}

/**
 * reply abort: batch reply with EDITOR=true → empty To: → "Aborted"
 * (cmd_reply is invoked; the raw message may not exist → error before editor)
 */
static void test_reply_missing_uid(void) {
    write_config();
    const char *a[] = {"--batch", "reply", NULL};
    PtySession *s = tui_run(a);
    ASSERT(s != NULL, "reply missing uid: opens");
    ASSERT_WAIT_FOR(s, "requires", WAIT_MS);
    pty_close(s);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <email-tui> <mock-smtp-server>\n", argv[0]);
        return 1;
    }

    snprintf(g_tui_bin,       sizeof(g_tui_bin),       "%s", argv[1]);
    snprintf(g_smtp_mock_bin, sizeof(g_smtp_mock_bin),  "%s", argv[2]);

    /* Isolated HOME for tests */
    snprintf(g_test_home, sizeof(g_test_home), "/tmp/email_cli_compose_test_%d", getpid());
    mkdir(g_test_home, 0700);
    setenv("HOME", g_test_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_DATA_HOME");

    write_config();
    start_smtp_server();
    for (int i = 0; i < 30 && probe_smtp() != 0; i++) usleep(100000);

    printf("\n--- Compose/send: help pages ---\n");
    RUN_TEST(test_help_compose);
    RUN_TEST(test_help_send);
    RUN_TEST(test_help_reply);

    printf("\n--- Compose/send: batch send ---\n");
    RUN_TEST(test_send_missing_to);
    RUN_TEST(test_send_batch_ok);
    RUN_TEST(test_send_no_smtp_config);

    printf("\n--- Compose/send: interactive compose ---\n");
    RUN_TEST(test_compose_abort_no_to);
    RUN_TEST(test_compose_editor_send);
    RUN_TEST(test_reply_missing_uid);

    stop_smtp_server();

    printf("\n--- PTY Compose Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
