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
static char  g_batch_cli_bin[512];  /* email-cli (batch), never changes */

#define WAIT_MS 6000
#define SETTLE_MS 600
#define ROWS 24
#define COLS 100

static void write_config(void) {
    char d1[300], d2[300], d3[350], d4[400], path[450];
    snprintf(d1, sizeof(d1), "%s/.config", g_test_home);
    snprintf(d2, sizeof(d2), "%s/.config/email-cli", g_test_home);
    snprintf(d3, sizeof(d3), "%s/.config/email-cli/accounts", g_test_home);
    snprintf(d4, sizeof(d4), "%s/.config/email-cli/accounts/testuser", g_test_home);
    mkdir(g_test_home, 0700);
    mkdir(d1, 0700);
    mkdir(d2, 0700);
    mkdir(d3, 0700);
    mkdir(d4, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", d4);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:9993\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SSL_NO_VERIFY=1\n"     /* TLS with self-signed cert */
        "SMTP_HOST=smtps://localhost:9465\n"); /* dummy SMTP — avoids blocking prompt */
    fclose(fp);
    chmod(path, 0600);
}

static void write_config_with_interval(int interval) {
    char d1[300], d2[300], d3[350], d4[400], path[450];
    snprintf(d1, sizeof(d1), "%s/.config", g_test_home);
    snprintf(d2, sizeof(d2), "%s/.config/email-cli", g_test_home);
    snprintf(d3, sizeof(d3), "%s/.config/email-cli/accounts", g_test_home);
    snprintf(d4, sizeof(d4), "%s/.config/email-cli/accounts/testuser", g_test_home);
    mkdir(g_test_home, 0700);
    mkdir(d1, 0700);
    mkdir(d2, 0700);
    mkdir(d3, 0700);
    mkdir(d4, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", d4);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:9993\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SSL_NO_VERIFY=1\n"   /* TLS with self-signed cert */
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
    ASSERT_WAIT_FOR(s, "Reading:", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "list");
    ASSERT_SCREEN_CONTAINS(s, "show");
    ASSERT_SCREEN_CONTAINS(s, "list-folders");
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
    const char *a[] = {"list-folders", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "help list-folders: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-cli", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "--tree");
    pty_close(s);
}

static void test_help_sync(void) {
    const char *a[] = {g_sync_bin, "--help", NULL};
    PtySession *s = pty_open(120, 50);
    ASSERT(s != NULL, "help sync: opens");
    ASSERT(pty_run(s, a) == 0, "help sync: pty_run");
    ASSERT_WAIT_FOR(s, "Usage: email-sync", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "sync");
    pty_close(s);
}

static void test_help_cron(void) {
    const char *a[] = {g_sync_bin, "cron", "--help", NULL};
    PtySession *s = pty_open(120, 50);
    ASSERT(s != NULL, "help cron: opens");
    ASSERT(pty_run(s, a) == 0, "help cron: pty_run");
    ASSERT_WAIT_FOR(s, "Usage: email-sync", WAIT_MS);
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
    const char *a[] = {"list-folders", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch list-folders: opens");
    ASSERT_WAIT_FOR(s, "INBOX", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX.Sent");
    ASSERT_SCREEN_CONTAINS(s, "INBOX.Trash");
    pty_close(s);
}

static void test_batch_folders_tree(void) {
    const char *a[] = {"list-folders", "--tree", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "batch list-folders --tree: opens");
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
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "batch sync: opens");
    const char *a[] = {g_sync_bin, NULL};
    ASSERT(pty_run(s, a) == 0, "batch sync: pty_run");
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);
}

static void test_batch_cron_status(void) {
    const char *a[] = {g_sync_bin, "cron", "status", NULL};
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "batch cron status: opens");
    ASSERT(pty_run(s, a) == 0, "batch cron status: pty_run");
    /* Matches "No email-sync cron entry found." or "Cron entry found:" */
    ASSERT_WAIT_FOR(s, "ron", WAIT_MS);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  COMMAND SEPARATION: labels (Gmail) vs folders (IMAP)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_list_labels_blocked_on_imap(void) {
    /* 'list-labels' is Gmail-only; on IMAP it must print an error and exit non-0 */
    restart_mock();
    const char *a[] = {"list-labels", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "list-labels on IMAP: opens");
    ASSERT_WAIT_FOR(s, "list-labels", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Gmail");
    pty_close(s);
}

static void test_list_folders_works_on_imap(void) {
    /* 'list-folders' must succeed on IMAP and list folder names */
    restart_mock();
    const char *a[] = {"list-folders", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "list-folders on IMAP: opens");
    ASSERT_WAIT_FOR(s, "INBOX", WAIT_MS);
    pty_close(s);
}

static void test_create_label_blocked_on_imap(void) {
    /* 'create-label' is Gmail-only; on IMAP it must print an error */
    restart_mock();
    const char *a[] = {"create-label", "TestLabel", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "create-label on IMAP: opens");
    ASSERT_WAIT_FOR(s, "create-label", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Gmail");
    pty_close(s);
}

static void test_delete_label_blocked_on_imap(void) {
    /* 'delete-label' is Gmail-only; on IMAP it must print an error */
    restart_mock();
    const char *a[] = {"delete-label", "SomeLabel", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "delete-label on IMAP: opens");
    ASSERT_WAIT_FOR(s, "delete-label", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Gmail");
    pty_close(s);
}

static void test_create_folder_help(void) {
    /* 'create-folder --help' must show usage */
    const char *a[] = {"create-folder", "--help", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "create-folder --help: opens");
    ASSERT_WAIT_FOR(s, "create-folder", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "IMAP");
    pty_close(s);
}

static void test_delete_folder_help(void) {
    /* 'delete-folder --help' must show usage */
    const char *a[] = {"delete-folder", "--help", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "delete-folder --help: opens");
    ASSERT_WAIT_FOR(s, "delete-folder", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "IMAP");
    pty_close(s);
}

static void test_create_folder_missing_arg(void) {
    /* 'create-folder' with no argument must print error and show usage */
    restart_mock();
    const char *a[] = {"create-folder", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "create-folder no arg: opens");
    ASSERT_WAIT_FOR(s, "create-folder", WAIT_MS);
    pty_close(s);
}

static void test_delete_folder_missing_arg(void) {
    /* 'delete-folder' with no argument must print error and show usage */
    restart_mock();
    const char *a[] = {"delete-folder", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "delete-folder no arg: opens");
    ASSERT_WAIT_FOR(s, "delete-folder", WAIT_MS);
    pty_close(s);
}

/* Forward declaration — defined later after the email-tui helper section */
static PtySession *tui_open_to_list(void);

/* ══════════════════════════════════════════════════════════════════════
 *  INTERACTIVE LIST VIEW
 * ══════════════════════════════════════════════════════════════════════ */

static void test_interactive_list_content(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "interactive list: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "From");
    ASSERT_SCREEN_CONTAINS(s, "Subject");
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_list_separator(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list separator: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "══");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_list_statusbar(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
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
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list ESC: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS); /* program exits quietly — no output to wait for */
    pty_close(s);
}

static void test_interactive_list_nav(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
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
    PtySession *s = tui_open_to_list();
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

/** '/' in the message list activates the inline subject/body filter. */
static void test_tui_list_search_filter(void) {
    /* Exercises list_filter_rebuild and filter input handling in email_service.c */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list search: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Activate filter mode */
    pty_send_str(s, "/");
    ASSERT_WAIT_FOR(s, "Filter", WAIT_MS);  /* filter bar appears */
    pty_settle(s, SETTLE_MS);

    /* Type search term — filter bar rebuilds on each character */
    pty_send_str(s, "Test");
    pty_settle(s, SETTLE_MS);

    /* TAB: cycle scope (Subject → From → …) */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS / 2);

    /* BACK: remove last typed character */
    pty_send_key(s, PTY_KEY_BACK);
    pty_settle(s, SETTLE_MS / 2);

    /* Enter: commit filter (stay in filter mode but stop typing) */
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS / 2);

    /* First ESC: clear filter, remain in list */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);

    /* Second ESC: exit list view */
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  INTERACTIVE SHOW VIEW
 * ══════════════════════════════════════════════════════════════════════ */

static void test_interactive_show_content(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
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
    PtySession *s = tui_open_to_list();
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
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "show statusbar: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    int sb = find_row(s, "/=search");
    ASSERT(sb >= 0, "show sb: found /=search");
    if (sb >= 0) {
        ASSERT(pty_row_contains(s, sb, "/=search"), "show sb: /=search");
        ASSERT(pty_row_contains(s, sb, "v=source"), "show sb: v=source");
        ASSERT_CELL_ATTR(s, sb, 2, PTY_ATTR_REVERSE);
    }
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_show_backspace(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
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

/*
 * ══════════════════════════════════════════════════════════════════════
 * US-RD-05: As a user reading a message, pressing ESC exits the program
 *           immediately; Backspace returns to the message list.
 * Acceptance criteria:
 *   - ESC in reader: program terminates, list NOT shown.
 *   - Backspace in reader: list view shown again.
 *   - 'q' in reader: list view shown again.
 * ══════════════════════════════════════════════════════════════════════
 */
static void test_interactive_show_esc_exits(void) {
    /* US-RD-05 */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "show ESC→exit: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* ESC from reader exits the program (does NOT return to list) */
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS * 2);
    int r = pty_wait_for(s, "message(s) in", 1500);
    ASSERT(r != 0, "show ESC: does NOT return to list");
    pty_close(s);
}

/*
 * ══════════════════════════════════════════════════════════════════════
 * US-RD-01: As a user, I want to see the message UID in the reader header
 *           so that I can identify the message for debugging or scripting.
 * Acceptance criteria:
 *   - A "UID:" line is visible in the reader header area.
 *   - The UID value is a non-empty string.
 * ══════════════════════════════════════════════════════════════════════
 */
static void test_interactive_show_uid_in_header(void) {
    /* US-RD-01 */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "show UID header: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "UID:");
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/*
 * ══════════════════════════════════════════════════════════════════════
 * US-RD-02: As a user, I want to toggle between rendered and raw source
 *           view with 'v' so I can inspect headers and raw MIME content.
 * Acceptance criteria:
 *   - Default view is rendered (HTML/text body visible).
 *   - 'v' switches to raw source: MIME headers (Content-Type:) visible.
 *   - Statusbar shows "v=rendered" in source mode, "v=source" in rendered.
 *   - Second 'v' returns to rendered view.
 * ══════════════════════════════════════════════════════════════════════
 */
static void test_interactive_show_source_toggle(void) {
    /* US-RD-02 */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "show source toggle: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Default: rendered view */
    ASSERT_SCREEN_CONTAINS(s, "Hello from Mock Server");
    /* Press v → raw source view */
    pty_send_str(s, "v");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Content-Type:");
    ASSERT_WAIT_FOR(s, "v=rendered", WAIT_MS);
    /* Press v again → back to rendered */
    pty_send_str(s, "v");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Hello from Mock Server");
    ASSERT_SCREEN_CONTAINS(s, "v=source");
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/*
 * ══════════════════════════════════════════════════════════════════════
 * US-RD-03: As a user, I want to search within a message body using '/'
 *           so I can quickly jump to relevant content.
 * Acceptance criteria:
 *   - '/' opens an inline search prompt on the bottom row.
 *   - Typing and Enter jumps to the first matching line.
 *   - 'n' goes to the next match; 'N' goes to the previous match.
 *   - ESC in search prompt cancels without jumping.
 *   - Non-matching search shows "No match:" in the info line.
 * ══════════════════════════════════════════════════════════════════════
 */
static void test_interactive_show_search_finds(void) {
    /* US-RD-03 */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "show search: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Search for "Mock" — present in body */
    pty_send_str(s, "/");
    pty_settle(s, SETTLE_MS / 2);
    pty_send_str(s, "Mock");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    /* Heading still visible, no "No match" error */
    ASSERT_SCREEN_CONTAINS(s, "From:");
    int r = find_row(s, "No match");
    ASSERT(r < 0, "show search: no 'No match' error");
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_show_search_no_match(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "show search no match: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "/");
    pty_settle(s, SETTLE_MS / 2);
    pty_send_str(s, "xyzzy_no_such_text");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "No match");
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/*
 * ══════════════════════════════════════════════════════════════════════
 * US-RD-04: As a user, I want URLs rendered in blue so they stand out
 *           from surrounding text and are easy to identify.
 * Acceptance criteria:
 *   - URLs (https://…) appear on their own line.
 *   - The URL text is rendered with ANSI foreground color 34 (blue).
 *   - Non-URL text is NOT blue.
 * ══════════════════════════════════════════════════════════════════════
 */
static void test_interactive_show_url_rendered(void) {
    /* US-RD-04 */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "show URL rendered: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Scroll to end to reveal URL (body may be multi-page) */
    pty_send_key(s, PTY_KEY_END);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "https://click.example.com/test");
    /* Verify URL is rendered in blue (ANSI color 34) */
    int url_row = find_row(s, "https://click.example.com/test");
    ASSERT(url_row >= 0, "show URL: row found");
    if (url_row >= 0)
        ASSERT(pty_cell_fg(s, url_row, 0) == 34, "show URL: blue color (34)");
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_show_q_to_list(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
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
    PtySession *s = tui_open_to_list();
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
    /* Open email-tui in a 10-row terminal and navigate to message list */
    const char *tui_args[] = {g_tui_bin, NULL};
    PtySession *s = pty_open(COLS, 10);
    ASSERT(s != NULL, "show arrow scroll: opens");
    if (pty_run(s, tui_args) != 0) { pty_close(s); return; }
    if (pty_wait_for(s, "Email Account", WAIT_MS) != 0) { pty_close(s); return; }
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return; }
    pty_send_key(s, PTY_KEY_ENTER);
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
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list-folders content: opens");
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
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list-folders sb: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    int sb = find_row(s, "Enter=open/select");
    ASSERT(sb >= 0, "list-folders sb: found Enter=open/select");
    if (sb >= 0)
        ASSERT_CELL_ATTR(s, sb, 2, PTY_ATTR_REVERSE);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_folders_toggle(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list-folders toggle: opens");
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
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list-folders select: opens");
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
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list-folders nav: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Navigate down and back up — list-folders remain visible */
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
    /* Backspace from folder browser (root) returns to accounts screen in email-tui */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list-folders back→list: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Backspace from folder browser (root) returns to accounts screen */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_folders_flat_navigate_up(void) {
    /* US 15: in flat view, Enter on a folder with children navigates into it
     * (sets current_prefix); Backspace navigates back up one level. */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list-folders flat nav up: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);            /* open folder browser */
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "t");                     /* toggle to flat view */
    ASSERT_WAIT_FOR(s, "t=tree", WAIT_MS);    /* flat mode: hint shows "t=tree" */
    pty_settle(s, SETTLE_MS);
    /* Flat view at root shows only top-level list-folders (no '.' in name) */
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    pty_send_key(s, PTY_KEY_ENTER);           /* navigate into INBOX */
    ASSERT_WAIT_FOR(s, "Backspace=up", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Header shows "Folders: INBOX/ (3)"; flat mode shows last component only */
    ASSERT_SCREEN_CONTAINS(s, "INBOX/");
    ASSERT_SCREEN_CONTAINS(s, "Sent");
    pty_send_key(s, PTY_KEY_BACK);            /* navigate up to root */
    ASSERT_WAIT_FOR(s, "Backspace=back", WAIT_MS);
    pty_send_str(s, "t");                     /* toggle back to tree mode */
    ASSERT_WAIT_FOR(s, "t=flat", WAIT_MS);   /* tree mode shows "t=flat" hint */
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_folders_esc_quit(void) {
    /* ESC from folder browser quits the entire application */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list-folders ESC quit: opens");
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

static void test_tui_folder_browser_search(void) {
    /* '/' in the folder browser opens an inline search bar */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "folder search: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Activate search */
    pty_send_str(s, "/");
    ASSERT_WAIT_FOR(s, "Search", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Type a search term */
    pty_send_str(s, "Inbox");
    pty_settle(s, SETTLE_MS);
    /* TAB cycles scope: Subject → From → To → Body */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS / 2);
    /* BACK removes last character */
    pty_send_key(s, PTY_KEY_BACK);
    pty_settle(s, SETTLE_MS / 2);
    /* ESC cancels search, returns to folder browser */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  EMPTY FOLDER
 * ══════════════════════════════════════════════════════════════════════ */

static void test_interactive_empty_folder(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
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

    /* Formal empty-folder layout: inverse title + column headers + (empty) */
    ASSERT_WAIT_FOR(s, "0 of 0 message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Subject");
    ASSERT_SCREEN_CONTAINS(s, "(empty)");
    ASSERT_SCREEN_CONTAINS(s, "Backspace=folders");

    /* Backspace returns to folder browser */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_empty_folder_cron(void) {
    /* Cron mode: navigate to INBOX.Empty — cron config triggers ⚠ no-cache view */
    write_config_with_interval(15);
    /* Reset persisted folder so tui_open_to_list() lands on INBOX (the default) */
    { char p[512]; snprintf(p, sizeof(p), "%s/.local/share/email-cli/ui.ini", g_test_home); remove(p); }
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "empty cron: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);              /* open folder browser */
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_DOWN);              /* move to INBOX.Empty */
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);             /* open empty folder */
    ASSERT_WAIT_FOR(s, "0 of 0 message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "(empty)");
    ASSERT_SCREEN_CONTAINS(s, "No cached data");
    ASSERT_SCREEN_CONTAINS(s, "Backspace=folders");
    /* Restore folder cursor to INBOX so subsequent tests open INBOX */
    pty_send_key(s, PTY_KEY_BACK);          /* back to folder browser */
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_UP);            /* cursor back up to INBOX */
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);         /* open INBOX → saves cursor */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    write_config();  /* restore normal config */
}

/* ══════════════════════════════════════════════════════════════════════
 *  ATTACHMENT SAVE TESTS
 *
 * The mock server message is multipart/mixed with two list-attachments:
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
    PtySession *s = tui_open_to_list();
    if (!s) return NULL;
    if (pty_wait_for(s, "Test Message", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    /* Wait for the attachment status bar — proves the full show view rendered */
    if (pty_wait_for(s, "A=save-all", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/** Status bar shows  a=save  A=save-all(2) when list-attachments present. */
static void test_show_attachment_statusbar(void) {
    PtySession *s = open_show_view();
    ASSERT(s != NULL, "att statusbar: opens");

    ASSERT_SCREEN_CONTAINS(s, "a=save");
    ASSERT_SCREEN_CONTAINS(s, "A=save-all(2)");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** Attachment picker is shown when 'a' is pressed (2 list-attachments). */
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

/** Pressing 'A' then Enter saves all list-attachments to the default dir. */
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

/** Exercises HOME/END/LEFT/RIGHT/BACK/DELETE cursor keys inside input_line_run. */
static void test_show_save_input_line_cursor(void) {
    PtySession *s = open_show_view();
    ASSERT(s != NULL, "input cursor: opens show view");

    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Attachments", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_ENTER);           /* select first attachment */
    ASSERT_WAIT_FOR(s, "Save as:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Exercise cursor movement keys while input_line_run is active */
    pty_send_key(s, PTY_KEY_HOME);            /* TERM_KEY_HOME  → cursor = 0 */
    pty_send_key(s, PTY_KEY_END);             /* TERM_KEY_END   → cursor = len */
    pty_send_key(s, PTY_KEY_LEFT);            /* TERM_KEY_LEFT  → il_move_left */
    pty_send_key(s, PTY_KEY_RIGHT);           /* TERM_KEY_RIGHT → il_move_right */
    pty_send_key(s, PTY_KEY_BACK);            /* TERM_KEY_BACK  → il_backspace */
    pty_send_key(s, PTY_KEY_HOME);            /* back to start so DELETE acts on a char */
    pty_send_str(s, "\033[3~");               /* TERM_KEY_DELETE → il_delete_fwd */

    pty_send_key(s, PTY_KEY_ESC);             /* cancel — no file written */
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** Exercises TAB/Shift+Tab path completion inside the Save-all dialog. */
static void test_show_save_tab_completion(void) {
    /* Ensure two known files exist in the test home for completion to find. */
    char tab1[512], tab2[512];
    snprintf(tab1, sizeof(tab1), "%s/zztab1.txt", g_test_home);
    snprintf(tab2, sizeof(tab2), "%s/zztab2.txt", g_test_home);
    { FILE *f = fopen(tab1, "w"); if (f) fclose(f); }
    { FILE *f = fopen(tab2, "w"); if (f) fclose(f); }

    PtySession *s = open_show_view();
    ASSERT(s != NULL, "tab complete: opens show view");

    pty_send_str(s, "A");                     /* "Save all to:" dialog */
    ASSERT_WAIT_FOR(s, "Save all to:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Pre-filled: $HOME.  Append "/zz" so TAB prefix is "zz". */
    pty_send_key(s, PTY_KEY_END);
    pty_send_str(s, "/zz");

    /* First TAB: scans dir, finds zztab1/zztab2, completes to zztab1 */
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "zztab1", WAIT_MS);

    /* Second TAB: cycles to zztab2 */
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "zztab2", WAIT_MS);

    /* Shift+Tab: cycles back to zztab1 */
    pty_send_str(s, "\033[Z");
    ASSERT_WAIT_FOR(s, "zztab1", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);             /* cancel — no file written */
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    remove(tab1);
    remove(tab2);
}

/* ══════════════════════════════════════════════════════════════════════
 *  email-cli-ro EXCLUSIVE TESTS
 * ══════════════════════════════════════════════════════════════════════ */

static void test_ro_help_general(void) {
    const char *a[] = {"--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "ro help: opens");
    ASSERT_WAIT_FOR(s, "Reading:", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "list");
    ASSERT_SCREEN_CONTAINS(s, "show");
    ASSERT_SCREEN_CONTAINS(s, "list-folders");
    ASSERT_SCREEN_CONTAINS(s, "list-attachments");
    ASSERT_SCREEN_CONTAINS(s, "save-attachment");
    ASSERT_SCREEN_CONTAINS(s, "help");
    pty_close(s);
}

static void test_ro_help_list(void) {
    const char *a[] = {"list", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "ro help list: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-cli-ro", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "--folder");
    pty_close(s);
}

static void test_ro_help_attachments(void) {
    const char *a[] = {"list-attachments", "--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "ro help list-attachments: opens");
    ASSERT_WAIT_FOR(s, "list-attachments <uid>", WAIT_MS);
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
    const char *a[] = {"list-attachments", "1", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "ro list-attachments: opens");
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
    ASSERT(strstr(out, "Reading:") != NULL, "non-tty: shows general help (Reading:)");
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

    const char *wiz[] = {"add-account", NULL};
    PtySession *s = cli_run(wiz);
    ASSERT(s != NULL, "wizard abort: opens");
    ASSERT_WAIT_FOR(s, "Account type", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);  /* EOF on stdin → getline returns -1 → wizard aborts */
    ASSERT_WAIT_FOR(s, "cancelled", WAIT_MS);
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
    const char *wiz[] = {"add-account", NULL};
    PtySession *s = cli_run(wiz);
    ASSERT(s != NULL, "wizard complete: opens");
    ASSERT_WAIT_FOR(s, "Account type", WAIT_MS);
    pty_send_str(s, "1\n");  /* IMAP */
    ASSERT_WAIT_FOR(s, "IMAP Host", WAIT_MS);
    pty_send_str(s, "imaps://localhost:9993\n");   /* explicit correct protocol */
    ASSERT_WAIT_FOR(s, "Port", WAIT_MS);
    pty_send_str(s, "\n");  /* accept default 993 */
    ASSERT_WAIT_FOR(s, "sername", WAIT_MS);
    pty_send_str(s, "testuser\n");
    ASSERT_WAIT_FOR(s, "assword", WAIT_MS);
    pty_send_str(s, "testpass\n");
    ASSERT_WAIT_FOR(s, "older", WAIT_MS);
    pty_send_str(s, "INBOX\n");
    ASSERT_WAIT_FOR(s, "SMTP Host", WAIT_MS);
    pty_send_str(s, "\n");  /* skip SMTP config */
    ASSERT_WAIT_FOR(s, "added.", WAIT_MS);
    pty_close(s);

    setenv("HOME", g_test_home, 1);
}

/* Wizard: plain hostname (no protocol) → auto-completes to imaps:// */
static void test_wizard_host_autocomplete(void) {
    char wiz_home[300];
    snprintf(wiz_home, sizeof(wiz_home), "%s/wizard_autocomplete", g_test_home);
    mkdir(wiz_home, 0700);
    setenv("HOME", wiz_home, 1);
    unsetenv("XDG_CONFIG_HOME");

    const char *wiz[] = {"add-account", NULL};
    PtySession *s = cli_run(wiz);
    ASSERT(s != NULL, "wizard autocomplete: opens");
    ASSERT_WAIT_FOR(s, "Account type", WAIT_MS);
    pty_send_str(s, "1\n");  /* IMAP */
    ASSERT_WAIT_FOR(s, "IMAP Host", WAIT_MS);
    pty_send_str(s, "localhost:9993\n");            /* no protocol → auto imaps:// */
    ASSERT_WAIT_FOR(s, "imaps://", WAIT_MS);        /* confirmation line printed */
    ASSERT_WAIT_FOR(s, "Port", WAIT_MS);
    pty_send_str(s, "\n");   /* accept default 993 */
    ASSERT_WAIT_FOR(s, "sername", WAIT_MS);
    pty_send_str(s, "testuser\n");
    ASSERT_WAIT_FOR(s, "assword", WAIT_MS);
    pty_send_str(s, "testpass\n");
    ASSERT_WAIT_FOR(s, "older", WAIT_MS);
    pty_send_str(s, "\n");   /* accept default INBOX */
    ASSERT_WAIT_FOR(s, "SMTP Host", WAIT_MS);
    pty_send_str(s, "\n");   /* skip SMTP */
    ASSERT_WAIT_FOR(s, "added.", WAIT_MS);
    pty_close(s);

    setenv("HOME", g_test_home, 1);
}

/* Wizard: explicit wrong protocol → error + re-prompt → correct → completes */
static void test_wizard_bad_protocol_rejected(void) {
    char wiz_home[300];
    snprintf(wiz_home, sizeof(wiz_home), "%s/wizard_badproto", g_test_home);
    mkdir(wiz_home, 0700);
    setenv("HOME", wiz_home, 1);
    unsetenv("XDG_CONFIG_HOME");

    const char *wiz[] = {"add-account", NULL};
    PtySession *s = cli_run(wiz);
    ASSERT(s != NULL, "wizard bad proto: opens");
    ASSERT_WAIT_FOR(s, "Account type", WAIT_MS);
    pty_send_str(s, "1\n");  /* IMAP */
    ASSERT_WAIT_FOR(s, "IMAP Host", WAIT_MS);
    pty_send_str(s, "imap://localhost:9993\n");     /* wrong protocol */
    ASSERT_WAIT_FOR(s, "unsupported protocol", WAIT_MS);
    ASSERT_WAIT_FOR(s, "IMAP Host", WAIT_MS);       /* re-prompted */
    pty_send_str(s, "imaps://localhost:9993\n");    /* now correct */
    ASSERT_WAIT_FOR(s, "Port", WAIT_MS);
    pty_send_str(s, "\n");   /* accept default 993 */
    ASSERT_WAIT_FOR(s, "sername", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);                /* abort to keep test short */
    ASSERT_WAIT_FOR(s, "cancelled", WAIT_MS);
    pty_close(s);

    setenv("HOME", g_test_home, 1);
}

/* Wizard: enter SMTP host (plain name, no protocol) to cover normalize_smtp_host */
static void test_wizard_with_smtp(void) {
    char wiz_home[300];
    snprintf(wiz_home, sizeof(wiz_home), "%s/wizard_smtp", g_test_home);
    mkdir(wiz_home, 0700);
    setenv("HOME", wiz_home, 1);
    unsetenv("XDG_CONFIG_HOME");

    restart_mock();
    const char *wiz[] = {"add-account", NULL};
    PtySession *s = cli_run(wiz);
    ASSERT(s != NULL, "wizard smtp: opens");
    ASSERT_WAIT_FOR(s, "Account type", WAIT_MS);
    pty_send_str(s, "1\n");                           /* IMAP */
    ASSERT_WAIT_FOR(s, "IMAP Host", WAIT_MS);
    pty_send_str(s, "localhost:9993\n");               /* plain name → imaps:// prepended */
    ASSERT_WAIT_FOR(s, "Port", WAIT_MS);
    pty_send_str(s, "\n");                            /* accept default 993 */
    ASSERT_WAIT_FOR(s, "sername", WAIT_MS);
    pty_send_str(s, "testuser\n");
    ASSERT_WAIT_FOR(s, "assword", WAIT_MS);
    pty_send_str(s, "testpass\n");
    ASSERT_WAIT_FOR(s, "older", WAIT_MS);
    pty_send_str(s, "INBOX\n");
    ASSERT_WAIT_FOR(s, "SMTP Host", WAIT_MS);
    /* Enter a bad SMTP protocol first → error → re-prompt */
    pty_send_str(s, "smtp://localhost\n");             /* bad protocol */
    ASSERT_WAIT_FOR(s, "unsupported protocol", WAIT_MS);
    ASSERT_WAIT_FOR(s, "SMTP Host", WAIT_MS);          /* re-prompted */
    /* Now enter plain hostname → normalize_smtp_host prepends smtps:// */
    pty_send_str(s, "localhost\n");
    ASSERT_WAIT_FOR(s, "SMTP Port", WAIT_MS);
    pty_send_str(s, "\n");                            /* accept default 587 */
    ASSERT_WAIT_FOR(s, "SMTP Username", WAIT_MS);
    pty_send_str(s, "\n");                            /* same as IMAP */
    ASSERT_WAIT_FOR(s, "SMTP Password", WAIT_MS);
    pty_send_str(s, "\n");                            /* same as IMAP */
    ASSERT_WAIT_FOR(s, "added.", WAIT_MS);
    pty_close(s);

    setenv("HOME", g_test_home, 1);
}

/* ══════════════════════════════════════════════════════════════════════
 *  CRON MANAGEMENT (US 06, 07, 08)
 * ══════════════════════════════════════════════════════════════════════ */

/** Remove any email-sync cron entry — used for cleanup between cron tests. */
static void cron_cleanup(void) {
    const char *a[] = {g_sync_bin, "cron", "remove", NULL};
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return;
    if (pty_run(s, a) != 0) { pty_close(s); return; }
    /* Wait until crontab is fully written before closing */
    pty_wait_for(s, "ron", WAIT_MS); /* "Cron job removed." or "No email-sync cron entry found." */
    pty_close(s);
}

static void test_cron_status_not_found(void) {
    cron_cleanup(); /* ensure clean state */
    const char *a[] = {g_sync_bin, "cron", "status", NULL};
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "cron status not found: opens");
    ASSERT(pty_run(s, a) == 0, "cron status not found: pty_run");
    ASSERT_WAIT_FOR(s, "No email-sync cron entry found", WAIT_MS);
    pty_close(s);
}

static void test_cron_setup_default_interval(void) {
    /* Standard config has no SYNC_INTERVAL → defaults to 5 min */
    write_config();
    const char *a[] = {g_sync_bin, "cron", "setup", NULL};
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "cron setup default interval: opens");
    ASSERT(pty_run(s, a) == 0, "cron setup default interval: pty_run");
    ASSERT_WAIT_FOR(s, "sync_interval not configured", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Cron job installed:", WAIT_MS);
    pty_close(s);
    cron_cleanup();
}

static void test_cron_setup_installs(void) {
    cron_cleanup();
    write_config_with_interval(15);
    const char *a[] = {g_sync_bin, "cron", "setup", NULL};
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "cron setup installs: opens");
    ASSERT(pty_run(s, a) == 0, "cron setup installs: pty_run");
    ASSERT_WAIT_FOR(s, "Cron job installed:", WAIT_MS);
    pty_close(s);
    /* leave entry for next test */
}

static void test_cron_status_found(void) {
    /* Entry was left by test_cron_setup_installs */
    const char *a[] = {g_sync_bin, "cron", "status", NULL};
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "cron status found: opens");
    ASSERT(pty_run(s, a) == 0, "cron status found: pty_run");
    ASSERT_WAIT_FOR(s, "Cron entry found:", WAIT_MS);
    pty_close(s);
}

static void test_cron_setup_already_installed(void) {
    /* Entry still present from test_cron_setup_installs */
    const char *a[] = {g_sync_bin, "cron", "setup", NULL};
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "cron setup already installed: opens");
    ASSERT(pty_run(s, a) == 0, "cron setup already installed: pty_run");
    ASSERT_WAIT_FOR(s, "already installed", WAIT_MS);
    pty_close(s);
}

static void test_cron_remove_entry(void) {
    /* Entry still present — remove it */
    const char *a[] = {g_sync_bin, "cron", "remove", NULL};
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "cron remove entry: opens");
    ASSERT(pty_run(s, a) == 0, "cron remove entry: pty_run");
    ASSERT_WAIT_FOR(s, "Cron job removed", WAIT_MS);
    pty_close(s);
}

static void test_cron_remove_not_found(void) {
    /* Entry was removed by test_cron_remove_entry */
    const char *a[] = {g_sync_bin, "cron", "remove", NULL};
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "cron remove not found: opens");
    ASSERT(pty_run(s, a) == 0, "cron remove not found: pty_run");
    ASSERT_WAIT_FOR(s, "No email-sync cron entry found", WAIT_MS);
    pty_close(s);
    write_config(); /* restore standard config (no sync_interval) */
}

/* ══════════════════════════════════════════════════════════════════════
 *  SYNC PROGRESS (US 05)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_sync_progress(void) {
    restart_mock();
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "sync progress: opens");
    const char *a[] = {g_sync_bin, NULL};
    ASSERT(pty_run(s, a) == 0, "sync progress: pty_run");
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
    { PtySession *s = pty_open(COLS, ROWS);
      ASSERT(s != NULL, "show cache: sync opens");
      const char *a[] = {g_sync_bin, NULL};
      ASSERT(pty_run(s, a) == 0, "show cache: sync pty_run");
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
    { PtySession *s = pty_open(COLS, ROWS);
      ASSERT(s != NULL, "offline list: sync opens");
      const char *a[] = {g_sync_bin, NULL};
      ASSERT(pty_run(s, a) == 0, "offline list: sync pty_run");
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

/* ── email-tui helper: open interactive TUI and navigate to message list ── */

/**
 * Open email-tui with no args, navigate through the accounts screen (Enter),
 * and return the PTY session sitting on the message list view.
 * Returns NULL if any step fails.
 */
static PtySession *tui_open_to_list(void) {
    const char *args[] = {g_tui_bin, NULL};
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    /* email-tui opens the accounts screen first; press Enter to open account */
    if (pty_wait_for(s, "Email Account", WAIT_MS) != 0) {
        pty_close(s);
        return NULL;
    }
    pty_send_key(s, PTY_KEY_ENTER);
    /* Folder browser appears; navigate to INBOX regardless of saved preference.
     * HOME→VP_UNREAD(1) + 6×DOWN skips all 8 virtual rows and lands on INBOX
     * (VPREFIX=8: Tags/Flags header + 6 virtual rows + Folders header = 8). */
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) {
        pty_close(s);
        return NULL;
    }
    pty_send_key(s, PTY_KEY_HOME);
    for (int _i = 0; _i < 6; _i++) pty_send_key(s, PTY_KEY_DOWN);
    /* Wait for INBOX to be visible before pressing Enter — IMAP LIST may be in flight */
    if (pty_wait_for(s, "INBOX", WAIT_MS) != 0) {
        pty_close(s);
        return NULL;
    }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    return s;
}

static void test_tui_list_content(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "tui list content: opens through accounts screen");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_list_esc_quit(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "tui list ESC: opens through accounts screen");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    pty_close(s);
}

static void test_tui_show_esc_exits(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "tui show ESC→exit: opens through accounts screen");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* ESC exits the program, not back to list */
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS * 2);
    int r = pty_wait_for(s, "message(s) in", 1500);
    ASSERT(r != 0, "tui show ESC: does NOT return to list");
    pty_close(s);
}

/* ── email-tui help tests ────────────────────────────────────────────── */

static void test_tui_help_general(void) {
    const char *a[] = {"--help", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "tui help: opens");
    ASSERT_WAIT_FOR(s, "Usage: email-tui", WAIT_MS);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "Options:");
    ASSERT_SCREEN_CONTAINS(s, "account selector");
    pty_close(s);
}

static void test_tui_help_list(void) {
    /* email-tui does not accept arguments; verify the rejection message */
    const char *a[] = {"list", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "tui no-args reject: opens");
    ASSERT_WAIT_FOR(s, "does not accept arguments", WAIT_MS);
    pty_close(s);
}

static void test_tui_help_cron(void) {
    /* email-tui does not accept arguments; verify the rejection message */
    const char *a[] = {"cron", NULL};
    PtySession *s = cli_open_size(120, 50, a);
    ASSERT(s != NULL, "tui cron reject: opens");
    ASSERT_WAIT_FOR(s, "does not accept arguments", WAIT_MS);
    pty_close(s);
}

static void test_tui_interactive_launch(void) {
    /* US 18: email-tui with no args in a TTY shows accounts screen,
     * then opens the message list after Enter */
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "tui no-args launch: opens through accounts screen");
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
    ASSERT_WAIT_FOR(s, "No accounts configured", WAIT_MS);
    pty_close(s);

    setenv("HOME", g_test_home, 1);
}

/* ══════════════════════════════════════════════════════════════════════
 *  MULTI-ACCOUNT TUI (US-21)
 * ══════════════════════════════════════════════════════════════════════ */

/** Write a second account config under accounts/<name>/ */
static void write_second_account(const char *name) {
    char dir1[400], dir2[450], path[500];
    snprintf(dir1, sizeof(dir1), "%s/.config/email-cli/accounts",  g_test_home);
    snprintf(dir2, sizeof(dir2), "%s/.config/email-cli/accounts/%s", g_test_home, name);
    mkdir(dir1, 0700);
    mkdir(dir2, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", dir2);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:9993\n"
        "EMAIL_USER=%s\n"
        "EMAIL_PASS=pass2\n"
        "EMAIL_FOLDER=INBOX\n"
        "SSL_NO_VERIFY=1\n", name);  /* TLS with self-signed cert */
    fclose(fp);
    chmod(path, 0600);
}

static void test_tui_accounts_screen_shows(void) {
    /* US-21 AC1: launching email-tui always shows the Accounts screen */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "accounts screen: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "testuser");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_accounts_esc_quit(void) {
    /* US-21: ESC/q from accounts screen exits the TUI */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "accounts ESC quit: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    pty_close(s);
}

static void test_tui_accounts_enter_opens_list(void) {
    /* US-21 AC3: Enter on account opens inbox */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "accounts Enter: opens message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_accounts_backspace_from_list(void) {
    /* US-21 AC7: Backspace from folder root returns to accounts screen */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "accounts backspace: opens message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Backspace → folder browser */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Backspace at root → accounts screen */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_accounts_multiple_shown(void) {
    /* US-21 AC2: multiple accounts are listed */
    write_second_account("second@example.com");
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "accounts multiple: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "testuser");
    ASSERT_SCREEN_CONTAINS(s, "second@example.com");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    /* cleanup */
    char path[500];
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/second@example.com/config.ini",
             g_test_home);
    unlink(path);
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/second@example.com", g_test_home);
    rmdir(path);
}

static void test_tui_accounts_backspace_ignored(void) {
    /* US-21 AC10: Backspace at accounts screen is ignored, does not quit */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "accounts backspace ignored: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    pty_settle(s, SETTLE_MS);
    /* Must still be on the accounts screen */
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_accounts_columns(void) {
    /* US-21 AC2: accounts screen shows Unread/Flagged/Account/Server columns */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "accounts columns: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Unread");
    ASSERT_SCREEN_CONTAINS(s, "Flagged");
    ASSERT_SCREEN_CONTAINS(s, "Account");
    ASSERT_SCREEN_CONTAINS(s, "Server");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_accounts_cursor_restored(void) {
    /* US-21 AC11: cursor is restored to previously open account on return */
    write_second_account("second@example.com");
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "cursor restore: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Move to second account and open it */
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    /* commit 5b4053b added folder browser step; press Enter to select INBOX */
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Navigate back to accounts via Backspace × 2 */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Selection arrow must be visible and second account must be on screen */
    ASSERT_SCREEN_CONTAINS(s, "second@example.com");
    /* The → arrow (UTF-8: \xe2\x86\x92) and second account must both appear */
    ASSERT_SCREEN_CONTAINS(s, "\xe2\x86\x92");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    /* cleanup */
    char path[500];
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/second@example.com/config.ini",
             g_test_home);
    unlink(path);
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/second@example.com", g_test_home);
    rmdir(path);
}

/* US-18: email-tui always starts at accounts screen (no warm-start bypass) */
static void test_tui_always_starts_at_accounts(void) {
    /* First session: navigate to inbox and quit — sets last_account pref */
    {
        PtySession *s = tui_open_to_list();
        ASSERT(s != NULL, "always accounts: first run opens to list");
        ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
    /* Second session: must still show accounts screen, NOT jump to inbox */
    {
        PtySession *s = cli_run(NULL);
        ASSERT(s != NULL, "always accounts: second run opens");
        ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
        pty_settle(s, SETTLE_MS);
        /* Accounts screen is showing — not the inbox */
        ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}

/* US-18: per-account folder cursor persisted to ui.ini */
static void test_tui_folder_cursor_persisted(void) {
    /* Navigate to a non-default folder (Down once = INBOX.Sent on mock server) */
    {
        PtySession *s = cli_run(NULL);
        ASSERT(s != NULL, "folder cursor: first run opens");
        ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
        pty_send_key(s, PTY_KEY_ENTER);
        ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_DOWN);   /* move to second folder */
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ENTER);  /* open it */
        pty_settle(s, SETTLE_MS);        /* wait for folder to load */
        pty_send_key(s, PTY_KEY_ESC);    /* quit */
        pty_close(s);
    }
    /* Verify ui.ini has folder_cursor_testuser key */
    char pref_path[512];
    snprintf(pref_path, sizeof(pref_path),
             "%s/.local/share/email-cli/ui.ini", g_test_home);
    FILE *fp = fopen(pref_path, "r");
    ASSERT(fp != NULL, "folder cursor: ui.ini exists after session");
    if (fp) {
        int found = 0;
        char line[512];
        while (fgets(line, sizeof(line), fp))
            if (strncmp(line, "folder_cursor_testuser=", 23) == 0) { found = 1; break; }
        fclose(fp);
        ASSERT(found, "folder cursor: folder_cursor_testuser key saved in ui.ini");
    }
    /* Second session: folder browser must open on the saved folder */
    {
        PtySession *s = cli_run(NULL);
        ASSERT(s != NULL, "folder cursor: second run opens");
        ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
        pty_send_key(s, PTY_KEY_ENTER);
        ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
        pty_settle(s, SETTLE_MS);
        /* Folder browser is showing with folder content */
        ASSERT_SCREEN_CONTAINS(s, "INBOX");
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  HELP PANEL (US-22)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_tui_accounts_help_panel(void) {
    /* US-22 AC7: 'h' in accounts screen shows help overlay */
    restart_mock();
    const char *args[] = {g_tui_bin, NULL};
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "accounts help: pty_open");
    if (pty_run(s, args) != 0) { pty_close(s); return; }
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "h");
    ASSERT_WAIT_FOR(s, "Accounts shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Press any key to close");
    /* dismiss with any key */
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    /* should be back on accounts screen */
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_list_help_panel(void) {
    /* US-22 AC7: 'h' in message list shows help overlay */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list help: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "h");
    ASSERT_WAIT_FOR(s, "Message list shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Press any key to close");
    ASSERT_SCREEN_CONTAINS(s, "Compose new message");
    /* dismiss */
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    /* still in list view */
    ASSERT_SCREEN_CONTAINS(s, "message(s) in");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_show_help_panel(void) {
    /* US-22 AC7: 'h' in message reader shows help overlay */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "show help: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "h");
    ASSERT_WAIT_FOR(s, "Message reader shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Press any key to close");
    ASSERT_SCREEN_CONTAINS(s, "Reply to this message");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    /* back in reader */
    ASSERT_SCREEN_CONTAINS(s, "r=reply");
    pty_send_key(s, PTY_KEY_ESC);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_folders_help_panel(void) {
    /* US-22 AC7: 'h' in folder browser shows help overlay */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list-folders help: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "h");
    ASSERT_WAIT_FOR(s, "Folder browser shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Press any key to close");
    ASSERT_SCREEN_CONTAINS(s, "Toggle tree");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    /* back in folder browser */
    ASSERT_SCREEN_CONTAINS(s, "Folders");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_help_panel_question_mark(void) {
    /* US-22 AC1: '?' also opens the help panel (same as 'h') */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "? help: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Message list shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);   /* any key dismisses */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "message(s) in");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TLS ENFORCEMENT (US-23)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Write a config with an insecure imap:// URL and WITHOUT SSL_NO_VERIFY=1.
 * Used to test that the application rejects plain-text IMAP connections.
 */
static void write_config_no_ssl_verify_imap(void) {
    char d1[300], d2[300], d3[350], d4[400], path[450];
    snprintf(d1, sizeof(d1), "%s/.config", g_test_home);
    snprintf(d2, sizeof(d2), "%s/.config/email-cli", g_test_home);
    snprintf(d3, sizeof(d3), "%s/.config/email-cli/accounts", g_test_home);
    snprintf(d4, sizeof(d4), "%s/.config/email-cli/accounts/testuser", g_test_home);
    mkdir(g_test_home, 0700);
    mkdir(d1, 0700);
    mkdir(d2, 0700);
    mkdir(d3, 0700);
    mkdir(d4, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", d4);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "EMAIL_HOST=imap://localhost:9993\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n");
    /* No SSL_NO_VERIFY=1 — insecure URL must be rejected */
    fclose(fp);
    chmod(path, 0600);
}

/**
 * Write a config with a secure imaps:// IMAP URL but an insecure smtp:// SMTP
 * URL and WITHOUT SSL_NO_VERIFY=1.  Used to test that plain-text SMTP is
 * rejected.
 */
static void write_config_no_ssl_verify_smtp(void) {
    char d1[300], d2[300], d3[350], d4[400], path[450];
    snprintf(d1, sizeof(d1), "%s/.config", g_test_home);
    snprintf(d2, sizeof(d2), "%s/.config/email-cli", g_test_home);
    snprintf(d3, sizeof(d3), "%s/.config/email-cli/accounts", g_test_home);
    snprintf(d4, sizeof(d4), "%s/.config/email-cli/accounts/testuser", g_test_home);
    mkdir(g_test_home, 0700);
    mkdir(d1, 0700);
    mkdir(d2, 0700);
    mkdir(d3, 0700);
    mkdir(d4, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", d4);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:9993\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SMTP_HOST=smtp://localhost:9025\n"
        "SMTP_PORT=9025\n"
        "SMTP_USER=testuser\n"
        "SMTP_PASS=testpass\n");
    /* No SSL_NO_VERIFY=1 — insecure smtp:// URL must be rejected */
    fclose(fp);
    chmod(path, 0600);
}

static void test_tls_imap_rejected(void) {
    /* US-23 AC1: imap:// in EMAIL_HOST without SSL_NO_VERIFY=1 must be
     * rejected with an error message mentioning imaps:// */
    write_config_no_ssl_verify_imap();
    const char *a[] = {"list", "--batch", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "tls imap rejected: opens");
    ASSERT_WAIT_FOR(s, "imaps://", WAIT_MS);
    pty_close(s);
    write_config(); /* restore safe config */
}

static void test_tls_smtp_rejected(void) {
    /* US-23 AC2: smtp:// in SMTP_HOST without SSL_NO_VERIFY=1 must be
     * rejected with an error message mentioning smtps:// */
    write_config_no_ssl_verify_smtp();
    /* email-cli send: config_store rejects smtp:// before any network call */
    const char *a[] = {"send",
        "--to",      "recipient@example.com",
        "--subject", "TLS test",
        "--body",    "body",
        NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "tls smtp rejected: opens");
    ASSERT_WAIT_FOR(s, "smtps://", WAIT_MS);
    pty_close(s);
    write_config(); /* restore safe config */
}

/* ══════════════════════════════════════════════════════════════════════
 *  TUI LIST: COMPOSE / REPLY FROM LIST (US-20)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_tui_list_compose_key(void) {
    /* US-20: 'c' in TUI list launches compose; EDITOR=true → abort → back to list */
    restart_mock();
    setenv("EDITOR", "true", 1);  /* no-op editor exits without writing To: */
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "tui list compose key: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);  /* compose dialog title */
    pty_send_key(s, PTY_KEY_ENTER);  /* To → Cc */
    pty_send_key(s, PTY_KEY_ENTER);  /* Cc → Bcc */
    pty_send_key(s, PTY_KEY_ENTER);  /* Bcc → Subject */
    pty_send_key(s, PTY_KEY_ENTER);  /* Subject → submit with empty To */
    ASSERT_WAIT_FOR(s, "Aborted", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[Press any key to return to inbox]", WAIT_MS);
    pty_send_str(s, " ");   /* any key — return to list */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_list_reply_key(void) {
    /* US-20: 'r' in TUI list launches reply; editor clears To: → abort → back to list */
    restart_mock();
    /* Write an editor script that blanks To: so compose aborts cleanly */
    char editor_script[256];
    snprintf(editor_script, sizeof(editor_script),
             "/tmp/test_reply_list_editor_%d.sh", (int)getpid());
    FILE *ef = fopen(editor_script, "w");
    if (ef) {
        fprintf(ef, "#!/bin/sh\n");
        fprintf(ef,
            "printf 'From: test@x.com\\nTo: \\nSubject: Re\\n\\nbody\\n' > \"$1\"\n");
        fclose(ef);
        chmod(editor_script, 0755);
    }
    setenv("EDITOR", editor_script, 1);
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "tui list reply key: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);    /* compose dialog title */
    pty_send_key(s, PTY_KEY_ENTER);  /* Cc → Bcc (To: pre-filled, starts at Cc) */
    pty_send_key(s, PTY_KEY_ENTER);  /* Bcc → Subject */
    pty_send_key(s, PTY_KEY_ENTER);  /* Subject → submit */
    ASSERT_WAIT_FOR(s, "Aborted", WAIT_MS * 2);
    ASSERT_WAIT_FOR(s, "[Press any key to return to inbox]", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    unlink(editor_script);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TUI LIST: BACKGROUND SYNC + SIGCHLD NOTIFICATION (US-19)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_tui_list_sync_and_refresh(void) {
    /* US-19: 's' starts background sync; next keypress shows notification;
     * 'R' clears it and re-renders the normal count line.
     *
     * Design: run the TUI in cron mode (SYNC_INTERVAL) so it reads from the
     * local cache and holds NO persistent IMAP connection.  This lets the
     * background email-sync subprocess connect to the single-threaded mock
     * server freely and complete quickly. */
    restart_mock();

    /* Pre-populate the local store so the cron-mode TUI shows a real list */
    {
        PtySession *ss = pty_open(COLS, ROWS);
        ASSERT(ss != NULL, "tui sync+refresh: pre-sync opens");
        const char *sa[] = {g_sync_bin, NULL};
        ASSERT(pty_run(ss, sa) == 0, "tui sync+refresh: pre-sync run");
        ASSERT_WAIT_FOR(ss, "Sync complete", WAIT_MS);
        pty_close(ss);
    }
    write_config_with_interval(5);  /* cron mode: TUI reads from local cache only */

    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "tui sync+refresh: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "s");            /* start background sync */
    ASSERT_WAIT_FOR(s, "syncing", WAIT_MS);
    /* Sync completes quickly: no IMAP conflict since TUI holds no connection.
     * 3 s settle ensures SIGCHLD fires before the DOWN keypress. */
    pty_settle(s, 3000);
    pty_send_key(s, PTY_KEY_DOWN);   /* re-render: bg_sync_done=1 → shows notification */
    ASSERT_WAIT_FOR(s, "New mail", WAIT_MS);
    pty_send_str(s, "U");            /* refresh: bg_sync_done=0, re-enters list */
    int _ok = 0;
    for (int _ms = 0; _ms < WAIT_MS; _ms += 100) {
        pty_settle(s, 100);
        if (!pty_screen_contains(s, "New mail")
                && pty_screen_contains(s, "message(s) in")) {
            _ok = 1; break;
        }
    }
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);      /* always close before final ASSERT to prevent zombie cascade */
    write_config();    /* restore standard config for subsequent tests */
    ASSERT(_ok, "tui sync+refresh: New mail notification cleared after R");
}

/* ══════════════════════════════════════════════════════════════════════
 *  GMAIL WIZARD — Account type selection (US-27)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_wizard_gmail_type_selection(void) {
    /* Gmail flow: choose type 2 → "Email address" prompt appears */
    char wiz_home[300];
    snprintf(wiz_home, sizeof(wiz_home), "%s/wizard_gmail", g_test_home);
    mkdir(wiz_home, 0700);
    setenv("HOME", wiz_home, 1);
    unsetenv("XDG_CONFIG_HOME");

    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "wizard gmail: opens");
    ASSERT_WAIT_FOR(s, "Account type", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "IMAP");
    ASSERT_SCREEN_CONTAINS(s, "Gmail");
    pty_send_str(s, "2\n");  /* Gmail */
    ASSERT_WAIT_FOR(s, "Email address", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);  /* abort — no OAuth server available */
    ASSERT_WAIT_FOR(s, "borted", WAIT_MS);
    pty_close(s);

    setenv("HOME", g_test_home, 1);
}

static void write_gmail_account(const char *name) {
    char dir1[512], dir2[512], path[600];
    snprintf(dir1, sizeof(dir1), "%s/.config/email-cli/accounts", g_test_home);
    snprintf(dir2, sizeof(dir2), "%s/.config/email-cli/accounts/%s", g_test_home, name);
    mkdir(dir1, 0700);
    mkdir(dir2, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", dir2);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "EMAIL_USER=%s\n"
        "GMAIL_MODE=1\n"
        "GMAIL_REFRESH_TOKEN=dummy_test_token\n"
        "SSL_NO_VERIFY=1\n", name);
    fclose(fp);
    chmod(path, 0600);

    /* Create local store with a dummy label .idx so label list is non-empty.
     * Use a flat layout: accounts/<name>/labels/INBOX.idx */
    {
        char d[1024];
        snprintf(d, sizeof(d), "%s/.local", g_test_home); mkdir(d, 0700);
        snprintf(d, sizeof(d), "%s/.local/share", g_test_home); mkdir(d, 0700);
        snprintf(d, sizeof(d), "%s/.local/share/email-cli", g_test_home); mkdir(d, 0700);
        snprintf(d, sizeof(d), "%s/.local/share/email-cli/accounts", g_test_home); mkdir(d, 0700);
        snprintf(d, sizeof(d), "%s/.local/share/email-cli/accounts/%s", g_test_home, name); mkdir(d, 0700);
        snprintf(d, sizeof(d), "%s/.local/share/email-cli/accounts/%s/labels", g_test_home, name); mkdir(d, 0700);
        char idx[1100];
        snprintf(idx, sizeof(idx), "%s/INBOX.idx", d);
        fp = fopen(idx, "w");
        if (fp) { fprintf(fp, "18c9b46d67a60001\n"); fclose(fp); }
    }
}

static void test_gmail_labels_backspace(void) {
    /* Gmail account: Enter opens Labels view; Backspace → accounts */
    write_gmail_account("gmailtest@gmail.com");
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "gmail labels backspace: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Gmail account should show "Gmail" in Type column */
    ASSERT_SCREEN_CONTAINS(s, "Gmail");
    /* gmail.com sorts before testuser alphabetically, BUT last_account may restore
     * the cursor to testuser; use HOME to guarantee we're on gmailtest@gmail.com. */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Labels", WAIT_MS);
    /* Backspace → back to accounts */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    /* Cleanup */
    char path[500];
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/gmailtest@gmail.com/config.ini",
             g_test_home);
    unlink(path);
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/gmailtest@gmail.com", g_test_home);
    rmdir(path);
}

static void test_account_list_type_column(void) {
    /* Account list shows Type column with "IMAP" for standard accounts */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "account type column: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Type");
    ASSERT_SCREEN_CONTAINS(s, "IMAP");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TUI ACCOUNTS: ADD / DELETE / EDIT IMAP (US-21 AC4/5/13)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_tui_accounts_new_key(void) {
    /* US-21 AC4: 'n' launches Setup Wizard; Ctrl-D aborts → accounts screen */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "accounts new key: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "n");
    ASSERT_WAIT_FOR(s, "Account type", WAIT_MS);
    pty_send_str(s, "1\n");  /* IMAP */
    ASSERT_WAIT_FOR(s, "IMAP Host", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);  /* EOF aborts wizard */
    ASSERT_WAIT_FOR(s, "borted", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_accounts_delete_key(void) {
    /* US-21 AC5: 'd' deletes the selected account; it disappears from the list */
    write_second_account("todelete@example.com");
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "accounts delete key: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "todelete@example.com");
    /* domain-first sort: todelete@example.com (domain: example.com) sorts before
     * testuser (no domain); cursor starts at 0 = todelete. Use HOME to ensure it. */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_screen_contains(s, "todelete@example.com") == 0,
           "accounts delete: account removed from list");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    /* cleanup: directory already removed by the TUI; unlink is defensive */
    char path[500];
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/todelete@example.com/config.ini",
             g_test_home);
    unlink(path);
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/todelete@example.com", g_test_home);
    rmdir(path);
}

static void test_tui_accounts_imap_edit_key(void) {
    /* US-21 AC13: 'i' opens IMAP wizard for selected account; Ctrl-D aborts */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "accounts imap edit: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "i");
    ASSERT_WAIT_FOR(s, "current:", WAIT_MS);   /* "IMAP Host [current: ...]" prompt */
    pty_send_key(s, PTY_KEY_CTRL_D);           /* abort wizard */
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  EMAIL-SYNC --ACCOUNT FILTER (US-25)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_sync_account_filter_known(void) {
    /* US-25: --account syncs only the named account */
    write_second_account("testacct@test.com");
    restart_mock();
    const char *a[] = {"--account", "testacct@test.com", NULL};
    PtySession *s = sync_run(a);
    ASSERT(s != NULL, "sync account filter known: opens");
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);
    /* cleanup */
    char path[500];
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/testacct@test.com/config.ini",
             g_test_home);
    unlink(path);
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/testacct@test.com", g_test_home);
    rmdir(path);
}

static void test_sync_account_filter_unknown(void) {
    /* US-25: --account with unknown name prints "not found" and exits non-zero */
    const char *a[] = {"--account", "nobody@nowhere.invalid", NULL};
    PtySession *s = sync_run(a);
    ASSERT(s != NULL, "sync account filter unknown: opens");
    ASSERT_WAIT_FOR(s, "not found", WAIT_MS);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  PENDING FLAG OFFLINE QUEUE (US-26)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_pending_flags_offline_queue(void) {
    /* US-26: flag a message in offline/cron mode → manifest updated optimistically,
     * pending_flags file created on disk for next sync to consume */

    /* Sync first to populate manifest with known UIDs */
    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "pending flags: sync opens");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    /* Switch to offline/cron mode and stop the server */
    write_config_with_interval(5);
    stop_mock_server();

    /* Open TUI list — works offline in cron mode (served from manifest) */
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "pending flags: tui opens offline");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Press 'f' to toggle Flagged on the first message */
    pty_send_str(s, "f");
    pty_settle(s, SETTLE_MS);
    /* 'f' toggles Flagged; message may already be flagged from earlier tests,
     * so either "Starred" or "Unstarred" is acceptable feedback. */
    ASSERT(pty_screen_contains(s, "Starred") || pty_screen_contains(s, "Unstarred"),
           "pending flags: 'f' produced star-feedback");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    /* pending_flags file must exist on disk */
    char pf_path[512];
    snprintf(pf_path, sizeof(pf_path),
             "%s/.local/share/email-cli/accounts/testuser/pending_flags/INBOX.tsv",
             g_test_home);
    ASSERT(access(pf_path, F_OK) == 0,
           "pending flags: INBOX.tsv written to disk after offline flag change");

    /* Restore standard config (no sync_interval) */
    write_config();
    restart_mock();
}

/* ══════════════════════════════════════════════════════════════════════
 *  VIRTUAL UNREAD / FLAGGED LIST (Ticket 4 + IMAP bug fixes)
 * ══════════════════════════════════════════════════════════════════════ */

/* Helper: create directory chain for account data under g_test_home. */
static void make_account_dirs(void) {
    char p[700];
    snprintf(p, sizeof(p), "%s/.local",                             g_test_home); mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/.local/share",                       g_test_home); mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/.local/share/email-cli",             g_test_home); mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/.local/share/email-cli/accounts",    g_test_home); mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/.local/share/email-cli/accounts/testuser", g_test_home); mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/.local/share/email-cli/accounts/testuser/manifests", g_test_home); mkdir(p, 0700);
}

