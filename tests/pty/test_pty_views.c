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
static char  g_tui_bin[512];
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

static void write_config_with_interval(int interval) {
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
        "EMAIL_FOLDER=INBOX\n"
        "SYNC_INTERVAL=%d\n", interval);
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
    /* Matches "No email-sync cron entry found." or "Cron entry found:" */
    ASSERT_WAIT_FOR(s, "ron", WAIT_MS);
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
    /* PgDn scrolls a full page; header (From:) stays pinned at top */
    pty_send_key(s, PTY_KEY_PGDN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_send_key(s, PTY_KEY_PGUP);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_show_arrow_scroll(void) {
    /* US 12: ↓/↑ scroll one line at a time (not a full page like PgDn/PgUp).
     * Use a small terminal (10 rows → rows_avail=4) with a 9-line body so
     * there are 3 pages.  PgDn jumps to page 2; ↑ steps back to page 1;
     * ↓ steps forward to page 2 again — confirming single-line granularity. */
    restart_mock();
    PtySession *s = cli_open_size(COLS, 10, NULL); /* small rows → multi-page */
    ASSERT(s != NULL, "show arrow scroll: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Jump a full page to page 2 */
    pty_send_key(s, PTY_KEY_PGDN);
    ASSERT_WAIT_FOR(s, "2/", WAIT_MS);
    /* ↑ steps back one line → page 1 */
    pty_send_key(s, PTY_KEY_UP);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "1/");
    /* ↓ steps forward one line → page 2 again */
    pty_send_key(s, PTY_KEY_DOWN);
    ASSERT_WAIT_FOR(s, "2/", WAIT_MS);
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

static void test_interactive_folders_flat_navigate_up(void) {
    /* US 15: in flat view, Enter on a folder with children navigates into it
     * (sets current_prefix); Backspace navigates back up one level. */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "folders flat nav up: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);            /* open folder browser */
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "t");                     /* toggle to flat view */
    ASSERT_WAIT_FOR(s, "t=tree", WAIT_MS);    /* flat mode: hint shows "t=tree" */
    pty_settle(s, SETTLE_MS);
    /* Flat view at root shows only top-level folders (no '.' in name) */
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    pty_send_key(s, PTY_KEY_ENTER);           /* navigate into INBOX */
    ASSERT_WAIT_FOR(s, "Backspace=up", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Header shows "Folders: INBOX/ (3)"; flat mode shows last component only */
    ASSERT_SCREEN_CONTAINS(s, "INBOX/");
    ASSERT_SCREEN_CONTAINS(s, "Sent");
    pty_send_key(s, PTY_KEY_BACK);            /* navigate up to root */
    ASSERT_WAIT_FOR(s, "Backspace=back", WAIT_MS);
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

static void test_ro_help_general(void) {
    const char *a[] = {"--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "ro help: opens");
    ASSERT_WAIT_FOR(s, "Commands:", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "list");
    ASSERT_SCREEN_CONTAINS(s, "show");
    ASSERT_SCREEN_CONTAINS(s, "folders");
    ASSERT_SCREEN_CONTAINS(s, "attachments");
    ASSERT_SCREEN_CONTAINS(s, "save-attachment");
    ASSERT_SCREEN_CONTAINS(s, "help");
    pty_close(s);
}

static void test_ro_help_list(void) {
    const char *a[] = {"list", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "ro help list: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-cli-ro list", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "--folder");
    pty_close(s);
}

static void test_ro_help_attachments(void) {
    const char *a[] = {"attachments", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "ro help attachments: opens");
    ASSERT_WAIT_FOR(s, "attachments <uid>", WAIT_MS);
    pty_close(s);
}

static void test_ro_help_save_attachment(void) {
    const char *a[] = {"save-attachment", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "ro help save-attachment: opens");
    ASSERT_WAIT_FOR(s, "save-attachment", WAIT_MS);
    pty_close(s);
}

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

static void test_ro_sync_unknown(void) {
    const char *a[] = {"sync", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "ro sync unknown: opens");
    ASSERT_WAIT_FOR(s, "Unknown command", WAIT_MS);
    pty_close(s);
}

static void test_ro_attachments(void) {
    restart_mock();
    const char *a[] = {"attachments", "1", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "ro attachments: opens");
    ASSERT_WAIT_FOR(s, "notes.txt", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "data.bin");
    pty_close(s);
}

static void test_ro_save_attachment(void) {
    restart_mock();
    char tmpdir[300];
    snprintf(tmpdir, sizeof(tmpdir), "%s/att_save", g_test_home);
    mkdir(tmpdir, 0700);
    const char *a[] = {"save-attachment", "1", "notes.txt", tmpdir, NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "ro save-attachment: opens");
    ASSERT_WAIT_FOR(s, "Saved:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    char expected[600];
    snprintf(expected, sizeof(expected), "%s/notes.txt", tmpdir);
    struct stat st;
    ASSERT(stat(expected, &st) == 0, "ro save-attachment: file exists");
    ASSERT(st.st_size > 0, "ro save-attachment: file non-empty");
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

/* ══════════════════════════════════════════════════════════════════════
 *  NON-TTY FALLBACK (US 01)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_nonttty_shows_help(void) {
    /* Run email-cli with stdout piped (non-TTY) — must print help and exit 0. */
    char cmd[768];
    snprintf(cmd, sizeof(cmd), "%s 2>/dev/null", g_cli_bin);
    FILE *fp = popen(cmd, "r");
    ASSERT(fp != NULL, "non-tty: popen");
    char out[4096] = {0};
    if (fp) {
        size_t n = fread(out, 1, sizeof(out) - 1, fp);
        (void)n;
        pclose(fp);
    }
    ASSERT(strstr(out, "Commands:") != NULL, "non-tty: shows general help (Commands:)");
}

/* ══════════════════════════════════════════════════════════════════════
 *  SETUP WIZARD (US 10)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_wizard_abort(void) {
    char wiz_home[300];
    snprintf(wiz_home, sizeof(wiz_home), "%s/wizard_abort", g_test_home);
    mkdir(wiz_home, 0700);
    setenv("HOME", wiz_home, 1);
    unsetenv("XDG_CONFIG_HOME");

    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "wizard abort: opens");
    ASSERT_WAIT_FOR(s, "IMAP Host", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);  /* EOF on stdin → getline returns -1 → wizard aborts */
    ASSERT_WAIT_FOR(s, "borted", WAIT_MS);
    pty_close(s);

    setenv("HOME", g_test_home, 1);
}

static void test_wizard_complete(void) {
    char wiz_home[300];
    snprintf(wiz_home, sizeof(wiz_home), "%s/wizard_complete", g_test_home);
    mkdir(wiz_home, 0700);
    setenv("HOME", wiz_home, 1);
    unsetenv("XDG_CONFIG_HOME");

    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "wizard complete: opens");
    ASSERT_WAIT_FOR(s, "IMAP Host", WAIT_MS);
    pty_send_str(s, "imap://localhost:9993\n");
    ASSERT_WAIT_FOR(s, "sername", WAIT_MS);
    pty_send_str(s, "testuser\n");
    ASSERT_WAIT_FOR(s, "assword", WAIT_MS);
    pty_send_str(s, "testpass\n");
    ASSERT_WAIT_FOR(s, "older", WAIT_MS);
    pty_send_str(s, "INBOX\n");
    ASSERT_WAIT_FOR(s, "SMTP Host", WAIT_MS);
    pty_send_str(s, "\n");  /* skip SMTP config */
    ASSERT_WAIT_FOR(s, "Configuration saved", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    setenv("HOME", g_test_home, 1);
}

/* ══════════════════════════════════════════════════════════════════════
 *  CRON MANAGEMENT (US 06, 07, 08)
 * ══════════════════════════════════════════════════════════════════════ */

/** Remove any email-sync cron entry — used for cleanup between cron tests. */
static void cron_cleanup(void) {
    const char *a[] = {"cron", "remove", NULL};
    PtySession *s = cli_run(a);
    if (!s) return;
    /* Wait until crontab is fully written before closing */
    pty_wait_for(s, "ron", WAIT_MS); /* "Cron job removed." or "No email-sync cron entry found." */
    pty_close(s);
}

static void test_cron_status_not_found(void) {
    cron_cleanup(); /* ensure clean state */
    const char *a[] = {"cron", "status", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "cron status not found: opens");
    ASSERT_WAIT_FOR(s, "No email-sync cron entry found", WAIT_MS);
    pty_close(s);
}

static void test_cron_setup_default_interval(void) {
    /* Standard config has no SYNC_INTERVAL → defaults to 5 min */
    write_config();
    const char *a[] = {"cron", "setup", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "cron setup default interval: opens");
    ASSERT_WAIT_FOR(s, "sync_interval not configured", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Cron job installed:", WAIT_MS);
    pty_close(s);
    cron_cleanup();
}

static void test_cron_setup_installs(void) {
    cron_cleanup();
    write_config_with_interval(15);
    const char *a[] = {"cron", "setup", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "cron setup installs: opens");
    ASSERT_WAIT_FOR(s, "Cron job installed:", WAIT_MS);
    pty_close(s);
    /* leave entry for next test */
}

static void test_cron_status_found(void) {
    /* Entry was left by test_cron_setup_installs */
    const char *a[] = {"cron", "status", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "cron status found: opens");
    ASSERT_WAIT_FOR(s, "Cron entry found:", WAIT_MS);
    pty_close(s);
}

static void test_cron_setup_already_installed(void) {
    /* Entry still present from test_cron_setup_installs */
    const char *a[] = {"cron", "setup", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "cron setup already installed: opens");
    ASSERT_WAIT_FOR(s, "already installed", WAIT_MS);
    pty_close(s);
}

static void test_cron_remove_entry(void) {
    /* Entry still present — remove it */
    const char *a[] = {"cron", "remove", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "cron remove entry: opens");
    ASSERT_WAIT_FOR(s, "Cron job removed", WAIT_MS);
    pty_close(s);
}

static void test_cron_remove_not_found(void) {
    /* Entry was removed by test_cron_remove_entry */
    const char *a[] = {"cron", "remove", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "cron remove not found: opens");
    ASSERT_WAIT_FOR(s, "No email-sync cron entry found", WAIT_MS);
    pty_close(s);
    write_config(); /* restore standard config (no sync_interval) */
}

/* ══════════════════════════════════════════════════════════════════════
 *  SYNC PROGRESS (US 05)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_sync_progress(void) {
    restart_mock();
    const char *a[] = {"sync", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "sync progress: opens");
    ASSERT_WAIT_FOR(s, "Syncing", WAIT_MS);
    ASSERT_WAIT_FOR(s, "fetched", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  SHOW CACHE HIT (US 03)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_show_cache_hit(void) {
    /* Sync to populate local store */
    restart_mock();
    { const char *a[] = {"sync", NULL};
      PtySession *s = cli_run(a);
      ASSERT(s != NULL, "show cache: sync opens");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    /* Show must work from cache even with no server */
    stop_mock_server();
    { const char *a[] = {"show", "1", "--batch", NULL};
      PtySession *s = cli_run(a);
      ASSERT(s != NULL, "show cache: show opens");
      ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
      pty_close(s); }

    restart_mock();
}

/* ══════════════════════════════════════════════════════════════════════
 *  OFFLINE / CRON MODE (US 11)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_offline_list(void) {
    /* Sync first to populate manifest */
    restart_mock();
    { const char *a[] = {"sync", NULL};
      PtySession *s = cli_run(a);
      ASSERT(s != NULL, "offline list: sync opens");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    /* Switch to cron/offline mode (sync_interval > 0) */
    write_config_with_interval(5);

    /* Stop server — list must be served entirely from manifest */
    stop_mock_server();
    { const char *a[] = {"list", "--batch", NULL};
      PtySession *s = cli_run(a);
      ASSERT(s != NULL, "offline list: list opens");
      ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
      pty_close(s); }

    /* Restore */
    write_config();
    restart_mock();
}

static void test_offline_show_not_cached(void) {
    /* US 11: opening a non-cached message in cron/offline mode shows an error */
    write_config_with_interval(5);
    stop_mock_server();

    /* UID 999 has never been synced → not in local store */
    const char *a[] = {"show", "999", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "offline show not cached: opens");
    ASSERT_WAIT_FOR(s, "Could not load", WAIT_MS);
    pty_close(s);

    write_config();
    restart_mock();
}

/* ── email-tui help tests ────────────────────────────────────────────── */

static void test_tui_help_general(void) {
    const char *a[] = {"--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "tui help: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-tui", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "Commands:");
    ASSERT_SCREEN_CONTAINS(s, "list");
    ASSERT_SCREEN_CONTAINS(s, "cron");
    pty_close(s);
}

static void test_tui_help_list(void) {
    const char *a[] = {"list", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "tui help list: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-tui list", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "--all");
    ASSERT_SCREEN_CONTAINS(s, "--folder");
    pty_close(s);
}

static void test_tui_help_cron(void) {
    const char *a[] = {"cron", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "tui help cron: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-tui cron", WAIT_MS);
    pty_close(s);
}

static void test_tui_interactive_launch(void) {
    /* US 18: email-tui with no args in a TTY starts the interactive TUI */
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "tui no-args launch: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
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

    if (argc < 6) {
        fprintf(stderr, "Usage: %s <email-cli> <mock-server> <email-cli-ro> <email-sync> <email-tui>\n", argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(g_cli_bin,    sizeof(g_cli_bin),    "%s", argv[1]);
    snprintf(g_mock_bin,   sizeof(g_mock_bin),   "%s", argv[2]);
    snprintf(g_cli_ro_bin, sizeof(g_cli_ro_bin), "%s", argv[3]);
    snprintf(g_sync_bin,   sizeof(g_sync_bin),   "%s", argv[4]);
    snprintf(g_tui_bin,    sizeof(g_tui_bin),    "%s", argv[5]);

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
    RUN_TEST(test_interactive_show_arrow_scroll);

    printf("\n--- Interactive folders ---\n");
    RUN_TEST(test_interactive_folders_content);
    RUN_TEST(test_interactive_folders_statusbar);
    RUN_TEST(test_interactive_folders_toggle);
    RUN_TEST(test_interactive_folders_select);
    RUN_TEST(test_interactive_folders_nav);
    RUN_TEST(test_interactive_folders_back_to_list);
    RUN_TEST(test_interactive_folders_flat_navigate_up);
    RUN_TEST(test_interactive_folders_esc_quit);

    printf("\n--- Attachment save ---\n");
    RUN_TEST(test_show_attachment_statusbar);
    RUN_TEST(test_show_attachment_picker);
    RUN_TEST(test_show_save_single);
    RUN_TEST(test_show_save_all_cancel);
    RUN_TEST(test_show_save_all_confirm);

    printf("\n--- Empty folder ---\n");
    RUN_TEST(test_interactive_empty_folder);

    printf("\n--- Sync progress ---\n");
    RUN_TEST(test_sync_progress);

    printf("\n--- Show cache hit ---\n");
    RUN_TEST(test_show_cache_hit);

    printf("\n--- Offline / cron mode ---\n");
    RUN_TEST(test_offline_list);
    RUN_TEST(test_offline_show_not_cached);

    /* ── email-cli-ro: batch-only subset ─────────────────────────────── */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_cli_ro_bin);

    printf("\n--- email-cli-ro: help pages ---\n");
    RUN_TEST(test_ro_help_general);
    RUN_TEST(test_ro_help_list);
    RUN_TEST(test_help_show);
    RUN_TEST(test_help_folders);
    RUN_TEST(test_ro_help_attachments);
    RUN_TEST(test_ro_help_save_attachment);

    printf("\n--- email-cli-ro: batch mode ---\n");
    restart_mock();
    RUN_TEST(test_batch_list);
    RUN_TEST(test_batch_list_empty);
    RUN_TEST(test_batch_show);
    RUN_TEST(test_batch_folders_flat);
    RUN_TEST(test_batch_folders_tree);

    printf("\n--- email-cli-ro: exclusive capabilities ---\n");
    RUN_TEST(test_ro_list_folder);
    RUN_TEST(test_ro_list_limit);
    RUN_TEST(test_ro_list_offset);
    RUN_TEST(test_ro_sync_unknown);
    RUN_TEST(test_ro_attachments);
    RUN_TEST(test_ro_save_attachment);
    RUN_TEST(test_ro_no_config);

    /* Restore full email-cli binary for the remaining tests */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", argv[1]);

    /* ── Setup wizard ────────────────────────────────────────────────── */
    printf("\n--- Setup wizard ---\n");
    RUN_TEST(test_wizard_abort);
    RUN_TEST(test_wizard_complete);

    /* ── Non-TTY fallback ────────────────────────────────────────────── */
    printf("\n--- Non-TTY fallback ---\n");
    RUN_TEST(test_nonttty_shows_help);

    /* ── Cron management ─────────────────────────────────────────────── */
    printf("\n--- Cron management ---\n");
    RUN_TEST(test_cron_status_not_found);
    RUN_TEST(test_cron_setup_default_interval);
    RUN_TEST(test_cron_setup_installs);
    RUN_TEST(test_cron_status_found);
    RUN_TEST(test_cron_setup_already_installed);
    RUN_TEST(test_cron_remove_entry);
    RUN_TEST(test_cron_remove_not_found);

    /* ── email-sync: standalone sync binary ──────────────────────────── */
    printf("\n--- email-sync ---\n");
    RUN_TEST(test_sync_help);
    RUN_TEST(test_sync_run);
    RUN_TEST(test_sync_unknown_opt);
    RUN_TEST(test_sync_no_config);

    /* ── email-tui: full interactive TUI + future write operations ─────── */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_tui_bin);

    printf("\n--- email-tui: help pages ---\n");
    RUN_TEST(test_tui_help_general);
    RUN_TEST(test_tui_help_list);
    RUN_TEST(test_tui_help_cron);

    printf("\n--- email-tui: batch mode ---\n");
    restart_mock();
    RUN_TEST(test_batch_list);
    RUN_TEST(test_batch_list_all);
    RUN_TEST(test_batch_show);
    RUN_TEST(test_batch_folders_flat);

    printf("\n--- email-tui: interactive ---\n");
    RUN_TEST(test_interactive_list_content);
    RUN_TEST(test_interactive_list_esc_quit);
    RUN_TEST(test_interactive_show_esc_to_list);

    printf("\n--- email-tui: TUI launch ---\n");
    restart_mock();
    RUN_TEST(test_tui_interactive_launch);

    printf("\n--- email-tui: wizard + cron ---\n");
    RUN_TEST(test_wizard_abort);
    RUN_TEST(test_cron_status_not_found);

    /* Restore full email-cli binary */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", argv[1]);

done:
    stop_mock_server();
    if (g_old_home[0]) setenv("HOME", g_old_home, 1);

    printf("\n--- PTY Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
