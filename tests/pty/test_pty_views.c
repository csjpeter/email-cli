/**
 * @file test_pty_views.c
 * @brief PTY-based tests for all email-cli views and modes.
 *
 * Usage: test-pty-views <email-cli> <mock-imap-server> <email-cli-ro> <email-sync>
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ptytest.h"
#include "pty_assert.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>

int g_tests_run = 0;
int g_tests_failed = 0;

/* ── Test infrastructure ─────────────────────────────────────────────── */

static pid_t g_mock_pid = -1;
static char  g_test_home[256];
static char  g_cli_bin[512];
static char  g_cli_ro_bin[512];
static char  g_sync_bin[512];
static char  g_mock_bin[512];
static char  g_old_home[512];

#define WAIT_MS 6000
#define SETTLE_MS 600
#define ROWS 24
#define COLS 100

static void write_config(void) {
    char d1[300], d2[300], path[350];
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
        "EMAIL_FOLDER=INBOX\n");
    fclose(fp);
    chmod(path, 0600);
}

static int start_mock_server(void) {
    g_mock_pid = fork();
    if (g_mock_pid < 0) return -1;
    if (g_mock_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execl(g_mock_bin, "mock_imap_server", (char *)NULL);
        _exit(127);
    }
    usleep(800000);
    return 0;
}

static int probe_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = { .sin_family = AF_INET,
        .sin_port = htons(9993), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return ret;
}

static void restart_mock(void) {
    if (g_mock_pid > 0) {
        kill(g_mock_pid, SIGKILL);
        waitpid(g_mock_pid, NULL, 0);
        g_mock_pid = -1;
    }
    usleep(200000);
    start_mock_server();
    for (int i = 0; i < 30 && probe_server() != 0; i++)
        usleep(100000);
}

static void stop_mock_server(void) {
    if (g_mock_pid > 0) {
        kill(g_mock_pid, SIGKILL);
        waitpid(g_mock_pid, NULL, 0);
        g_mock_pid = -1;
    }
}

static PtySession *cli_open_size(int cols, int rows, const char **extra_args) {
    const char *args[32];
    int n = 0;
    args[n++] = g_cli_bin;
    if (extra_args)
        for (int i = 0; extra_args[i] && n < 31; i++)
            args[n++] = extra_args[i];
    args[n] = NULL;

    PtySession *s = pty_open(cols, rows);
    if (!s) return NULL;
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    return s;
}

static PtySession *cli_run(const char **extra_args) {
    return cli_open_size(COLS, ROWS, extra_args);
}