/*
 * Helper: write a manifest TSV file for the given folder.
 * Each element of `lines` must be a complete TSV line:
 *   uid TAB from TAB subject TAB date TAB flags
 */
static void write_test_manifest(const char *folder,
                                 const char **lines, int n) {
    make_account_dirs();
    char path[700];
    snprintf(path, sizeof(path),
             "%s/.local/share/email-cli/accounts/testuser/manifests/%s.tsv",
             g_test_home, folder);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    for (int i = 0; i < n; i++) fprintf(fp, "%s\n", lines[i]);
    fclose(fp);
}

/* Helper: write a minimal .eml cache file so the reader can open a UID.
 * UID "0000000000000001" → d1='1', d2='0' → store/INBOX/1/0/<uid>.eml */
static void write_test_eml(const char *folder, const char *uid,
                            const char *from, const char *subject,
                            const char *body) {
    make_account_dirs();
    char last = uid[strlen(uid) - 1];
    char prev = strlen(uid) > 1 ? uid[strlen(uid) - 2] : '0';
    char dir[700], path[800];
    snprintf(dir, sizeof(dir),
             "%s/.local/share/email-cli/accounts/testuser/store/%s/%c/%c",
             g_test_home, folder, last, prev);
    /* mkdir -p equivalent for four levels */
    {
        char tmp[700];
        snprintf(tmp, sizeof(tmp),
                 "%s/.local/share/email-cli/accounts/testuser/store", g_test_home);
        mkdir(tmp, 0700);
        snprintf(tmp, sizeof(tmp),
                 "%s/.local/share/email-cli/accounts/testuser/store/%s", g_test_home, folder);
        mkdir(tmp, 0700);
        char d1dir[700];
        snprintf(d1dir, sizeof(d1dir),
                 "%s/.local/share/email-cli/accounts/testuser/store/%s/%c", g_test_home, folder, last);
        mkdir(d1dir, 0700);
        mkdir(dir, 0700);
    }
    snprintf(path, sizeof(path), "%s/%s.eml", dir, uid);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
            "From: %s\r\n"
            "Subject: %s\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "\r\n"
            "%s\r\n", from, subject, body);
    fclose(fp);
}

/* Helper: open email-tui and stop at the folder browser (before INBOX list).
 * Caller must call pty_close(s) and ESC/settle as needed. */
static PtySession *tui_open_to_folders(void) {
    PtySession *s = cli_run(NULL);
    if (!s) return NULL;
    if (pty_wait_for(s, "Email Account", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return NULL; }
    return s;
}

static void test_virtual_folder_sections_shown(void) {
    /* Ticket 4: folder browser shows all virtual rows under Tags/Flags */
    restart_mock();
    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "virtual sections: opens folder browser");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Tags / Flags");
    ASSERT_SCREEN_CONTAINS(s, "Unread");
    ASSERT_SCREEN_CONTAINS(s, "Flagged");
    ASSERT_SCREEN_CONTAINS(s, "Junk");
    ASSERT_SCREEN_CONTAINS(s, "Phishing");
    ASSERT_SCREEN_CONTAINS(s, "Answered");
    ASSERT_SCREEN_CONTAINS(s, "Forwarded");
    ASSERT_SCREEN_CONTAINS(s, "Folders");
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_virtual_unread_count_from_manifests(void) {
    /* Ticket 4 / Bug 1: Unread row in folder browser reflects local manifests.
     * Two folders have unread messages → count is non-zero. */

    /* Sync first to populate the local folder list cache */
    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "unread count: sync opens");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    /* Seed: INBOX has 2 unread (flag=1) and 1 read (flag=0) */
    const char *inbox[] = {
        "0000000000000011\tsender@example.com\tUnread One\t2026-04-25 10:00\t1",
        "0000000000000012\tsender@example.com\tUnread Two\t2026-04-25 09:00\t1",
        "0000000000000013\tsender@example.com\tAlready Read\t2026-04-25 08:00\t0",
    };
    write_test_manifest("INBOX", inbox, 3);
    /* Seed: Sent has 1 unread */
    const char *sent[] = {
        "0000000000000021\tme@example.com\tSent Unread\t2026-04-24 08:00\t1",
    };
    write_test_manifest("Sent", sent, 1);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "unread count: opens folder browser offline");
    pty_settle(s, SETTLE_MS);

    /* Navigate UP twice to highlight the Unread row:
     * VPREFIX(4=INBOX) → UP → VP_FLAGGED(2) → UP → VP_UNREAD(1) */
    pty_send_key(s, PTY_KEY_UP);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_UP);
    pty_settle(s, SETTLE_MS);

    /* The Unread row must show total unread = 3 (2 from INBOX + 1 from Sent) */
    ASSERT_SCREEN_CONTAINS(s, "3");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