/** @brief Find the row containing text; returns -1 if not found. */
static int find_row(PtySession *s, const char *text) {
    for (int r = 0; r < ROWS; r++)
        if (pty_row_contains(s, r, text)) return r;
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════
 *  HELP PAGE TESTS
 * ══════════════════════════════════════════════════════════════════════ */

static void test_help_general(void) {
    const char *a[] = {"--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "help: opens");
    ASSERT_WAIT_FOR(s, "Commands:", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "list");
    ASSERT_SCREEN_CONTAINS(s, "show");
    ASSERT_SCREEN_CONTAINS(s, "folders");
    ASSERT_SCREEN_CONTAINS(s, "sync");
    ASSERT_SCREEN_CONTAINS(s, "help");
    pty_close(s);
}

static void test_help_list(void) {
    const char *a[] = {"list", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "help list: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-cli", WAIT_MS); /* common prefix for -ro too */
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "--all");
    ASSERT_SCREEN_CONTAINS(s, "--folder");
    pty_close(s);
}

static void test_help_show(void) {
    const char *a[] = {"show", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "help show: opens");
    ASSERT_WAIT_FOR(s, "show <uid>", WAIT_MS);
    pty_close(s);
}

static void test_help_folders(void) {
    const char *a[] = {"folders", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "help folders: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-cli", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "--tree");
    pty_close(s);
}

static void test_help_sync(void) {
    const char *a[] = {"sync", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "help sync: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-cli", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "sync");
    pty_close(s);
}

static void test_help_cron(void) {
    const char *a[] = {"cron", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "help cron: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-cli", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "cron");
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  BATCH MODE TESTS
 * ══════════════════════════════════════════════════════════════════════ */

static void test_batch_list(void) {
    const char *a[] = {"list", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch list: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "UID");
    ASSERT_SCREEN_CONTAINS(s, "From");
    ASSERT_SCREEN_CONTAINS(s, "Subject");
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_close(s);
}

static void test_batch_list_all(void) {
    const char *a[] = {"list", "--all", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch list --all: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "message(s) in");
    pty_close(s);
}

static void test_batch_list_empty(void) {
    const char *a[] = {"list", "--folder", "INBOX.Empty", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch list empty: opens");
    ASSERT_WAIT_FOR(s, "No messages", WAIT_MS);
    pty_close(s);
}

static void test_batch_show(void) {
    const char *a[] = {"show", "1", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch show: opens");
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Subject:");
    ASSERT_SCREEN_CONTAINS(s, "Date:");
    ASSERT_SCREEN_CONTAINS(s, "Hello from Mock Server");
    pty_close(s);
}

static void test_batch_folders_flat(void) {
    const char *a[] = {"folders", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch folders: opens");
    ASSERT_WAIT_FOR(s, "INBOX", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX.Sent");
    ASSERT_SCREEN_CONTAINS(s, "INBOX.Trash");
    pty_close(s);
}

static void test_batch_folders_tree(void) {
    const char *a[] = {"folders", "--tree", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch folders --tree: opens");
    ASSERT_WAIT_FOR(s, "INBOX", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Sent");
    ASSERT_SCREEN_CONTAINS(s, "Trash");
    pty_close(s);
}

static void test_batch_list_folder(void) {
    const char *a[] = {"list", "--folder", "INBOX.Sent", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch list --folder: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX.Sent");
    pty_close(s);
}

static void test_batch_list_limit(void) {
    const char *a[] = {"list", "--limit", "1", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch list --limit: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_close(s);
}

static void test_batch_list_offset(void) {
    const char *a[] = {"list", "--offset", "1", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch list --offset: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_close(s);
}

static void test_batch_sync(void) {
    restart_mock();
    const char *a[] = {"sync", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch sync: opens");
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);
}

static void test_batch_cron_status(void) {
    const char *a[] = {"cron", "status", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch cron status: opens");
    /* Matches either "No email-cli sync cron entry is installed." or
     * "Active sync cron entry:" — both contain "cron" */
    ASSERT_WAIT_FOR(s, "cron", WAIT_MS);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  INTERACTIVE LIST VIEW
 * ══════════════════════════════════════════════════════════════════════ */

static void test_interactive_list_content(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "interactive list: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "UID");
    ASSERT_SCREEN_CONTAINS(s, "From");
    ASSERT_SCREEN_CONTAINS(s, "Subject");
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_list_separator(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "list separator: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "══");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_list_statusbar(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "list statusbar: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Find statusbar dynamically — contains "step" */
    int sb = find_row(s, "step");
    ASSERT(sb >= 0, "list sb: found row with 'step'");
    if (sb >= 0) {
        ASSERT(pty_row_contains(s, sb, "Enter=open"), "list sb: Enter=open");
        ASSERT(pty_row_contains(s, sb, "Backspace=folders"), "list sb: Backspace");
        ASSERT(pty_row_contains(s, sb, "ESC=quit"), "list sb: ESC=quit");
        ASSERT_CELL_ATTR(s, sb, 2, PTY_ATTR_REVERSE);
    }
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_list_esc_quit(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "list ESC: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS); /* program exits quietly — no output to wait for */
    pty_close(s);
}

static void test_interactive_list_nav(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "list nav: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* With one message, UP/DOWN keep cursor in place — no crash expected */
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_key(s, PTY_KEY_UP);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_key(s, PTY_KEY_PGDN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_key(s, PTY_KEY_PGUP);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_list_flags(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "list flags: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Flag keys: n=new, f=flagged, d=done — server accepts silently */
    pty_send_str(s, "n");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_str(s, "f");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  INTERACTIVE SHOW VIEW
 * ══════════════════════════════════════════════════════════════════════ */

static void test_interactive_show_content(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "show content: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Subject:");
    ASSERT_SCREEN_CONTAINS(s, "Date:");
    ASSERT_SCREEN_CONTAINS(s, "Hello from Mock Server");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_show_separator(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "show separator: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "────");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_show_statusbar(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "show statusbar: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    int sb = find_row(s, "Backspace/ESC/q=list");
    ASSERT(sb >= 0, "show sb: found Backspace/ESC/q=list");
    if (sb >= 0) {
        ASSERT(pty_row_contains(s, sb, "PgDn"), "show sb: PgDn");
        ASSERT_CELL_ATTR(s, sb, 2, PTY_ATTR_REVERSE);
    }
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_show_backspace(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "show backspace: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Backspace returns to list */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_show_esc_to_list(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "show ESC→list: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* ESC from show view returns to the message list */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_show_q_to_list(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "show q→list: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* 'q' from show view returns to the message list */
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_show_pgdn(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "show PgDn: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* PgDn/PgUp on a single-page message — no crash expected */
    pty_send_key(s, PTY_KEY_PGDN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_send_key(s, PTY_KEY_PGUP);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_send_key(s, PTY_KEY_UP);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  INTERACTIVE FOLDER BROWSER
 * ══════════════════════════════════════════════════════════════════════ */

static void test_interactive_folders_content(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "folders content: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    ASSERT_SCREEN_CONTAINS(s, "Sent");
    ASSERT_SCREEN_CONTAINS(s, "Trash");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_folders_statusbar(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "folders sb: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    int sb = find_row(s, "Enter=open/select");
    ASSERT(sb >= 0, "folders sb: found Enter=open/select");
    if (sb >= 0)
        ASSERT_CELL_ATTR(s, sb, 2, PTY_ATTR_REVERSE);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_folders_toggle(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "folders toggle: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Toggle to flat */
    pty_send_str(s, "t");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "t=tree");

    /* Toggle back to tree */
    pty_send_str(s, "t");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "t=flat");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_folders_select(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "folders select: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_folders_nav(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "folders nav: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Navigate down and back up — folders remain visible */
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    pty_send_key(s, PTY_KEY_UP);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    pty_send_key(s, PTY_KEY_PGDN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    pty_send_key(s, PTY_KEY_PGUP);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_folders_back_to_list(void) {
    /* Backspace from folder browser (root) returns to message list */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "folders back→list: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Backspace returns to the message list */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_folders_esc_quit(void) {
    /* ESC from folder browser quits the entire application */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "folders ESC quit: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* ESC exits the application from the folder browser */
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  EMPTY FOLDER
 * ══════════════════════════════════════════════════════════════════════ */

static void test_interactive_empty_folder(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "empty: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Navigate to INBOX.Empty (tree: INBOX=0, Empty=1, Sent=2, Trash=3) */
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);

    if (pty_wait_for(s, "No messages", WAIT_MS) == 0) {
        ASSERT_SCREEN_CONTAINS(s, "Backspace=folders");
        pty_send_key(s, PTY_KEY_BACK);
        ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    }
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  ATTACHMENT SAVE TESTS
 *
 * The mock server message is multipart/mixed with two attachments:
 *   notes.txt  (content: "Hello World")
 *   data.bin   (content: "test data")
 *
 * attachment_save_dir() returns $HOME (= g_test_home) because there is
 * no ~/Downloads in the isolated test environment.
 * ══════════════════════════════════════════════════════════════════════ */

/** Helper: open the show view of the single test message.
 *  Waits until both headers AND attachment status bar are fully rendered. */
static PtySession *open_show_view(void) {
    restart_mock();
    PtySession *s = cli_run(NULL);
    if (!s) return NULL;
    if (pty_wait_for(s, "Test Message", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    /* Wait for the attachment status bar — proves the full show view rendered */
    if (pty_wait_for(s, "A=save-all", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/** Status bar shows  a=save  A=save-all(2) when attachments present. */
static void test_show_attachment_statusbar(void) {
    PtySession *s = open_show_view();
    ASSERT(s != NULL, "att statusbar: opens");

    ASSERT_SCREEN_CONTAINS(s, "a=save");
    ASSERT_SCREEN_CONTAINS(s, "A=save-all(2)");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** Attachment picker is shown when 'a' is pressed (2 attachments). */
static void test_show_attachment_picker(void) {
    PtySession *s = open_show_view();
    ASSERT(s != NULL, "att picker: opens");

    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Attachments", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "notes.txt");
    ASSERT_SCREEN_CONTAINS(s, "data.bin");

    /* Backspace returns to show view */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** Pressing 'a', selecting first attachment, confirming saves it. */
static void test_show_save_single(void) {
    PtySession *s = open_show_view();
    ASSERT(s != NULL, "save single: opens");

    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Attachments", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Select first attachment (notes.txt) */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Save as:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Accept pre-filled path */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Saved:", WAIT_MS);

    /* File must exist on disk */
    char path[512];
    snprintf(path, sizeof(path), "%s/notes.txt", g_test_home);
    ASSERT(access(path, F_OK) == 0, "notes.txt saved to disk");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** Pressing 'A' then ESC cancels without saving any file. */
static void test_show_save_all_cancel(void) {
    PtySession *s = open_show_view();
    ASSERT(s != NULL, "save-all cancel: opens");

    /* Remove any pre-existing file from a previous test run */
    char path[512];
    snprintf(path, sizeof(path), "%s/data.bin", g_test_home);
    remove(path);

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Save all to:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);

    /* No "Saved" status must appear after ESC */
    ASSERT(pty_screen_contains(s, "Saved 2/2") == 0, "no save on ESC");
    ASSERT(access(path, F_OK) != 0, "data.bin NOT on disk after ESC");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** Pressing 'A' then Enter saves all attachments to the default dir. */
static void test_show_save_all_confirm(void) {
    PtySession *s = open_show_view();
    ASSERT(s != NULL, "save-all confirm: opens");

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Save all to:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Accept the pre-filled default (g_test_home) */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Saved 2/2", WAIT_MS);

    /* Both files must exist on disk */
    char p1[512], p2[512];
    snprintf(p1, sizeof(p1), "%s/notes.txt", g_test_home);
    snprintf(p2, sizeof(p2), "%s/data.bin",  g_test_home);
    ASSERT(access(p1, F_OK) == 0, "notes.txt saved");
    ASSERT(access(p2, F_OK) == 0, "data.bin saved");

    /* Verify content of notes.txt ("Hello World") */
    FILE *f = fopen(p1, "r");
    if (f) {
        char buf[64] = "";
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        ASSERT(strcmp(buf, "Hello World") == 0, "notes.txt content correct");
    }

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  email-cli-ro EXCLUSIVE TESTS
 * ══════════════════════════════════════════════════════════════════════ */

static void test_ro_list_folder(void) {
    const char *a[] = {"list", "--folder", "INBOX.Sent", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "ro list --folder: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX.Sent");
    pty_close(s);
}

static void test_ro_list_limit(void) {
    const char *a[] = {"list", "--limit", "1", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "ro list --limit: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_close(s);
}

static void test_ro_list_offset(void) {
    const char *a[] = {"list", "--offset", "1", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "ro list --offset: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_close(s);
}

static void test_ro_sync(void) {
    restart_mock();
    const char *a[] = {"sync", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "ro sync: opens");
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);
}

static void test_ro_no_config(void) {
    /* Run with a HOME that contains no email-cli configuration */
    char no_home[300];
    snprintf(no_home, sizeof(no_home), "%s/no_config", g_test_home);
    mkdir(no_home, 0700);
    setenv("HOME", no_home, 1);
    unsetenv("XDG_CONFIG_HOME");

    const char *a[] = {"list", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "ro no config: opens");
    ASSERT_WAIT_FOR(s, "No configuration found", WAIT_MS);
    pty_close(s);

    /* Restore test HOME */
    setenv("HOME", g_test_home, 1);
}

/* ── email-sync tests ────────────────────────────────────────────────── */

static PtySession *sync_run(const char **args) {
    const char *argv[16];
    int n = 0;
    argv[n++] = g_sync_bin;
    if (args)
        for (int i = 0; args[i] && n < 15; i++)
            argv[n++] = args[i];
    argv[n] = NULL;

    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    if (pty_run(s, argv) != 0) { pty_close(s); return NULL; }
    return s;
}

static void test_sync_help(void) {
    const char *a[] = {"--help", NULL};
    PtySession *s = sync_run(a);
    ASSERT(s != NULL, "sync --help: opens");
    ASSERT_WAIT_FOR(s, "email-sync", WAIT_MS);
    ASSERT_WAIT_FOR(s, "--help", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Exit Codes", WAIT_MS);
    pty_close(s);
}

static void test_sync_run(void) {
    restart_mock();
    const char *a[] = {NULL};
    PtySession *s = sync_run(a);
    ASSERT(s != NULL, "sync run: opens");
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);
}

static void test_sync_unknown_opt(void) {
    const char *a[] = {"--bogus-option", NULL};
    PtySession *s = sync_run(a);
    ASSERT(s != NULL, "sync unknown opt: opens");
    ASSERT_WAIT_FOR(s, "Unknown option", WAIT_MS);
    pty_close(s);
}

static void test_sync_no_config(void) {
    char no_home[300];
    snprintf(no_home, sizeof(no_home), "%s/no_config_sync", g_test_home);
    mkdir(no_home, 0700);
    setenv("HOME", no_home, 1);
    unsetenv("XDG_CONFIG_HOME");

    const char *a[] = {NULL};
    PtySession *s = sync_run(a);
    ASSERT(s != NULL, "sync no config: opens");
    ASSERT_WAIT_FOR(s, "No configuration found", WAIT_MS);
    pty_close(s);

    setenv("HOME", g_test_home, 1);
}

/* ── Main ────────────────────────────────────────────────────────────── */

#define RUN_TEST(fn) do { printf("  %s...\n", #fn); fn(); } while(0)

int main(int argc, char *argv[]) {
    printf("--- email-cli PTY View Tests ---\n\n");

    if (argc < 5) {
        fprintf(stderr, "Usage: %s <email-cli> <mock-server> <email-cli-ro> <email-sync>\n", argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(g_cli_bin,    sizeof(g_cli_bin),    "%s", argv[1]);
    snprintf(g_mock_bin,   sizeof(g_mock_bin),   "%s", argv[2]);
    snprintf(g_cli_ro_bin, sizeof(g_cli_ro_bin), "%s", argv[3]);
    snprintf(g_sync_bin,   sizeof(g_sync_bin),   "%s", argv[4]);

    const char *home = getenv("HOME");
    if (home) snprintf(g_old_home, sizeof(g_old_home), "%s", home);

    snprintf(g_test_home, sizeof(g_test_home),
             "/tmp/email-cli-pty-test-%d", getpid());
    setenv("HOME", g_test_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_DATA_HOME");
    write_config();

    printf("--- Help pages ---\n");
    RUN_TEST(test_help_general);
    RUN_TEST(test_help_list);
    RUN_TEST(test_help_show);
    RUN_TEST(test_help_folders);
    RUN_TEST(test_help_sync);
    RUN_TEST(test_help_cron);

    printf("\n--- Starting mock IMAP server ---\n");
    if (start_mock_server() != 0) {
        fprintf(stderr, "FATAL: cannot start mock server\n");
        goto done;
    }

    printf("\n--- Batch mode ---\n");
    RUN_TEST(test_batch_list);
    RUN_TEST(test_batch_list_all);
    RUN_TEST(test_batch_list_empty);
    RUN_TEST(test_batch_list_folder);
    RUN_TEST(test_batch_list_limit);
    RUN_TEST(test_batch_list_offset);
    RUN_TEST(test_batch_show);
    RUN_TEST(test_batch_folders_flat);
    RUN_TEST(test_batch_folders_tree);
    RUN_TEST(test_batch_sync);
    RUN_TEST(test_batch_cron_status);

    printf("\n--- Interactive list ---\n");
    RUN_TEST(test_interactive_list_content);
    RUN_TEST(test_interactive_list_separator);
    RUN_TEST(test_interactive_list_statusbar);
    RUN_TEST(test_interactive_list_esc_quit);
    RUN_TEST(test_interactive_list_nav);
    RUN_TEST(test_interactive_list_flags);

    printf("\n--- Interactive show ---\n");
    RUN_TEST(test_interactive_show_content);
    RUN_TEST(test_interactive_show_separator);
    RUN_TEST(test_interactive_show_statusbar);
    RUN_TEST(test_interactive_show_backspace);
    RUN_TEST(test_interactive_show_esc_to_list);
    RUN_TEST(test_interactive_show_q_to_list);
    RUN_TEST(test_interactive_show_pgdn);

    printf("\n--- Interactive folders ---\n");
    RUN_TEST(test_interactive_folders_content);
    RUN_TEST(test_interactive_folders_statusbar);
    RUN_TEST(test_interactive_folders_toggle);
    RUN_TEST(test_interactive_folders_select);
    RUN_TEST(test_interactive_folders_nav);
    RUN_TEST(test_interactive_folders_back_to_list);
    RUN_TEST(test_interactive_folders_esc_quit);

    printf("\n--- Attachment save ---\n");
    RUN_TEST(test_show_attachment_statusbar);
    RUN_TEST(test_show_attachment_picker);
    RUN_TEST(test_show_save_single);
    RUN_TEST(test_show_save_all_cancel);
    RUN_TEST(test_show_save_all_confirm);

    printf("\n--- Empty folder ---\n");
    RUN_TEST(test_interactive_empty_folder);

    /* ── email-cli-ro: batch-only subset ─────────────────────────────── */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_cli_ro_bin);

    printf("\n--- email-cli-ro: help pages ---\n");
    RUN_TEST(test_help_general);
    RUN_TEST(test_help_list);
    RUN_TEST(test_help_show);
    RUN_TEST(test_help_folders);
    RUN_TEST(test_help_sync);

    printf("\n--- email-cli-ro: batch mode ---\n");
    restart_mock();
    RUN_TEST(test_batch_list);
    RUN_TEST(test_batch_list_all);
    RUN_TEST(test_batch_list_empty);
    RUN_TEST(test_batch_show);
    RUN_TEST(test_batch_folders_flat);
    RUN_TEST(test_batch_folders_tree);

    printf("\n--- email-cli-ro: exclusive capabilities ---\n");
    RUN_TEST(test_ro_list_folder);
    RUN_TEST(test_ro_list_limit);
    RUN_TEST(test_ro_list_offset);
    RUN_TEST(test_ro_sync);
    RUN_TEST(test_ro_no_config);

    /* ── email-sync: standalone sync binary ──────────────────────────── */
    printf("\n--- email-sync ---\n");
    RUN_TEST(test_sync_help);
    RUN_TEST(test_sync_run);
    RUN_TEST(test_sync_unknown_opt);
    RUN_TEST(test_sync_no_config);

done:
    stop_mock_server();
    if (g_old_home[0]) setenv("HOME", g_old_home, 1);

    printf("\n--- PTY Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