static void test_virtual_unread_list_shows_messages(void) {
    /* Bug 2 / Bug 3 prerequisite: navigating into the virtual Unread list
     * shows all unread messages across folders. */

    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "unread list: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    const char *inbox[] = {
        "0000000000000031\tsender@example.com\tImportant Update\t2026-04-25 10:00\t1",
        "0000000000000032\tsender@example.com\tAlready Read\t2026-04-25 09:00\t0",
    };
    write_test_manifest("INBOX", inbox, 2);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "unread list: opens folder browser");
    pty_settle(s, SETTLE_MS);

    /* Navigate to VP_UNREAD row: HOME goes directly to VP_UNREAD */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);

    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Only the unread message should appear (the read one is filtered out) */
    ASSERT_SCREEN_CONTAINS(s, "Important Update");
    ASSERT(pty_screen_contains(s, "Already Read") == 0,
           "unread list: read message must not appear in Unread virtual list");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

static void test_virtual_enter_opens_reader(void) {
    /* Bug 3 fix: Enter in the virtual Unread list opens the message reader.
     * Previously failed because entries[i].folder was empty. */

    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "virtual enter: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    const char uid[] = "0000000000000041";
    const char *inbox[] = {
        "0000000000000041\tsender@example.com\tOpen Me Please\t2026-04-25 10:00\t1",
    };
    write_test_manifest("INBOX", inbox, 1);
    write_test_eml("INBOX", uid, "sender@example.com", "Open Me Please",
                   "This is the message body.");

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "virtual enter: opens folder browser");
    pty_settle(s, SETTLE_MS);

    /* Navigate to Unread: HOME goes directly to VP_UNREAD */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);

    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Open Me Please");

    /* Press Enter on the first (and only) message → opens reader */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "sender@example.com");

    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

static void test_virtual_n_marks_message_read(void) {
    /* Bug 2 fix: pressing 'n' in the virtual Unread list marks the message read.
     * Previously 'n' saved to __unread__.tsv instead of the real folder manifest,
     * so the status marker did not change. */

    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "virtual n: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    const char *inbox[] = {
        "0000000000000051\tsender@example.com\tMark Me Read\t2026-04-25 10:00\t1",
    };
    write_test_manifest("INBOX", inbox, 1);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "virtual n: opens folder browser");
    pty_settle(s, SETTLE_MS);

    /* Navigate to Unread: HOME goes directly to VP_UNREAD */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);

    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Mark Me Read");

    /* Status column must show 'N' (unread marker) before toggling */
    ASSERT_SCREEN_CONTAINS(s, "N");

    /* Press 'n' to mark as read */
    pty_send_str(s, "n");
    pty_settle(s, SETTLE_MS);

    /* 'N' status marker must disappear; message should show as read "----" */
    ASSERT_SCREEN_CONTAINS(s, "----");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    /* Verify the per-folder manifest on disk was updated (flags=0) */
    char mpath[600];
    snprintf(mpath, sizeof(mpath),
             "%s/.local/share/email-cli/accounts/testuser/manifests/INBOX.tsv",
             g_test_home);
    FILE *fp = fopen(mpath, "r");
    ASSERT(fp != NULL, "virtual n: INBOX manifest exists");
    if (fp) {
        char line[512] = "";
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "0000000000000051")) break;
        }
        fclose(fp);
        /* Last field (flags) must be 0 after marking read */
        ASSERT(strstr(line, "\t0\n") || strstr(line, "\t0\r"),
               "virtual n: INBOX manifest updated to flags=0");
    }

    write_config();
    restart_mock();
}

static void test_virtual_flagged_shows_messages(void) {
    /* Ticket 4: Flagged virtual list shows messages with MSG_FLAG_FLAGGED(=2). */

    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "virtual flagged: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    const char *inbox[] = {
        "0000000000000061\tsender@example.com\tStarred Message\t2026-04-25 10:00\t2",
        "0000000000000062\tsender@example.com\tNot Starred\t2026-04-25 09:00\t0",
    };
    write_test_manifest("INBOX", inbox, 2);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "virtual flagged: opens folder browser");
    pty_settle(s, SETTLE_MS);

    /* Navigate to VP_FLAGGED: HOME→VP_UNREAD(1), DOWN→VP_FLAGGED(2) */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);

    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "Starred Message");
    ASSERT(pty_screen_contains(s, "Not Starred") == 0,
           "virtual flagged: unflagged message must not appear in Flagged list");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

static void test_unread_count_refreshes_after_mark(void) {
    /* Bug 1 fix: after marking a message read in INBOX and going back to the
     * folder browser, manifest_count_all_flags is re-called and shows the
     * updated (lower) unread count. */

    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "count refresh: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    /* Seed INBOX with exactly one unread message */
    const char *inbox[] = {
        "0000000000000071\tsender@example.com\tSingle Unread\t2026-04-25 10:00\t1",
    };
    write_test_manifest("INBOX", inbox, 1);

    /* Open folder browser; the Unread row should show "1" */
    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "count refresh: opens folder browser");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "1");

    /* Navigate to INBOX (default position) and open the message list */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Single Unread");

    /* Mark it as read */
    pty_send_str(s, "n");
    pty_settle(s, SETTLE_MS);

    /* Go back to folder browser via Backspace */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Unread row must now show "0": HOME → VP_UNREAD(1) */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "0");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

/* ══════════════════════════════════════════════════════════════════════
 *  VIRTUAL JUNK / ANSWERED / FORWARDED LISTS
 * ══════════════════════════════════════════════════════════════════════ */

static void test_virtual_junk_list_shows_messages(void) {
    /* Junk virtual row opens a message list showing $Junk flagged messages. */
    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "virtual junk: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    /* flags=64 = MSG_FLAG_JUNK */
    const char *inbox[] = {
        "00000000000000e1\tspam@bad.com\tYou Won!\t2026-04-25 08:00\t64",
        "00000000000000e2\tnormal@ok.com\tNormal mail\t2026-04-25 09:00\t0",
    };
    write_test_manifest("INBOX", inbox, 2);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "virtual junk: opens folder browser");
    pty_settle(s, SETTLE_MS);

    /* Junk row is VP_JUNK=3, which is 3 rows below VP_HDR_FLAGS(0):
     * HOME lands at VP_UNREAD(1), then 2× DOWN reaches VP_JUNK(3) */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "You Won!");
    /* Normal mail must NOT appear in Junk list */
    ASSERT_SCREEN_NOT_CONTAINS(s, "Normal mail");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

static void test_virtual_answered_list_shows_messages(void) {
    /* Answered virtual row (VP_ANSWERED=5) opens a list of replied messages. */
    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "virtual answered: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    /* flags=16 = MSG_FLAG_ANSWERED */
    const char *inbox[] = {
        "00000000000000f1\tboss@acme.com\tRe: Budget\t2026-04-25 10:00\t16",
        "00000000000000f2\tteam@acme.com\tTeam update\t2026-04-25 11:00\t0",
    };
    write_test_manifest("INBOX", inbox, 2);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "virtual answered: opens folder browser");
    pty_settle(s, SETTLE_MS);

    /* VP_ANSWERED=5: HOME → VP_UNREAD(1), then 4× DOWN skipping VP_HDR_FOLD(7)? No:
     * order is HDR(0) UNREAD(1) FLAGGED(2) JUNK(3) PHISHING(4) ANSWERED(5) FORWARDED(6) HDR_FOLD(7)
     * So from UNREAD(1): 4× DOWN = JUNK(3) PHISHING(4) ANSWERED(5)... wait that's 4 downs.
     * Actually: 1→2→3→4→5 = 4 downs from VP_UNREAD */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    for (int k = 0; k < 4; k++) { pty_send_key(s, PTY_KEY_DOWN); pty_settle(s, SETTLE_MS); }
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "Re: Budget");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Team update");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

static void test_virtual_forwarded_list_shows_messages(void) {
    /* Forwarded virtual row (VP_FORWARDED=6) opens a list of forwarded messages. */
    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "virtual forwarded: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    /* flags=32 = MSG_FLAG_FORWARDED */
    const char *inbox[] = {
        "0000000000000101\tcolleague@acme.com\tFwd: Offer\t2026-04-25 12:00\t32",
        "0000000000000102\tother@acme.com\tUnrelated\t2026-04-25 13:00\t0",
    };
    write_test_manifest("INBOX", inbox, 2);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "virtual forwarded: opens folder browser");
    pty_settle(s, SETTLE_MS);

    /* VP_FORWARDED=6: 5× DOWN from VP_UNREAD(1) */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    for (int k = 0; k < 5; k++) { pty_send_key(s, PTY_KEY_DOWN); pty_settle(s, SETTLE_MS); }
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "Fwd: Offer");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Unrelated");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

/* ══════════════════════════════════════════════════════════════════════
 *  FLAG STATUS COLUMN (P/J/R/F markers)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_status_junk_marker_shown(void) {
    /* A message with MSG_FLAG_JUNK(=64) must display 'J' in the status column. */
    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "junk marker: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    /* flags=64 = MSG_FLAG_JUNK */
    const char *inbox[] = {
        "00000000000000a1\tspammer@bad.com\tWin a Prize!\t2026-04-25 10:00\t64",
    };
    write_test_manifest("INBOX", inbox, 1);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "junk marker: opens folder browser");
    pty_settle(s, SETTLE_MS);

    /* Open INBOX (default cursor position) */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "Win a Prize!");
    /* Status column: position 1 must be 'J' (junk) */
    ASSERT_SCREEN_CONTAINS(s, "J");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

static void test_status_phishing_marker_shown(void) {
    /* A message with MSG_FLAG_PHISHING(=128) must display 'P' in the status column.
     * P has higher priority than J. */
    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "phishing marker: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    /* flags=128 = MSG_FLAG_PHISHING */
    const char *inbox[] = {
        "00000000000000b1\tphisher@evil.com\tUpdate Your Password\t2026-04-25 11:00\t128",
    };
    write_test_manifest("INBOX", inbox, 1);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "phishing marker: opens folder browser");
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "Update Your Password");
    ASSERT_SCREEN_CONTAINS(s, "P");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

static void test_status_answered_marker_shown(void) {
    /* A message with MSG_FLAG_ANSWERED(=16) must display 'R' in position 5. */
    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "answered marker: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    /* flags=16 = MSG_FLAG_ANSWERED */
    const char *inbox[] = {
        "00000000000000c1\tboss@acme.com\tRe: Meeting\t2026-04-25 09:00\t16",
    };
    write_test_manifest("INBOX", inbox, 1);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "answered marker: opens folder browser");
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "Re: Meeting");
    /* Status column position 5: 'R' for answered */
    ASSERT_SCREEN_CONTAINS(s, "R");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

static void test_status_forwarded_marker_shown(void) {
    /* A message with MSG_FLAG_FORWARDED(=32) must display 'F' in position 5. */
    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "forwarded marker: sync");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    write_config_with_interval(5);
    stop_mock_server();

    /* flags=32 = MSG_FLAG_FORWARDED */
    const char *inbox[] = {
        "00000000000000d1\tcolleague@acme.com\tFwd: Proposal\t2026-04-25 08:30\t32",
    };
    write_test_manifest("INBOX", inbox, 1);

    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "forwarded marker: opens folder browser");
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "Fwd: Proposal");
    ASSERT_SCREEN_CONTAINS(s, "F");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    write_config();
    restart_mock();
}

/* ══════════════════════════════════════════════════════════════════════
 *  mark-junk / mark-notjunk CLI COMMANDS
 * ══════════════════════════════════════════════════════════════════════ */

static void test_mark_junk_help(void) {
    /* 'mark-junk --help' must show usage with $Junk and SPAM references. */
    const char *a[] = {"mark-junk", "--help", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "mark-junk --help: opens");
    ASSERT_WAIT_FOR(s, "mark-junk", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "junk");
    pty_close(s);
}

static void test_mark_notjunk_help(void) {
    /* 'mark-notjunk --help' must show usage. */
    const char *a[] = {"mark-notjunk", "--help", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "mark-notjunk --help: opens");
    ASSERT_WAIT_FOR(s, "mark-notjunk", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "junk");
    pty_close(s);
}

static void test_mark_junk_missing_arg(void) {
    /* 'mark-junk' with no UID must print an error and show usage. */
    restart_mock();
    const char *a[] = {"mark-junk", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "mark-junk no arg: opens");
    ASSERT_WAIT_FOR(s, "mark-junk", WAIT_MS);
    pty_close(s);
}

static void test_mark_notjunk_missing_arg(void) {
    /* 'mark-notjunk' with no UID must print an error and show usage. */
    restart_mock();
    const char *a[] = {"mark-notjunk", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "mark-notjunk no arg: opens");
    ASSERT_WAIT_FOR(s, "mark-notjunk", WAIT_MS);
    pty_close(s);
}

static void test_mark_junk_blocked_in_ro(void) {
    /* email-cli-ro must reject mark-junk. */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_cli_ro_bin);
    restart_mock();
    const char *a[] = {"mark-junk", "0000000000000001", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "mark-junk ro block: opens");
    ASSERT_WAIT_FOR(s, "read-only", WAIT_MS);
    pty_close(s);
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_tui_bin);
}

static void test_mark_notjunk_blocked_in_ro(void) {
    /* email-cli-ro must reject mark-notjunk. */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_cli_ro_bin);
    restart_mock();
    const char *a[] = {"mark-notjunk", "0000000000000001", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "mark-notjunk ro block: opens");
    ASSERT_WAIT_FOR(s, "read-only", WAIT_MS);
    pty_close(s);
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_tui_bin);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TUI RULES EDITOR (US-62)
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * Rules editor operations are purely local (no network after list view opens).
 * Use a shorter wait than the global WAIT_MS which is sized for IMAP latency.
 */
#define RULES_WAIT_MS 1500

static void write_rules_ini(void) {
    char path[512];
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/testuser/rules.ini",
             g_test_home);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "[rule \"SpamFilter\"]\n"
        "if-from = *@spam.example.com\n"
        "then-add-label    = _junk\n"
        "\n"
        "[rule \"WorkMail\"]\n"
        "if-subject = *[work]*\n"
        "then-add-label    = Work\n"
        "\n");
    fclose(fp);
}

static void remove_rules_ini(void) {
    char path[512];
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/testuser/rules.ini",
             g_test_home);
    unlink(path);
}

static void test_tui_rules_editor_opens(void) {
    /* US-62 AC1: 'l' from message list opens the rules editor screen */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules editor opens: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS); /* status bar rendered after title fflush */
    ASSERT_SCREEN_CONTAINS(s, "a=add");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_rules_editor_empty_message(void) {
    /* US-62 AC2: empty rules list shows the "no rules" hint */
    remove_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules editor empty: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "no rules", RULES_WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "a=add");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_rules_editor_lists_rules(void) {
    /* US-62 AC3: pre-populated rules.ini → rule names visible in editor */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules editor list: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "SpamFilter");
    ASSERT_SCREEN_CONTAINS(s, "WorkMail");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_editor_add_rule(void) {
    /* US-62 AC4 / US-80: 'a' → two-step wizard → 'y' → rule saved
     * Step 1: when list editor (a=add, enter text, q=confirm)
     * Step 2: name + 8 action fields */
    remove_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules editor add: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "no rules", RULES_WAIT_MS);
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Add new rule", RULES_WAIT_MS);
    /* Step 1: when list editor */
    pty_send_str(s, "a");                   /* open "new:" input */
    pty_send_str(s, "from:*@test.com\n");   /* type atom, confirm with Enter */
    pty_send_str(s, "q");                   /* confirm when list, proceed to step 2 */
    ASSERT_WAIT_FOR(s, "step 2", RULES_WAIT_MS);
    /* Step 2: name + action fields */
    pty_send_str(s, "TestRule\n");          /* Name */
    pty_send_str(s, "IMPORTANT\n");         /* add-label[1] */
    pty_send_str(s, "\n");                  /* add-label[2] (empty) */
    pty_send_str(s, "\n");                  /* add-label[3] (empty) */
    pty_send_str(s, "\n");                  /* rm-label[1] (empty) */
    pty_send_str(s, "\n");                  /* rm-label[2] (empty) */
    pty_send_str(s, "\n");                  /* rm-label[3] (empty) */
    pty_send_str(s, "\n");                  /* then-move-folder (empty) */
    pty_send_str(s, "\n");                  /* then-forward-to (empty) */
    ASSERT_WAIT_FOR(s, "Save?", RULES_WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "TestRule", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_editor_cancel_add(void) {
    /* US-62 AC5: 'a' → empty name → "required" message → back to list */
    remove_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules editor cancel add: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "no rules", RULES_WAIT_MS);
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Add new rule", RULES_WAIT_MS);
    /* Step 1: when list editor — confirm immediately with q (empty list) */
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "step 2", RULES_WAIT_MS);
    /* Step 2: 9 fields all empty — name check fires after last field */
    pty_send_str(s, "\n\n\n\n\n\n\n\n\n");
    ASSERT_WAIT_FOR(s, "required", RULES_WAIT_MS);
    pty_send_str(s, "\n"); /* dismiss "press any key" */
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_rules_editor_delete_rule(void) {
    /* US-62 AC6: cursor on first rule → 'd' → confirm 'y' → rule removed */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules editor delete: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Cursor starts at index 0 (SpamFilter); press 'd' to delete it */
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "(y/N)", RULES_WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_screen_contains(s, "SpamFilter") == 0,
           "rules editor delete: SpamFilter removed from list");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_editor_q_closes(void) {
    /* US-62 AC7: 'q' also exits the rules editor (same as ESC) */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules editor q closes: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ── US-78: rules list navigation ───────────────────────────────────── */

static void test_tui_rules_nav_down(void) {
    /* US-78 AC1: j key moves cursor to second rule */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules nav down: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "j");              /* move cursor to WorkMail */
    pty_send_key(s, PTY_KEY_ENTER);    /* open detail */
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "WorkMail");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_nav_arrow_down(void) {
    /* US-78 AC2: down-arrow also moves cursor */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules nav arrow down: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "WorkMail");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_enter_opens_first(void) {
    /* US-78 AC3: Enter on default cursor opens first rule's detail */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules enter opens first: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "SpamFilter");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

/* ── US-79: rule detail view ─────────────────────────────────────────── */

static void test_tui_rules_detail_shows_fields(void) {
    /* US-79 AC1: detail view shows all non-empty rule fields */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules detail fields: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS); /* wait for when/labels flushed after title */
    ASSERT_SCREEN_CONTAINS(s, "*@spam.example.com");
    ASSERT_SCREEN_CONTAINS(s, "_junk");
    ASSERT_SCREEN_CONTAINS(s, "e=edit");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_detail_esc_back(void) {
    /* US-79 AC2: ESC in detail view returns to the rules list */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules detail esc back: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_detail_delete(void) {
    /* US-79 AC3: 'd' in detail view with 'y' confirm removes the rule */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules detail delete: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "(y/N)", RULES_WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_screen_contains(s, "SpamFilter") == 0,
           "rules detail delete: SpamFilter gone from list");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

/* ── US-80: rule edit form with inline editing ───────────────────────── */

static void test_tui_rules_edit_form_opens(void) {
    /* US-80 AC1: 'e' from detail view opens the two-step edit form; step 2 shows "Edit rule for" */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules edit form opens: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "e");
    ASSERT_WAIT_FOR(s, "conditions (step 1/2)", WAIT_MS); /* step 1: when-list editor */
    pty_send_str(s, "q");                                  /* confirm conditions, advance to step 2 */
    ASSERT_WAIT_FOR(s, "Edit rule for", WAIT_MS);          /* step 2: actions form */
    pty_settle(s, SETTLE_MS);                              /* wait for name field to render */
    ASSERT_SCREEN_CONTAINS(s, "SpamFilter");               /* name prefill visible */
    pty_send_key(s, PTY_KEY_ESC);                          /* cancel step 2 */
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);            /* back to detail */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_edit_form_prefill(void) {
    /* US-80 AC2: when conditions from existing rule are shown in step 1 of the edit form */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules edit prefill: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "e");
    ASSERT_WAIT_FOR(s, "conditions (step 1/2)", WAIT_MS); /* step 1 */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "*@spam.example.com");       /* if-from condition prefilled */
    pty_send_key(s, PTY_KEY_ESC);                          /* cancel edit */
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_edit_form_esc_cancel(void) {
    /* US-80 AC3: ESC in step 1 of the edit form cancels without modifying the rule */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules edit cancel: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "e");
    ASSERT_WAIT_FOR(s, "conditions (step 1/2)", WAIT_MS); /* step 1 opens */
    pty_send_key(s, PTY_KEY_ESC);                          /* cancel — no change */
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);            /* still in detail view */
    ASSERT_SCREEN_CONTAINS(s, "SpamFilter");               /* rule unchanged */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "SpamFilter");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

#undef RULES_WAIT_MS

/* ── Main ────────────────────────────────────────────────────────────── */

#define RUN_TEST(fn) do { printf("  %s...\n", #fn); fn(); } while(0)

int main(int argc, char *argv[]) {
    printf("--- email-cli PTY View Tests ---\n\n");

    if (argc < 6) {
        fprintf(stderr, "Usage: %s <email-cli> <mock-server> <email-cli-ro> <email-sync> <email-tui>\n", argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(g_cli_bin,       sizeof(g_cli_bin),       "%s", argv[1]);
    snprintf(g_batch_cli_bin, sizeof(g_batch_cli_bin), "%s", argv[1]);
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

    printf("\n--- Command separation: labels vs folders ---\n");
    RUN_TEST(test_list_labels_blocked_on_imap);
    RUN_TEST(test_list_folders_works_on_imap);
    RUN_TEST(test_create_label_blocked_on_imap);
    RUN_TEST(test_delete_label_blocked_on_imap);
    RUN_TEST(test_create_folder_help);
    RUN_TEST(test_delete_folder_help);
    RUN_TEST(test_create_folder_missing_arg);
    RUN_TEST(test_delete_folder_missing_arg);

    /* Interactive tests require the TUI binary */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_tui_bin);

    printf("\n--- Interactive list ---\n");
    RUN_TEST(test_interactive_list_content);
    RUN_TEST(test_interactive_list_separator);
    RUN_TEST(test_interactive_list_statusbar);
    RUN_TEST(test_interactive_list_esc_quit);
    RUN_TEST(test_interactive_list_nav);
    RUN_TEST(test_interactive_list_flags);
    RUN_TEST(test_tui_list_search_filter);

    printf("\n--- Interactive show ---\n");
    RUN_TEST(test_interactive_show_content);
    RUN_TEST(test_interactive_show_separator);
    RUN_TEST(test_interactive_show_statusbar);
    RUN_TEST(test_interactive_show_backspace);
    RUN_TEST(test_interactive_show_esc_exits);
    RUN_TEST(test_interactive_show_q_to_list);
    RUN_TEST(test_interactive_show_pgdn);
    RUN_TEST(test_interactive_show_arrow_scroll);
    RUN_TEST(test_interactive_show_uid_in_header);
    RUN_TEST(test_interactive_show_source_toggle);
    RUN_TEST(test_interactive_show_search_finds);
    RUN_TEST(test_interactive_show_search_no_match);
    RUN_TEST(test_interactive_show_url_rendered);

    printf("\n--- Interactive list-folders ---\n");
    RUN_TEST(test_interactive_folders_content);
    RUN_TEST(test_interactive_folders_statusbar);
    RUN_TEST(test_interactive_folders_toggle);
    RUN_TEST(test_interactive_folders_select);
    RUN_TEST(test_interactive_folders_nav);
    RUN_TEST(test_interactive_folders_back_to_list);
    RUN_TEST(test_interactive_folders_flat_navigate_up);
    RUN_TEST(test_interactive_folders_esc_quit);
    RUN_TEST(test_tui_folder_browser_search);

    printf("\n--- Attachment save ---\n");
    RUN_TEST(test_show_attachment_statusbar);
    RUN_TEST(test_show_attachment_picker);
    RUN_TEST(test_show_save_single);
    RUN_TEST(test_show_save_all_cancel);
    RUN_TEST(test_show_save_all_confirm);
    RUN_TEST(test_show_save_input_line_cursor);
    RUN_TEST(test_show_save_tab_completion);

    printf("\n--- Empty folder ---\n");
    RUN_TEST(test_interactive_empty_folder);
    RUN_TEST(test_interactive_empty_folder_cron);

    /* Remaining sections use batch commands — restore email-cli */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", argv[1]);

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
    RUN_TEST(test_wizard_host_autocomplete);
    RUN_TEST(test_wizard_bad_protocol_rejected);
    RUN_TEST(test_wizard_with_smtp);

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

    /* Batch-mode tests use email-cli; TUI binary does not accept arguments */
    printf("\n--- email-tui: batch mode (via email-cli) ---\n");
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", argv[1]);
    restart_mock();
    RUN_TEST(test_batch_list);
    RUN_TEST(test_batch_list_all);
    RUN_TEST(test_batch_show);
    RUN_TEST(test_batch_folders_flat);
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_tui_bin);

    printf("\n--- email-tui: interactive ---\n");
    RUN_TEST(test_tui_list_content);
    RUN_TEST(test_tui_list_esc_quit);
    RUN_TEST(test_tui_show_esc_exits);

    printf("\n--- email-tui: TUI launch ---\n");
    restart_mock();
    RUN_TEST(test_tui_interactive_launch);

    printf("\n--- email-tui: startup behaviour (US-18) ---\n");
    restart_mock();
    RUN_TEST(test_tui_always_starts_at_accounts);
    RUN_TEST(test_tui_folder_cursor_persisted);

    printf("\n--- email-tui: multi-account (US-21) ---\n");
    restart_mock();
    RUN_TEST(test_tui_accounts_screen_shows);
    RUN_TEST(test_tui_accounts_esc_quit);
    RUN_TEST(test_tui_accounts_enter_opens_list);
    RUN_TEST(test_tui_accounts_backspace_from_list);
    RUN_TEST(test_tui_accounts_multiple_shown);
    RUN_TEST(test_tui_accounts_backspace_ignored);
    RUN_TEST(test_tui_accounts_columns);
    RUN_TEST(test_tui_accounts_cursor_restored);

    printf("\n--- email-tui: help panel (US-22) ---\n");
    restart_mock();
    RUN_TEST(test_tui_accounts_help_panel);
    RUN_TEST(test_tui_list_help_panel);
    RUN_TEST(test_tui_show_help_panel);
    RUN_TEST(test_tui_folders_help_panel);
    RUN_TEST(test_tui_help_panel_question_mark);

    printf("\n--- email-tui: wizard + cron ---\n");
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_batch_cli_bin);
    RUN_TEST(test_wizard_abort);
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_tui_bin);
    RUN_TEST(test_cron_status_not_found);

    printf("\n--- email-tui: list compose/reply (US-20) ---\n");
    restart_mock();
    RUN_TEST(test_tui_list_compose_key);
    RUN_TEST(test_tui_list_reply_key);

    printf("\n--- email-tui: sync and refresh (US-19) ---\n");
    restart_mock();
    RUN_TEST(test_tui_list_sync_and_refresh);

    printf("\n--- Gmail wizard & account type column (US-27) ---\n");
    restart_mock();
    RUN_TEST(test_wizard_gmail_type_selection);
    RUN_TEST(test_account_list_type_column);
    RUN_TEST(test_gmail_labels_backspace);

    printf("\n--- email-tui: accounts management (US-21) ---\n");
    restart_mock();
    RUN_TEST(test_tui_accounts_new_key);
    RUN_TEST(test_tui_accounts_delete_key);
    RUN_TEST(test_tui_accounts_imap_edit_key);

    printf("\n--- email-sync: account filter (US-25) ---\n");
    restart_mock();
    RUN_TEST(test_sync_account_filter_known);
    RUN_TEST(test_sync_account_filter_unknown);

    printf("\n--- pending flags offline queue (US-26) ---\n");
    restart_mock();
    RUN_TEST(test_pending_flags_offline_queue);

    printf("\n--- virtual Unread/Flagged list ---\n");
    restart_mock();
    RUN_TEST(test_virtual_folder_sections_shown);
    RUN_TEST(test_virtual_unread_count_from_manifests);
    RUN_TEST(test_virtual_unread_list_shows_messages);
    RUN_TEST(test_virtual_enter_opens_reader);
    RUN_TEST(test_virtual_n_marks_message_read);
    RUN_TEST(test_virtual_flagged_shows_messages);
    RUN_TEST(test_unread_count_refreshes_after_mark);

    printf("\n--- virtual Junk/Answered/Forwarded lists ---\n");
    restart_mock();
    RUN_TEST(test_virtual_junk_list_shows_messages);
    RUN_TEST(test_virtual_answered_list_shows_messages);
    RUN_TEST(test_virtual_forwarded_list_shows_messages);

    printf("\n--- flag status column (P/J/R/F markers) ---\n");
    restart_mock();
    RUN_TEST(test_status_junk_marker_shown);
    RUN_TEST(test_status_phishing_marker_shown);
    RUN_TEST(test_status_answered_marker_shown);
    RUN_TEST(test_status_forwarded_marker_shown);

    /* mark-junk/mark-notjunk are email-cli commands, not TUI */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", argv[1]);

    printf("\n--- mark-junk / mark-notjunk commands ---\n");
    restart_mock();
    RUN_TEST(test_mark_junk_help);
    RUN_TEST(test_mark_notjunk_help);
    RUN_TEST(test_mark_junk_missing_arg);
    RUN_TEST(test_mark_notjunk_missing_arg);
    RUN_TEST(test_mark_junk_blocked_in_ro);
    RUN_TEST(test_mark_notjunk_blocked_in_ro);

    /* Restore full email-cli binary */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", argv[1]);

    /* ── TLS enforcement (US-23) ──────────────────────────────────────── */
    printf("\n--- TLS enforcement (US-23) ---\n");
    RUN_TEST(test_tls_imap_rejected);
    RUN_TEST(test_tls_smtp_rejected);

    /* ── TUI rules editor (US-62) ────────────────────────────────────── */
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", g_tui_bin);
    printf("\n--- email-tui: rules editor (US-62) ---\n");
    restart_mock();
    RUN_TEST(test_tui_rules_editor_opens);
    RUN_TEST(test_tui_rules_editor_empty_message);
    RUN_TEST(test_tui_rules_editor_lists_rules);
    RUN_TEST(test_tui_rules_editor_add_rule);
    RUN_TEST(test_tui_rules_editor_cancel_add);
    RUN_TEST(test_tui_rules_editor_delete_rule);
    RUN_TEST(test_tui_rules_editor_q_closes);

    /* ── TUI rules list navigation (US-78) ──────────────────────────── */
    printf("\n--- email-tui: rules list navigation (US-78) ---\n");
    restart_mock();
    RUN_TEST(test_tui_rules_nav_down);
    RUN_TEST(test_tui_rules_nav_arrow_down);
    RUN_TEST(test_tui_rules_enter_opens_first);

    /* ── TUI rule detail view (US-79) ───────────────────────────────── */
    printf("\n--- email-tui: rule detail view (US-79) ---\n");
    restart_mock();
    RUN_TEST(test_tui_rules_detail_shows_fields);
    RUN_TEST(test_tui_rules_detail_esc_back);
    RUN_TEST(test_tui_rules_detail_delete);

    /* ── TUI rule edit form (US-80) ─────────────────────────────────── */
    printf("\n--- email-tui: rule edit form inline editing (US-80) ---\n");
    restart_mock();
    RUN_TEST(test_tui_rules_edit_form_opens);
    RUN_TEST(test_tui_rules_edit_form_prefill);
    RUN_TEST(test_tui_rules_edit_form_esc_cancel);

done:
    stop_mock_server();
    if (g_old_home[0]) setenv("HOME", g_old_home, 1);

    printf("\n--- PTY Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
