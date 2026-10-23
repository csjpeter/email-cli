/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-31 */
/* PTY tests of every view and mode of the clients. */
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
#include "test_scratch.h"
#include "pty_editor.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>

int g_tests_run = 0;
int g_tests_failed = 0;

/* ── Test infrastructure ─────────────────────────────────────────────── */

static pid_t g_mock_pid = -1;
static int   g_imap_port = 0;    /* the mock's port, picked by the kernel */
static int   g_dead_port = 0;    /* a port nobody listens on (SMTP is not mocked here) */
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

/** Opens the ':' action menu, moves to row @p row (0 is the first) and
 *  presses Enter.  The rows are the entries of the view's key table that
 *  change state, in table order (libemail/src/core/key_table.c). */
static void menu_pick(PtySession *s, int row) {
    pty_send_str(s, ":");
    pty_wait_for(s, "Actions", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    for (int i = 0; i < row; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_ENTER);
}

#define MENU_IMAP_STAR 0
#define MENU_IMAP_READ 1
#define MENU_IMAP_UNREAD 2
#define MENU_IMAP_JUNK 3
#define MENU_IMAP_SYNC 6
#define MENU_IMAP_MOVE 7
#define MENU_IMAP_DONE 8
#define MENU_READER_SAVE_ALL 6

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
        "EMAIL_HOST=imaps://localhost:%d\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SSL_NO_VERIFY=1\n"     /* TLS with self-signed cert */
        "SMTP_HOST=smtps://localhost:%d\n", /* dummy SMTP — avoids blocking prompt */
        g_imap_port, g_dead_port);
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
        "EMAIL_HOST=imaps://localhost:%d\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SSL_NO_VERIFY=1\n"   /* TLS with self-signed cert */
        "SYNC_INTERVAL=%d\n", g_imap_port, interval);
    fclose(fp);
    chmod(path, 0600);
}

/* Type a host line holding the mock's port into the wizard. */
static void send_host(PtySession *s, const char *fmt) {
    char line[128];
    snprintf(line, sizeof(line), fmt, g_imap_port);
    pty_send_str(s, line);
}

static int start_mock_server(void) {
    /* The first start lets the kernel pick the port; a restart asks for the
     * same one, because the configs written so far name it.  The port is
     * known when test_spawn_mock() returns: the mock has bound it and said
     * so, so no probe and no fixed number is needed. */
    int port = 0;
    g_mock_pid = test_spawn_mock(g_mock_bin, "MOCK_IMAP", g_imap_port, &port);
    if (g_mock_pid < 0) {
        fprintf(stderr, "views: the mock IMAP server did not start\n");
        return -1;
    }
    g_imap_port = port;
    return 0;
}

static void stop_mock_for_restart(void) {
    if (g_mock_pid > 0) {
        kill(g_mock_pid, SIGKILL);
        waitpid(g_mock_pid, NULL, 0);
        g_mock_pid = -1;
    }
}

static void restart_mock(void) {
    pty_editor_forget_draft(g_test_home);
    stop_mock_for_restart();
    if (start_mock_server() != 0) { g_tests_failed++; }
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
    /* The general help now also lists the search, filters and JSON options;
     * it no longer fits in 50 rows. */
    PtySession *s = cli_open_size(120, 80, a);
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
    PtySession *s = cli_open_size(120, 80, a)   /* list --help is ~60 lines */;
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
    /* The mock message renders to ~30 lines; on a 24-row terminal the header
     * block would scroll off before the assertions run. */
    PtySession *s = cli_open_size(COLS, 60, a);
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

static void write_rules_ini(void);
static void remove_rules_ini(void);

static void test_batch_rules_apply(void) {
    /* "AlwaysTag" has no conditions → when_from_flat returns NULL → when=NULL
     * → mail_rule_matches takes the legacy flat-field path → glob_match covered. */
    write_rules_ini();
    const char *a[] = {"rules", "apply", NULL};
    PtySession *s = cli_run(a);
    ASSERT(s != NULL, "rules apply: opens");
    ASSERT_WAIT_FOR(s, "Rules applied:", WAIT_MS);
    pty_close(s);
    remove_rules_ini();
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
static PtySession *tui_open_to_list_size(int cols, int rows);
static void folders_goto_inbox(PtySession *s);

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
        ASSERT(pty_row_contains(s, sb, "Enter open"), "list sb: Enter=open");
        ASSERT(pty_row_contains(s, sb, "Esc/q/BS folders"), "list sb: Esc/q/BS folders");
        ASSERT(!pty_row_contains(s, sb, "ESC=quit"), "list sb: no longer says ESC=quit");
        ASSERT_CELL_ATTR(s, sb, 2, PTY_ATTR_REVERSE);
    }
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* policies/tui-interface-model.md: Esc goes back one level, q is Esc, ctrl-c
 * quits at once; Esc on the top level (accounts) quits with status 0. */
static void test_interactive_list_esc_quit(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list ESC: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);                      /* list -> folders */
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);                      /* folders -> accounts */
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);                      /* top level: quits */
    ASSERT(pty_wait_exit(s, 5000) == 0, "list ESC x3: program exits with status 0");
    pty_close(s);
}

static void test_interactive_list_q_goes_back(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list q: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "q");                              /* q is Esc: back */
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "q");                              /* top level: q never quits */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    pty_send_key(s, PTY_KEY_CTRL_C);                   /* ctrl-c quits at once */
    ASSERT(pty_wait_exit(s, 5000) == 0, "accounts ctrl-c: program exits with status 0");
    pty_close(s);
}

/* EMAIL-159: PgDn/PgUp in the accounts list move by the visible rows (24-row
 * terminal: 19), not by one; they stop at the last and the first account. */
static void accounts_extra(int n, int create) {
    for (int i = 1; i <= n; i++) {
        char d[450], f[470];
        snprintf(d, sizeof(d), "%s/.config/email-cli/accounts/pg%02d", g_test_home, i);
        snprintf(f, sizeof(f), "%s/config.ini", d);
        if (create) {
            mkdir(d, 0700);
            FILE *fp = fopen(f, "w");
            if (!fp) continue;
            fprintf(fp, "EMAIL_HOST=imaps://localhost:%d\nEMAIL_USER=pg%02d\n"
                        "EMAIL_PASS=x\nEMAIL_FOLDER=INBOX\nSSL_NO_VERIFY=1\n",
                    g_imap_port, i);
            fclose(fp);
            chmod(f, 0600);
        } else {
            unlink(f);
            rmdir(d);
        }
    }
}

static void accounts_paging_body(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "accounts paging: pty");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "accounts paging: run");
    ASSERT_WAIT_FOR(s, "Email Accounts (41)", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_HOME);             /* the start row may be the default account */
    ASSERT_WAIT_FOR(s, "[1/41]", WAIT_MS);
    pty_send_key(s, PTY_KEY_PGDN);
    ASSERT_WAIT_FOR(s, "[20/41]", WAIT_MS);
    pty_send_key(s, PTY_KEY_PGDN);
    ASSERT_WAIT_FOR(s, "[39/41]", WAIT_MS);
    pty_send_key(s, PTY_KEY_PGDN);
    ASSERT_WAIT_FOR(s, "[41/41]", WAIT_MS);      /* stops at the last */
    pty_send_key(s, PTY_KEY_PGUP);
    ASSERT_WAIT_FOR(s, "[22/41]", WAIT_MS);
    pty_send_key(s, PTY_KEY_PGUP);
    ASSERT_WAIT_FOR(s, "[3/41]", WAIT_MS);
    pty_send_key(s, PTY_KEY_PGUP);
    ASSERT_WAIT_FOR(s, "[1/41]", WAIT_MS);       /* stops at the first */
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

/* The extra accounts are removed whatever the assertions did (a failed ASSERT
 * returns early), so that no later test sees them. */
static void test_accounts_pages_by_the_visible_rows(void) {
    accounts_extra(40, 0);
    accounts_extra(40, 1);                     /* 41 accounts with testuser */
    accounts_paging_body();
    accounts_extra(40, 0);
}

static void test_interactive_list_ctrl_c_quits_at_once(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list ctrl-c: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, 5000) == 0, "list ctrl-c: program exits with status 0");
    pty_close(s);
}

/* The header bar is bold white on blue, not reverse video (palette). */
static void test_interactive_header_bar_palette(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "header palette: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "message(s) in");
    ASSERT_CELL_ATTR(s, 0, 2, PTY_ATTR_BOLD);
    ASSERT(!(pty_cell_attr(s, 0, 2) & PTY_ATTR_REVERSE), "header bar is not reverse video");
    ASSERT(pty_cell_fg(s, 0, 2) == 37, "header bar foreground is white");
    pty_send_key(s, PTY_KEY_CTRL_C);
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
    /* Registry keys: '-' unread, '*' star, '!' junk — server accepts silently */
    pty_send_str(s, "-");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_str(s, "*");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_str(s, "!");
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

/* EMAIL-128: the filter bar has Since (inclusive) and Before (exclusive)
 * date scopes with the CLI's --since/--before semantics.  The mock mail is
 * dated 2026-03-26. */
static void test_tui_list_date_filter(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "date filter: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "/");
    ASSERT_WAIT_FOR(s, "Filter", WAIT_MS);
    for (int i = 0; i < 4; i++) pty_send_key(s, PTY_KEY_TAB);  /* -> Since */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Since");
    ASSERT_SCREEN_CONTAINS(s, "Before");

    /* Incomplete date: list is not narrowed */
    pty_send_str(s, "2026-03");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Showing 0 of");

    /* Since the day after the mail: nothing */
    pty_send_str(s, "-27");
    ASSERT_WAIT_FOR(s, "Showing 0 of", WAIT_MS);

    /* Since is inclusive: back to the mail's own day */
    pty_send_key(s, PTY_KEY_BACK);
    pty_send_str(s, "6");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Showing 0 of");
    ASSERT_SCREEN_CONTAINS(s, "Test Message");

    /* Before is exclusive: the mail's own day matches nothing */
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "Showing 0 of", WAIT_MS);
    /* Before the next day: the mail is back */
    pty_send_key(s, PTY_KEY_BACK);
    pty_send_str(s, "7");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Showing 0 of");
    ASSERT_SCREEN_CONTAINS(s, "Test Message");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
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
    /* The reader status bar lists every shortcut, including the two attachment
     * entries the mock message provides (a=save, A save-all).  That line is
     * longer than 100 columns and would be truncated before "/ search", so use
     * a wider terminal here. */
    PtySession *s = tui_open_to_list_size(160, ROWS);
    ASSERT(s != NULL, "show statusbar: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    int sb = find_row(s, "/ search");
    ASSERT(sb >= 0, "show sb: found /=search");
    if (sb >= 0) {
        ASSERT(pty_row_contains(s, sb, "/ search"), "show sb: /=search");
        ASSERT(pty_row_contains(s, sb, "v source"), "show sb: v=source");
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
 * US-RD-05: As a user reading a message, pressing Esc, q or Backspace
 *           returns to the message list; ctrl-c leaves the program at once.
 * Acceptance criteria:
 *   - Esc, q, Backspace in reader: list view shown again.
 *   - ctrl-c in reader: program terminates with status 0.
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
    /* Esc from the reader goes back to the list, like q and Backspace */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);                   /* ctrl-c quits at once */
    ASSERT(pty_wait_exit(s, 5000) == 0, "show ctrl-c: program exits with status 0");
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
 *   - Statusbar shows "v rendered" in source mode, "v source" in rendered.
 *   - Second 'v' returns to rendered view.
 * ══════════════════════════════════════════════════════════════════════
 */
static void test_interactive_show_source_toggle(void) {
    /* US-RD-02.  The v=source / v=rendered hint sits near the end of the
     * reader status bar, which is wider than 100 columns for a message with
     * attachments — use a wide terminal so it is not truncated away. */
    restart_mock();
    PtySession *s = tui_open_to_list_size(160, ROWS);
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
    ASSERT_WAIT_FOR(s, "v rendered", WAIT_MS);
    /* Press v again → back to rendered */
    pty_send_str(s, "v");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Hello from Mock Server");
    ASSERT_SCREEN_CONTAINS(s, "v source");
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

/* EMAIL-154: after a search, n is only "next match"; the read state keeps its
 * own keys (+ and -) in the reader. */
static void test_interactive_show_search_then_unread_keys(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "search then flags: opens");
    ASSERT_WAIT_FOR(s, "Test Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "/");
    pty_settle(s, SETTLE_MS / 2);
    pty_send_str(s, "Mock");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "n");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Marked as");
    pty_send_str(s, "+");
    ASSERT_WAIT_FOR(s, "Marked as read", WAIT_MS);
    pty_send_str(s, "-");
    ASSERT_WAIT_FOR(s, "Marked as unread", WAIT_MS);
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
    /* Open email-tui in a short terminal and navigate to the message list.
     * 24 rows is the smallest drawn terminal (EMAIL-97: below it only a
     * notice is shown): the header block (From/Subject/Date/UID/File/DMARC/
     * Attach plus separator) takes 8, the rendered body still needs two
     * pages — which is what this test exercises. */
    const char *tui_args[] = {g_tui_bin, NULL};
    PtySession *s = pty_open(COLS, 24);
    ASSERT(s != NULL, "show arrow scroll: opens");
    if (pty_run(s, tui_args) != 0) { pty_close(s); return; }
    if (pty_wait_for(s, "Email Account", WAIT_MS) != 0) { pty_close(s); return; }
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return; }
    folders_goto_inbox(s);
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
    pty_send_str(s, "l");                       /* the tree opens closed (EMAIL-200) */
    pty_settle(s, SETTLE_MS);
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

    int sb = find_row(s, "Enter open/select");
    ASSERT(sb >= 0, "list-folders sb: found Enter open/select");
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
    ASSERT_SCREEN_CONTAINS(s, "t tree");

    /* Toggle back to tree */
    pty_send_str(s, "t");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "t flat");

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
    ASSERT_WAIT_FOR(s, "t tree", WAIT_MS);    /* flat mode: hint shows "t=tree" */
    pty_settle(s, SETTLE_MS);
    /* Flat view at root shows only top-level list-folders (no '.' in name) */
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    pty_send_key(s, PTY_KEY_ENTER);           /* navigate into INBOX */
    ASSERT_WAIT_FOR(s, "Esc/q/BS up", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Header shows "Folders: INBOX/ (3)"; flat mode shows last component only */
    ASSERT_SCREEN_CONTAINS(s, "INBOX/");
    ASSERT_SCREEN_CONTAINS(s, "Sent");
    pty_send_key(s, PTY_KEY_BACK);            /* navigate up to root */
    ASSERT_WAIT_FOR(s, "Esc/q/BS back", WAIT_MS);
    pty_send_str(s, "t");                     /* toggle back to tree mode */
    ASSERT_WAIT_FOR(s, "t flat", WAIT_MS);   /* tree mode shows "t=flat" hint */
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* EMAIL-82: inside a folder that has subfolders the flat view lists the
 * folder itself first, so that its own messages can still be opened. */
static void test_interactive_folders_flat_parent_openable(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "flat parent: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "t");
    ASSERT_WAIT_FOR(s, "t tree", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);           /* into INBOX/ */
    ASSERT_WAIT_FOR(s, "Esc/q/BS up", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* the first row under the column heads is INBOX itself, cursor on it */
    int row = find_row(s, "Sent");
    ASSERT(row > 0, "flat parent: the subfolders are listed");
    int self = -1;
    for (int r = 0; r < ROWS && self < 0; r++) {
        char buf[256];
        pty_row_text(s, r, buf, sizeof(buf));
        /* a row ending in the folder name without a slash or tree glyph */
        if (strstr(buf, "INBOX") && !strstr(buf, "INBOX/") && !strstr(buf, "Folders"))
            self = r;
    }
    ASSERT(self >= 0, "flat parent: the folder itself is listed");
    if (self >= 0 && row > 0)
        ASSERT(self < row, "flat parent: the folder itself comes before its subfolders");
    pty_send_key(s, PTY_KEY_ENTER);           /* open INBOX itself */
    ASSERT_WAIT_FOR(s, "message(s) in INBOX", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* back to the folder browser, and the view mode back to the tree: the
     * mode is a stored preference the other tests rely on */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "t");
    ASSERT_WAIT_FOR(s, "t flat", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_interactive_folders_esc_quit(void) {
    /* ESC from the folder browser goes back to the accounts screen */
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "list-folders ESC quit: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT(pty_wait_exit(s, 5000) == 0, "folders ESC x2: program exits with status 0");
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

    /* Navigate to INBOX.Empty.  Folders are sorted byte-wise, so the mock's
     * LIST reply yields: INBOX(0), INBOX.AT&T(1), INBOX.Empty(2), INBOX.Sent(3),
     * INBOX.Trash(4), INBOX.Träger(5), INBOX.中(6), INBOX.😀(7).
     * INBOX is closed on opening: l opens it (EMAIL-200). */
    pty_send_str(s, "l");
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);

    /* Formal empty-folder layout: inverse title + column headers + (empty) */
    ASSERT_WAIT_FOR(s, "0 of 0 message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Subject");
    ASSERT_SCREEN_CONTAINS(s, "(empty)");
    ASSERT_SCREEN_CONTAINS(s, "Esc/q/BS folders");

    /* Backspace returns to folder browser */
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* EMAIL-81: a folder whose name has wide characters takes one row, and the
 * rows under it are not shifted: the folders with CJK and emoji names sit
 * directly one under the other. */
static void test_folder_tree_wide_characters_one_row_each(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "wide names: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");                       /* open INBOX (EMAIL-200) */
    pty_settle(s, SETTLE_MS);

    int umlaut = find_row(s, "Tr\xc3\xa4ger");
    int cjk    = find_row(s, "\xe4\xb8\xad");
    int emoji  = find_row(s, "\xf0\x9f\x98\x80");
    ASSERT(umlaut >= 0 && cjk >= 0 && emoji >= 0, "wide names: all three folders shown");
    if (umlaut >= 0 && cjk >= 0 && emoji >= 0) {
        ASSERT(cjk == umlaut + 1, "wide names: the CJK folder is the row under the umlaut one");
        ASSERT(emoji == cjk + 1, "wide names: the emoji folder is the row under the CJK one");
    }
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
    pty_send_str(s, "l");                       /* open INBOX (EMAIL-200) */
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_DOWN);              /* INBOX → INBOX.AT&T */
    pty_send_key(s, PTY_KEY_DOWN);              /* → INBOX.Empty */
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);             /* open empty folder */
    ASSERT_WAIT_FOR(s, "0 of 0 message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "(empty)");
    ASSERT_SCREEN_CONTAINS(s, "No cached data");
    ASSERT_SCREEN_CONTAINS(s, "Esc/q/BS folders");
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
    if (pty_wait_for(s, "a save(", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/** Status bar shows  a=save  A save-all(2) when list-attachments present. */
static void test_show_attachment_statusbar(void) {
    PtySession *s = open_show_view();
    ASSERT(s != NULL, "att statusbar: opens");

    ASSERT_SCREEN_CONTAINS(s, "a save(2)");
    /* saving all of them is in the ':' menu, not on a key */
    ASSERT_SCREEN_NOT_CONTAINS(s, "A save-all");

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

    menu_pick(s, MENU_READER_SAVE_ALL);
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

    menu_pick(s, MENU_READER_SAVE_ALL);
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

    menu_pick(s, MENU_READER_SAVE_ALL);       /* "Save all to:" dialog */
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
 *  ATTACHMENT PICKER KEYS (EMAIL-147), at 80x24
 * ══════════════════════════════════════════════════════════════════════ */

/** The reader of the message with @p n attachments, 80x24.  With attachments
 *  the status line shows "a save(n)". */
static PtySession *open_reader_n(int n) {
    char nb[16];
    snprintf(nb, sizeof(nb), "%d", n);
    /* The client keeps the message it fetched before, with the number of
     * attachments of that run: drop the cached copy. */
    char cached[600];
    snprintf(cached, sizeof(cached),
             "%s/.local/share/email-cli/accounts/testuser/store/INBOX/1/0/0000000000000001.eml",
             g_test_home);
    if (access(cached, F_OK) == 0 && remove(cached) != 0) perror("remove cached message");
    setenv("MOCK_IMAP_ATTACHMENTS", nb, 1);
    restart_mock();
    unsetenv("MOCK_IMAP_ATTACHMENTS");
    PtySession *s = tui_open_to_list_size(80, 24);
    if (!s) return NULL;
    if (pty_wait_for(s, "Test Message", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, n > 0 ? "a save(" : "From:", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static PtySession *open_picker_n(int n) {
    PtySession *s = open_reader_n(n);
    if (!s) return NULL;
    pty_send_str(s, "a");
    if (pty_wait_for(s, "Attachments (", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/** Picker, 80x24: the label says Esc/q/BS back; all three go back to the reader. */
static void test_picker_back_keys(void) {
    PtySession *s = open_picker_n(2);
    ASSERT(s != NULL, "picker back: opens");
    ASSERT_SCREEN_CONTAINS(s, "Esc/q/BS back");
    ASSERT_SCREEN_CONTAINS(s, "[1/2]");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Attachments (", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Attachments (");
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Attachments (", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Attachments (");
    pty_close(s);
}

/** Picker: ctrl-c quits the program at once. */
static void test_picker_ctrl_c_quits(void) {
    PtySession *s = open_picker_n(2);
    ASSERT(s != NULL, "picker ctrl-c: opens");
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, WAIT_MS) == 0, "picker: ctrl-c quits the program");
    pty_close(s);
}

/** Picker with 25 rows: j k g G Home End PgUp PgDn move as in every list. */
static void test_picker_navigation(void) {
    PtySession *s = open_picker_n(25);
    ASSERT(s != NULL, "picker navigation: opens");
    ASSERT_SCREEN_CONTAINS(s, "Attachments (25)");
    ASSERT_SCREEN_CONTAINS(s, "[1/25]");
    pty_send_str(s, "j");     ASSERT_WAIT_FOR(s, "[2/25]", WAIT_MS);
    pty_send_str(s, "k");     ASSERT_WAIT_FOR(s, "[1/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_DOWN); ASSERT_WAIT_FOR(s, "[2/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_UP);   ASSERT_WAIT_FOR(s, "[1/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_UP);   pty_settle(s, 200);
    ASSERT_SCREEN_CONTAINS(s, "[1/25]");      /* stops at the top */
    pty_send_str(s, "G");     ASSERT_WAIT_FOR(s, "[25/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_DOWN); pty_settle(s, 200);
    ASSERT_SCREEN_CONTAINS(s, "[25/25]");     /* stops at the bottom */
    pty_send_str(s, "g");     ASSERT_WAIT_FOR(s, "[1/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_END);  ASSERT_WAIT_FOR(s, "[25/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_HOME); ASSERT_WAIT_FOR(s, "[1/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_PGDN); ASSERT_WAIT_FOR(s, "[21/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_PGDN); ASSERT_WAIT_FOR(s, "[25/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_PGUP); ASSERT_WAIT_FOR(s, "[5/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_PGUP); ASSERT_WAIT_FOR(s, "[1/25]", WAIT_MS);
    /* Enter chooses the row under the cursor */
    pty_send_key(s, PTY_KEY_DOWN); ASSERT_WAIT_FOR(s, "[2/25]", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Save as:", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "data.bin");
    pty_close(s);
}

/** Picker: ? opens the help made from the key table, Esc closes only the help. */
static void test_picker_help(void) {
    PtySession *s = open_picker_n(2);
    ASSERT(s != NULL, "picker help: opens");
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Attachment picker shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "The attachments of the message.");     /* EMAIL-181 */
    ASSERT_SCREEN_CONTAINS(s, "Choose");
    ASSERT_SCREEN_CONTAINS(s, "Back");
    ASSERT_SCREEN_CONTAINS(s, "Page down");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Attachment picker shortcuts");
    ASSERT_SCREEN_CONTAINS(s, "Attachments (2)");
    pty_close(s);
}

/** Picker: an unbound key changes nothing and shows nothing. */
static void test_picker_unbound_key(void) {
    PtySession *s = open_picker_n(2);
    ASSERT(s != NULL, "picker unbound: opens");
    pty_send_str(s, "x");
    pty_send_str(s, "d");
    pty_send_str(s, "h");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Attachments (2)");
    ASSERT_SCREEN_CONTAINS(s, "[1/2]");
    pty_close(s);
}

/** One attachment: a goes straight to the save prompt; Esc there cancels
 *  without a file and returns to the reader. */
static void test_a_one_attachment(void) {
    PtySession *s = open_reader_n(1);
    ASSERT(s != NULL, "one attachment: opens");
    ASSERT_SCREEN_CONTAINS(s, "a save(1)");
    char path[512];
    snprintf(path, sizeof(path), "%s/notes.txt", g_test_home);
    remove(path);
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Save as:", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Attachments (");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Save as:");
    ASSERT(access(path, F_OK) != 0, "one attachment: Esc saved nothing");
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Save as:", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Saved:", WAIT_MS);
    ASSERT(access(path, F_OK) == 0, "one attachment: Enter saved the file");
    remove(path);
    pty_close(s);
}

/** No attachment: a says so; nothing else happens. */
static void test_a_without_attachments(void) {
    PtySession *s = open_reader_n(0);
    ASSERT(s != NULL, "no attachments: opens");
    ASSERT_SCREEN_NOT_CONTAINS(s, "a save(");
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "No attachments", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_close(s);
}

/** Picker, second row: Enter asks for data.bin; Esc in the prompt cancels
 *  back to the reader and saves nothing; ctrl-c in the prompt is not tested
 *  here (the line editor's own tests cover it). */
static void test_picker_choose_second_then_cancel(void) {
    PtySession *s = open_picker_n(2);
    ASSERT(s != NULL, "picker choose: opens");
    char path[512];
    snprintf(path, sizeof(path), "%s/data.bin", g_test_home);
    remove(path);
    pty_send_key(s, PTY_KEY_DOWN);
    ASSERT_WAIT_FOR(s, "[2/2]", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Save as:", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "data.bin");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Save as:");
    ASSERT(access(path, F_OK) != 0, "picker choose: Esc saved nothing");
    pty_close(s);
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
    PtySession *s = cli_open_size(120, 80, a)   /* list --help is ~60 lines */;
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
    send_host(s, "imaps://localhost:%d\n");   /* explicit correct protocol */
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
    send_host(s, "localhost:%d\n");            /* no protocol → auto imaps:// */
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
    send_host(s, "imap://localhost:%d\n");     /* wrong protocol */
    ASSERT_WAIT_FOR(s, "unsupported protocol", WAIT_MS);
    ASSERT_WAIT_FOR(s, "IMAP Host", WAIT_MS);       /* re-prompted */
    send_host(s, "imaps://localhost:%d\n");    /* now correct */
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
    send_host(s, "localhost:%d\n");               /* plain name → imaps:// prepended */
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

    /* Show must work from cache even with no server.
     * 60 rows: the rendered mock message is longer than a 24-row screen and
     * would scroll the header block away before the assertion runs. */
    stop_mock_server();
    { const char *a[] = {"show", "1", "--batch", NULL};
      PtySession *s = cli_open_size(COLS, 60, a);
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
static PtySession *tui_open_to_list_size(int cols, int rows) {
    const char *args[] = {g_tui_bin, NULL};
    PtySession *s = pty_open(cols, rows);
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

static PtySession *tui_open_to_list(void) {
    return tui_open_to_list_size(COLS, ROWS);
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

/* ── EMAIL-86: marks and bulk actions on the message list ───────────── */

static void test_tui_list_space_marks(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "marks: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    pty_send_str(s, " ");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1 marked]");
    /* one message: the cursor stays on it, so Space toggles the mark off */
    pty_send_str(s, " ");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    pty_send_str(s, " ");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1 marked]");
    pty_send_str(s, "x");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_list_esc_clears_marks(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "marks esc: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, " ");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1 marked]");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    ASSERT_SCREEN_CONTAINS(s, "message(s) in");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_list_bulk_star_marked(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "bulk star: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, " ");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1 marked]");
    menu_pick(s, MENU_IMAP_STAR);
    ASSERT_WAIT_FOR(s, "tarred", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_list_bulk_cursor_row(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "bulk cursor: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    menu_pick(s, MENU_IMAP_UNREAD);
    ASSERT_WAIT_FOR(s, "Marked as unread", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_list_bulk_cancel(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "bulk cancel: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, " ");
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Mark unread");
    ASSERT_SCREEN_CONTAINS(s, "[1 marked]");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* 'd' on a marked message asks, naming the message, and No is the default. */
static void test_tui_list_bulk_delete_confirm(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "bulk delete: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, " ");
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");        /* names the subject */
    pty_send_str(s, "n");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash");
    ASSERT_SCREEN_CONTAINS(s, "[1 marked]");
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);                   /* Enter on the default: No */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash");
    ASSERT_SCREEN_CONTAINS(s, "[1 marked]");
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Moved to Trash", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ── EMAIL-175: the keyboard policy of the message list ─────────────── */

/* The registry keys act at once, with no confirmation, and say what they did. */
static void test_keys_registry_routine(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "registry: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "+");
    ASSERT_WAIT_FOR(s, "Marked as read", WAIT_MS);
    pty_send_str(s, "-");
    ASSERT_WAIT_FOR(s, "Marked as unread", WAIT_MS);
    pty_send_str(s, "!");
    ASSERT_WAIT_FOR(s, "Marked as junk", WAIT_MS);
    pty_send_str(s, "!");
    ASSERT_WAIT_FOR(s, "Marked as not-junk", WAIT_MS);
    pty_send_str(s, "*");
    ASSERT_WAIT_FOR(s, "tarred", WAIT_MS);
    pty_send_str(s, "*");
    ASSERT_WAIT_FOR(s, "nstarred", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* Column of the junk letter of row @p row's status cell ("J-----"), or -1. */
static int junk_col(PtySession *s, int row) {
    char rt[256];
    pty_row_text(s, row, rt, sizeof rt);
    const char *p = strstr(rt, " J-----");
    return p ? (int)(p - rt) + 1 : -1;
}

/* EMAIL-136: '!' toggles junk on an IMAP account.  The server gets the flag
 * STOREs ($Junk set and $NotJunk cleared, then the reverse), the feedback line
 * says what was done, and the status column shows J (yellow) while junk. */
static void test_keys_junk_toggle_imap(void) {
    restart_mock();
    const char *log = test_mock_log(g_mock_pid);
    ASSERT(log != NULL, "junk imap: the mock has a log");
    if (!log) return;
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "junk imap: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT(test_log_count(log, "STORE") == 0, "junk imap: no STORE before the key");

    int row = -1;
    for (int r = 0; r < 30; r++)
        if (pty_row_contains(s, r, "Test Message")) { row = r; break; }
    ASSERT(row >= 0, "junk imap: the message row is listed");

    pty_send_str(s, "!");
    ASSERT_WAIT_FOR(s, "Marked as junk", WAIT_MS);
    ASSERT(test_log_wait(log, "UID STORE 0000000000000001 +FLAGS ($Junk)", 1, WAIT_MS) == 0,
           "junk imap: server got +FLAGS ($Junk)");
    ASSERT(test_log_count(log, "UID STORE 0000000000000001 -FLAGS ($NotJunk)") == 1,
           "junk imap: server got -FLAGS ($NotJunk)");
    ASSERT(test_log_count(log, "+FLAGS ($NotJunk)") == 0,
           "junk imap: $NotJunk is not set when marking junk");
    if (row >= 0) {
        int jc = junk_col(s, row);
        ASSERT(jc >= 0, "junk imap: status column shows J");
        if (jc >= 0) {
            int fg = pty_cell_fg(s, row, jc);
            ASSERT(fg == PTY_FG_YELLOW || fg == PTY_FG_YELLOW + 60, "junk imap: J is yellow");
        }
    }

    pty_send_str(s, "!");
    ASSERT_WAIT_FOR(s, "Marked as not-junk", WAIT_MS);
    ASSERT(test_log_wait(log, "UID STORE 0000000000000001 +FLAGS ($NotJunk)", 1, WAIT_MS) == 0,
           "junk imap: server got +FLAGS ($NotJunk)");
    ASSERT(test_log_count(log, "UID STORE 0000000000000001 -FLAGS ($Junk)") == 1,
           "junk imap: server got -FLAGS ($Junk)");
    ASSERT(test_log_count(log, "UID STORE 0000000000000001 +FLAGS ($Junk)") == 1,
           "junk imap: $Junk was set only once");
    if (row >= 0)
        ASSERT(junk_col(s, row) < 0, "junk imap: J is gone after the toggle back");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* The old letters that wrote to the server are gone: they change nothing. */
static void test_keys_old_letters_unbound(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "old letters: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    const char *old = "fnDjausm";
    for (const char *c = old; *c; c++) {
        char k[2] = { *c, 0 };
        pty_send_str(s, k);
        pty_settle(s, 200);
        ASSERT_SCREEN_NOT_CONTAINS(s, "Marked");
        ASSERT_SCREEN_NOT_CONTAINS(s, "tarred");
        ASSERT_SCREEN_NOT_CONTAINS(s, "Moved to Trash");
        ASSERT_SCREEN_NOT_CONTAINS(s, "syncing");
        ASSERT_SCREEN_NOT_CONTAINS(s, "Start a background sync");
    }
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* Delete and 'd' ask, name the message and default to No. */
static void test_keys_delete_dialog(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "delete dialog: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_DELETE);
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Delete");       /* the title */
    ASSERT_SCREEN_CONTAINS(s, "Test Message"); /* the subject */
    ASSERT_SCREEN_CONTAINS(s, "No");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Moved to Trash");
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);            /* the default is No */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Moved to Trash");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ':' lists every state-changing action with its direct key. */
static void test_keys_action_menu(void) {
    restart_mock();
    PtySession *s = tui_open_to_list_size(COLS, 40);
    ASSERT(s != NULL, "action menu: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Star");
    ASSERT_SCREEN_CONTAINS(s, "Mark read");
    ASSERT_SCREEN_CONTAINS(s, "Mark unread");
    ASSERT_SCREEN_CONTAINS(s, "Junk");
    ASSERT_SCREEN_CONTAINS(s, "Delete");
    ASSERT_SCREEN_CONTAINS(s, "Start a sync");
    ASSERT_SCREEN_CONTAINS(s, "Move to...");
    ASSERT_SCREEN_CONTAINS(s, "Toggle Done");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Start a sync");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* Starting a sync asks, naming the account; the default answer is No. */
static void test_keys_sync_asks(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "sync asks: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    menu_pick(s, MENU_IMAP_SYNC);
    ASSERT_WAIT_FOR(s, "Start a background sync", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "testuser");
    pty_send_key(s, PTY_KEY_ENTER);            /* Enter on the default: No */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Start a background sync");
    ASSERT_SCREEN_NOT_CONTAINS(s, "syncing");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ── EMAIL-129: creating and deleting an IMAP folder in the folder browser ── */

/* Opens the folder browser of the IMAP account (from the list: Esc). */
static PtySession *open_folder_browser(const char *what) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    int ok = s != NULL && pty_wait_for(s, "message(s) in", WAIT_MS) == 0;
    if (ok) {
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ESC);
        ok = pty_wait_for(s, "Folders", WAIT_MS) == 0;
    }
    g_tests_run++;
    if (!ok) {
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, what);
        g_tests_failed++;
        if (s) pty_close(s);
        return NULL;
    }
    pty_send_str(s, "l");                          /* the INBOX branch opens (EMAIL-200) */
    pty_settle(s, SETTLE_MS);
    return s;
}

/* The menu lists both actions; the hint bar names the menu; the old letters
 * stay harmless. */
static void test_folder_menu_lists_actions(void) {
    PtySession *s = open_folder_browser("folder menu: opens");
    if (!s) return;
    int sb = find_row(s, "step");
    ASSERT(sb >= 0 && pty_row_contains(s, sb, ": actions"), "folder menu: the hint bar names the menu");
    pty_send_str(s, "d");                          /* no direct delete key */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Delete the folder");
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Create a folder...");
    ASSERT_SCREEN_CONTAINS(s, "Delete the folder");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Create a folder...");
    ASSERT_SCREEN_CONTAINS(s, "Folders");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* The help names the menu key. */
static void test_folder_help_lists_menu(void) {
    PtySession *s = open_folder_browser("folder help: opens");
    if (!s) return;
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Folder browser shortcuts", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Actions");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* Create: the name is asked for, the server gets it and the list shows it. */
static void test_folder_create(void) {
    PtySession *s = open_folder_browser("folder create: opens");
    if (!s) return;
    ASSERT_SCREEN_NOT_CONTAINS(s, "Projects");
    menu_pick(s, 0);
    ASSERT_WAIT_FOR(s, "New folder name:", WAIT_MS);
    pty_send_str(s, "INBOX.Projects");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Folder INBOX.Projects created", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Projects");         /* listed from the server */
    /* The folder list is cached in the test home, which the other tests share:
     * delete the folder again so that they see the list they expect. */
    pty_send_str(s, "j");
    pty_settle(s, 200);
    int pr = find_row(s, "Projects");
    for (int i = 0; i < 12 && pr >= 0 && !pty_row_contains(s, pr, ">"); i++) {
        pty_send_str(s, "j");
        pty_settle(s, 200);
        pr = find_row(s, "Projects");
    }
    ASSERT(pr >= 0 && pty_row_contains(s, pr, ">"), "folder create: the cursor reaches Projects");
    menu_pick(s, 1);
    ASSERT_WAIT_FOR(s, "Delete the folder INBOX.Projects on the server?", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Folder INBOX.Projects deleted", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* Esc in the name prompt creates nothing. */
static void test_folder_create_cancelled(void) {
    PtySession *s = open_folder_browser("folder create cancel: opens");
    if (!s) return;
    menu_pick(s, 0);
    ASSERT_WAIT_FOR(s, "New folder name:", WAIT_MS);
    pty_send_str(s, "INBOX.Nope");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "New folder name:");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Nope");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* Delete asks first, naming the folder; No is the default; Yes deletes. */
static void test_folder_delete_asks(void) {
    PtySession *s = open_folder_browser("folder delete: opens");
    if (!s) return;
    int er = find_row(s, "Empty");
    ASSERT(er >= 0, "folder delete: the Empty folder is listed");
    for (int i = 0; i < 4 && er >= 0 && !pty_row_contains(s, er, ">"); i++) {
        pty_send_str(s, "j");
        pty_settle(s, 200);
        er = find_row(s, "Empty");
    }
    ASSERT(er >= 0 && pty_row_contains(s, er, ">"), "folder delete: the cursor is on Empty");
    menu_pick(s, 1);
    ASSERT_WAIT_FOR(s, "Delete the folder INBOX.Empty on the server?", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);                /* Enter on the default: No */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "on the server?");
    ASSERT_SCREEN_CONTAINS(s, "Empty");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Folder INBOX.Empty deleted");
    menu_pick(s, 1);
    ASSERT_WAIT_FOR(s, "on the server?", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Folder INBOX.Empty deleted", WAIT_MS);
    pty_send_str(s, "j");                          /* the message goes with the next key */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Empty");
    ASSERT_SCREEN_CONTAINS(s, "Sent");
    /* The folder list is cached in the test home, which the other tests share:
     * create the folder again so that they see the list they expect. */
    menu_pick(s, 0);
    ASSERT_WAIT_FOR(s, "New folder name:", WAIT_MS);
    pty_send_str(s, "INBOX.Empty");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Folder INBOX.Empty created", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* On a virtual row there is no folder to delete. */
static void test_folder_delete_needs_a_folder(void) {
    PtySession *s = open_folder_browser("folder delete virtual: opens");
    if (!s) return;
    pty_send_key(s, PTY_KEY_HOME);                 /* the Unread row */
    pty_settle(s, SETTLE_MS);
    menu_pick(s, 1);
    ASSERT_WAIT_FOR(s, "no folder selected", WAIT_MS);   /* the dialog names nothing */
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Select a folder to delete", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Empty");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ── EMAIL-97: resize and the minimum size ──────────────────────────── */

static void test_resize_list_redraws_at_once(void) {
    /* At 80 columns the key bar is cut; widening the window shows the rest
     * with no key pressed. */
    restart_mock();
    PtySession *s = tui_open_to_list_size(80, 24);
    ASSERT(s != NULL, "resize list: opens at 80x24");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "U refresh");
    ASSERT(pty_resize(s, 160, 24) == 0, "resize list: the window is resized");
    ASSERT_WAIT_FOR(s, "U refresh", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ── EMAIL-98: one status line ──────────────────────────────────────── */

static void test_statusline_one_row_hints_position(void) {
    /* The hints and the position are on the last row; no second bar above. */
    restart_mock();
    PtySession *s = tui_open_to_list_size(120, 24);
    ASSERT(s != NULL, "status line: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, 23, "Enter open"), "status line: hints on the last row");
    ASSERT(pty_row_contains(s, 23, "Esc/q/BS folders"), "status line: back hint kept");
    ASSERT(pty_row_contains(s, 23, "[1/"), "status line: position on the last row");
    ASSERT(pty_row_contains(s, 23, "? help"), "status line: help hint");
    ASSERT(!pty_row_contains(s, 22, "Enter open"), "status line: no second bar");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_statusline_message_replaces_hints(void) {
    /* After an action the message replaces the hints; the next key brings
     * the hints back. */
    restart_mock();
    PtySession *s = tui_open_to_list_size(120, 24);
    ASSERT(s != NULL, "status message: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "*");
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, 23, "arred"), "status message: shown on the last row");
    ASSERT(!pty_row_contains(s, 23, "Enter open"), "status message: replaces the hints");
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, 23, "Enter open"), "status message: the hints return");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_statusline_narrow_keeps_back_and_position(void) {
    /* A narrow line drops whole hints, lowest priority first; the way back
     * and the position stay. */
    restart_mock();
    PtySession *s = tui_open_to_list_size(80, 24);
    ASSERT(s != NULL, "status narrow: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, 23, "Esc/q/BS folders"), "status narrow: back hint kept");
    ASSERT(pty_row_contains(s, 23, "[1/"), "status narrow: position kept");
    ASSERT(!pty_row_contains(s, 23, "l rules"), "status narrow: low priority hint dropped");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_resize_keeps_selection(void) {
    restart_mock();
    PtySession *s = tui_open_to_list_size(100, 24);
    ASSERT(s != NULL, "resize selection: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_settle(s, SETTLE_MS);
    int row_before = -1;
    for (int r = 0; r < 24; r++)
        if (pty_row_contains(s, r, ">") && pty_row_contains(s, r, "Test Message")) row_before = r;
    pty_resize(s, 130, 30);
    pty_settle(s, SETTLE_MS * 2);
    int row_after = -1;
    for (int r = 0; r < 30; r++)
        if (pty_row_contains(s, r, ">") && pty_row_contains(s, r, "Test Message")) row_after = r;
    ASSERT(row_before >= 0 && row_after == row_before,
           "resize selection: the cursor stays on the same message");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_resize_below_minimum_shows_notice(void) {
    restart_mock();
    PtySession *s = tui_open_to_list_size(100, 24);
    ASSERT(s != NULL, "too small: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_settle(s, SETTLE_MS);
    pty_resize(s, 60, 20);
    ASSERT_WAIT_FOR(s, "Terminal too small", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "60x20");
    ASSERT_SCREEN_CONTAINS(s, "80x24");
    /* keys do not reach the list while the notice is shown */
    pty_send_str(s, "x");
    pty_settle(s, 200);
    ASSERT_SCREEN_CONTAINS(s, "Terminal too small");
    /* back to a usable size: the list is drawn again, selection kept */
    pty_resize(s, 100, 24);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    int sel = -1;
    for (int r = 0; r < 24; r++)
        if (pty_row_contains(s, r, ">") && pty_row_contains(s, r, "Test Message")) sel = r;
    ASSERT(sel >= 0, "too small: the selection is kept across the notice");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_start_below_minimum_shows_notice(void) {
    restart_mock();
    const char *args[] = {g_tui_bin, NULL};
    PtySession *s = pty_open(60, 16);
    ASSERT(s != NULL, "start small: opens");
    ASSERT(pty_run(s, args) == 0, "start small: runs");
    ASSERT_WAIT_FOR(s, "Terminal too small", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "60x16");
    pty_resize(s, 100, 24);
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_resize_on_every_screen(void) {
    /* accounts, folders, list, reader: each is redrawn by a resize alone. */
    restart_mock();
    const char *args[] = {g_tui_bin, NULL};
    PtySession *s = pty_open(80, 24);
    ASSERT(s != NULL, "resize screens: opens");
    ASSERT(pty_run(s, args) == 0, "resize screens: runs");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "ESC quit");
    pty_resize(s, 120, 24);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "ESC quit");
    ASSERT_SCREEN_CONTAINS(s, "e SMTP");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_resize(s, 90, 30);
    pty_settle(s, SETTLE_MS * 2);
    ASSERT_SCREEN_CONTAINS(s, "Folders");
    ASSERT_SCREEN_CONTAINS(s, "INBOX");
    folders_goto_inbox(s);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_resize(s, 130, 26);
    pty_settle(s, SETTLE_MS * 2);
    ASSERT_SCREEN_CONTAINS(s, "message(s) in");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_resize(s, 100, 24);
    pty_settle(s, SETTLE_MS * 2);
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}



/* ══════════════════════════════════════════════════════════════════════
 *  EMAIL-143: key study of the message list of an IMAP account (80x24)
 *  docs/spec/tui-keys.md, "Message list, IMAP".  30 messages: 20 unread
 *  (UIDs 1-20, listed first, newest first), 10 read.  The list window is 18
 *  rows, so the cursor row numbers below are [n/30].
 * ══════════════════════════════════════════════════════════════════════ */

/** Opens the list at 80x24 with @p count mock messages (0: the folder
 *  INBOX.Empty); the caller closes the session and calls lk_env(0). */
static void lk_env(int count) {
    /* The messages of one test are not the messages of the next: the list
     * reads the cached headers of INBOX before it asks the server. */
    test_rmtree(TP("home/.local/share/email-cli/accounts/testuser"));
    if (count > 1) {
        char n[16];
        snprintf(n, sizeof n, "%d", count);
        setenv("MOCK_IMAP_COUNT", n, 1);
    } else {
        unsetenv("MOCK_IMAP_COUNT");
    }
}

static PtySession *lk_open(void) {
    restart_mock();
    PtySession *s = tui_open_to_list_size(80, 24);
    if (!s) return NULL;
    if (pty_wait_for(s, "message(s) in", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static PtySession *lk_open_empty(void) {
    restart_mock();
    PtySession *s = tui_open_to_list_size(80, 24);
    if (!s) return NULL;
    if (pty_wait_for(s, "message(s) in", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_key(s, PTY_KEY_BACK);
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");                          /* open INBOX (EMAIL-200) */
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_DOWN);                 /* INBOX.AT&T */
    pty_send_key(s, PTY_KEY_DOWN);                 /* INBOX.Empty */
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "0 of 0 message(s) in", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/** The text of the row under the cursor ('>' in column 0), or "". */
static const char *lk_cursor_row(PtySession *s, char *buf, size_t size) {
    buf[0] = '\0';
    for (int r = 3; r < 23; r++) {
        pty_row_text(s, r, buf, size);
        if (buf[0] == '>') return buf;
    }
    buf[0] = '\0';
    return buf;
}

/** Sends @p key, waits until the status line shows @p pos, e.g. "[3/30]". */
static int lk_step(PtySession *s, PtyKey key, const char *pos) {
    pty_send_key(s, key);
    return pty_wait_for(s, pos, WAIT_MS) == 0;
}

static int lk_char(PtySession *s, const char *k, const char *pos) {
    pty_send_str(s, k);
    return pty_wait_for(s, pos, WAIT_MS) == 0;
}

static void lk_nav_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list nav: opens");
    if (!s) return;
    char row[200];
    ASSERT_SCREEN_CONTAINS(s, "[1/30]");
    ASSERT_SCREEN_CONTAINS(s, "1-19 of 30 message(s)");
    ASSERT_ROW_CONTAINS(s, 22, "Message 2");           /* the 19th row is used */
    ASSERT(strstr(lk_cursor_row(s, row, sizeof row), "Message 20") != NULL,
           "list nav: the cursor starts on the newest unread message");
    ASSERT(lk_char(s, "j", "[2/30]"), "list nav: j steps down");
    ASSERT(lk_step(s, PTY_KEY_DOWN, "[3/30]"), "list nav: Down steps down");
    ASSERT(lk_char(s, "k", "[2/30]"), "list nav: k steps up");
    ASSERT(lk_step(s, PTY_KEY_UP, "[1/30]"), "list nav: Up steps up");
    pty_send_key(s, PTY_KEY_UP);
    pty_send_str(s, "k");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1/30]");              /* stops at the first */
    ASSERT(lk_step(s, PTY_KEY_PGDN, "[20/30]"), "list nav: PgDn goes one page (19 rows) down");
    ASSERT(lk_cursor_row(s, row, sizeof row)[0] == '>',
           "list nav: the cursor row is on the screen after PgDn");
    ASSERT(lk_step(s, PTY_KEY_PGDN, "[30/30]"), "list nav: PgDn stops at the last");
    ASSERT(lk_step(s, PTY_KEY_PGUP, "[11/30]"), "list nav: PgUp goes one page up");
    ASSERT(lk_step(s, PTY_KEY_END, "[30/30]"), "list nav: End goes to the last");
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_str(s, "j");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[30/30]");             /* stops at the last */
    ASSERT(lk_step(s, PTY_KEY_HOME, "[1/30]"), "list nav: Home goes to the first");
    ASSERT(strstr(lk_cursor_row(s, row, sizeof row), "Message 20") != NULL,
           "list nav: Home scrolls back to the top");
    ASSERT(lk_char(s, "G", "[30/30]"), "list nav: G goes to the last, as End (EMAIL-201)");
    ASSERT(lk_char(s, "g", "[1/30]"), "list nav: g goes to the first, as Home (EMAIL-201)");
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

/* Every cursor key, with 30 messages; the page is the 19 rows of the table. */
static void test_lk_navigation(void) {
    lk_env(30);
    lk_nav_body();
    lk_env(0);
}

/* The header range "a-b of N" counts the rows on the screen after a scroll
 * (EMAIL-185). */
static void lk_range_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list range: opens");
    if (!s) return;
    pty_send_key(s, PTY_KEY_END);
    ASSERT_WAIT_FOR(s, "[30/30]", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    int first = -1, last = -1;
    for (int r = 4; r < 23; r++) {
        char t[200];
        pty_row_text(s, r, t, sizeof t);
        if (strstr(t, "Message ") || strstr(t, "20")) { if (first < 0) first = r; last = r; }
    }
    int rows = last - first + 1;
    char want[32];
    char hdr[200];
    pty_row_text(s, 0, hdr, sizeof hdr);
    int a = 0, b = 0, n = 0;
    ASSERT(sscanf(hdr, " %d-%d of %d", &a, &b, &n) == 3, "list range: the header has a range");
    snprintf(want, sizeof want, "%d rows", b - a + 1);
    ASSERT(b - a + 1 == rows, want);
    ASSERT(b == 30, "list range: the range ends at the cursor message");
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

static void test_lk_header_range_matches_rows(void) {
    lk_env(30);
    lk_range_body();
    lk_env(0);
}

/** Waits for the compose dialog of a reply / forward, cancels it and returns
 *  to the list. */
static int lk_dialog_and_back(PtySession *s, const char *text) {
    if (pty_wait_for(s, text, WAIT_MS) != 0) return 0;
    pty_send_key(s, PTY_KEY_ESC);
    /* A dialog with text in it (the reply's To:, the subject) asks first. */
    for (int ms = 0; ms < WAIT_MS; ms += 100) {
        pty_settle(s, 100);
        if (pty_screen_contains(s, "Discard what was typed?")) { pty_send_str(s, "y"); break; }
        if (pty_screen_contains(s, "message(s) in") && !pty_screen_contains(s, text)) break;
    }
    /* A cancel returns to the list at once, with no pause (EMAIL-222). */
    if (pty_wait_for(s, "message(s) in", WAIT_MS) != 0) return 0;
    pty_settle(s, 200);
    return !pty_screen_contains(s, "Press any key to return");
}

/* c r A F act on the message under the cursor; Esc in the dialog returns to
 * the list with the cursor where it was. */
static void lk_compose_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list compose keys: opens");
    if (!s) return;
    ASSERT(lk_char(s, "j", "[2/30]"), "list compose keys: j");
    pty_send_str(s, "c");
    ASSERT(lk_dialog_and_back(s, "New Message"), "list compose keys: c opens the compose dialog");
    ASSERT_SCREEN_CONTAINS(s, "[2/30]");
    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Re: Message 19", WAIT_MS);
    ASSERT(lk_dialog_and_back(s, "Re: Message 19"), "list compose keys: r replies to the cursor message");
    ASSERT_SCREEN_CONTAINS(s, "[2/30]");
    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Re: Message 19", WAIT_MS);
    ASSERT(lk_dialog_and_back(s, "Re: Message 19"), "list compose keys: A replies to all");
    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Fwd: Message 19", WAIT_MS);
    ASSERT(lk_dialog_and_back(s, "Fwd: Message 19"), "list compose keys: F forwards the cursor message");
    ASSERT_SCREEN_CONTAINS(s, "[2/30]");
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

static void test_lk_compose_reply_forward(void) {
    lk_env(30);
    lk_compose_body();
    lk_env(0);
}

/* Space marks and steps down, x clears, Esc drops the marks first and goes
 * back only the second time. */
static void lk_marks_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list marks: opens");
    if (!s) return;
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[1 marked]", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[2/30]", WAIT_MS);              /* stepped down */
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[2 marked]", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[3/30]", WAIT_MS);
    pty_send_str(s, "x");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    ASSERT_SCREEN_CONTAINS(s, "[3/30]");              /* x does not move */
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[1 marked]", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    ASSERT_SCREEN_CONTAINS(s, "message(s) in");       /* still in the list */
    pty_send_key(s, PTY_KEY_END);
    ASSERT_WAIT_FOR(s, "[30/30]", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[1 marked]", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "[30/30]");             /* the last row: no step */
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

static void test_lk_marks(void) {
    lk_env(30);
    lk_marks_body();
    lk_env(0);
}

/* Enter opens the reader on the cursor message; Backspace, Esc and q return
 * to the list with the cursor where it was. */
static void lk_enter_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list enter: opens");
    if (!s) return;
    ASSERT(lk_char(s, "j", "[2/30]"), "list enter: j");
    ASSERT(lk_char(s, "j", "[3/30]"), "list enter: j");
    const char *back[] = { NULL, "q", NULL };
    for (int i = 0; i < 3; i++) {
        pty_send_key(s, PTY_KEY_ENTER);
        ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
        ASSERT_WAIT_FOR(s, "Message 18", WAIT_MS);
        if (i == 0) pty_send_key(s, PTY_KEY_BACK);
        else if (i == 1) pty_send_str(s, back[1]);
        else pty_send_key(s, PTY_KEY_ESC);
        ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
        pty_settle(s, SETTLE_MS);
        ASSERT_SCREEN_CONTAINS(s, "[3/30]");
    }
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

static void test_lk_enter_opens_reader(void) {
    lk_env(30);
    lk_enter_body();
    lk_env(0);
}

/* Esc, q, Backspace, Left and h go back to the folders (EMAIL-202); ctrl-c
 * quits at once. */
static void lk_back_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list back: opens");
    if (!s) return;
    for (int i = 0; i < 5; i++) {
        if (i == 0) pty_send_key(s, PTY_KEY_ESC);
        else if (i == 1) pty_send_str(s, "q");
        else if (i == 2) pty_send_key(s, PTY_KEY_BACK);
        else if (i == 3) pty_send_key(s, PTY_KEY_LEFT);
        else pty_send_str(s, "h");
        ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
        pty_settle(s, SETTLE_MS);
        folders_goto_inbox(s);
        pty_send_key(s, PTY_KEY_ENTER);
        ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
        pty_settle(s, SETTLE_MS);
    }
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, 5000) == 0, "list back: ctrl-c quits at once");
    pty_close(s);
}

static void test_lk_back_keys(void) {
    lk_env(30);
    lk_back_body();
    lk_env(0);
}

/* U refreshes (the list is drawn again, the cursor starts over), l opens the
 * rules editor and Esc returns to the list. */
static void lk_refresh_rules_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list U l: opens");
    if (!s) return;
    ASSERT(lk_char(s, "j", "[2/30]"), "list U l: j");
    pty_send_str(s, "U");
    pty_settle(s, SETTLE_MS);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "of 30 message(s)");
    ASSERT_SCREEN_CONTAINS(s, "[2/30]");              /* the cursor stays (EMAIL-203) */
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "Rules for", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "of 30 message(s)");
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

static void test_lk_refresh_and_rules(void) {
    lk_env(30);
    lk_refresh_rules_body();
    lk_env(0);
}

/* ? opens the help made from the key table, at 80x24 it scrolls; Esc closes
 * it (and only it); ? is the only help key; the movement keys scroll the
 * overlay, not the list. */
static void lk_help_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list help: opens");
    if (!s) return;
    pty_send_str(s, "z");                             /* no help key but ? */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Message list shortcuts");
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Message list shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Down - move the cursor down");
    pty_send_str(s, "j");
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_END);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Quit");               /* the last entry is reachable */
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Message list shortcuts");
    ASSERT_SCREEN_CONTAINS(s, "message(s) in");
    ASSERT_SCREEN_CONTAINS(s, "[1/30]");              /* the list did not move */
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

static void test_lk_help_overlay(void) {
    lk_env(30);
    lk_help_body();
    lk_env(0);
}

/* The keys of older versions and other views do nothing in the list: no
 * dialog, no movement, no message. */
static void lk_unbound_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list unbound: opens");
    if (!s) return;
    ASSERT(lk_char(s, "j", "[2/30]"), "list unbound: j");
    const char *keys[] = { "n", "f", "s", "m", "D", "R", "a", "t", "u", "v", "N", NULL };
    for (int i = 0; keys[i]; i++) {
        pty_send_str(s, keys[i]);
        pty_settle(s, 300);
        char what[64];
        snprintf(what, sizeof what, "list unbound: %s changes nothing", keys[i]);
        ASSERT(pty_screen_contains(s, "[2/30]") && pty_screen_contains(s, "message(s) in")
               && !pty_screen_contains(s, "Actions") && !pty_screen_contains(s, "?]"), what);
    }
    pty_send_key(s, PTY_KEY_RIGHT);
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[2/30]");
    ASSERT_SCREEN_CONTAINS(s, "message(s) in");
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

static void test_lk_unbound_keys(void) {
    lk_env(30);
    lk_unbound_body();
    lk_env(0);
}

/* A filter that matches nothing leaves no message to act on: Enter, r, A, F,
 * d and Space do nothing (and open nothing) (EMAIL-184). */
static void lk_filter_nomatch_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list filter none: opens");
    if (!s) return;
    pty_send_str(s, "/");
    ASSERT_WAIT_FOR(s, "Filter", WAIT_MS);
    pty_send_str(s, "zzzz");
    ASSERT_WAIT_FOR(s, "Showing 0 of", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);                   /* commit the text */
    pty_settle(s, SETTLE_MS);
    const char *keys[] = { "\r", "r", "A", "F", "d", " ", NULL };
    for (int i = 0; keys[i]; i++) {
        pty_send_str(s, keys[i]);
        pty_settle(s, SETTLE_MS);
        ASSERT_SCREEN_CONTAINS(s, "Showing 0 of");
        ASSERT_SCREEN_NOT_CONTAINS(s, "From:");
        ASSERT_SCREEN_NOT_CONTAINS(s, "Re: ");
        ASSERT_SCREEN_NOT_CONTAINS(s, "Fwd: ");
        ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash");
        ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    }
    pty_send_key(s, PTY_KEY_ESC);                     /* clears the filter */
    ASSERT_WAIT_FOR(s, "of 30 message(s)", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

static void test_lk_filter_without_match(void) {
    lk_env(30);
    lk_filter_nomatch_body();
    lk_env(0);
}

/* d and the Delete key ask first, naming the message of the cursor row, and
 * No is the default; y moves it to the Trash. */
static void lk_delete_body(void) {
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list delete: opens");
    if (!s) return;
    ASSERT(lk_char(s, "j", "[2/30]"), "list delete: j");
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Message 19");
    pty_send_str(s, "n");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash");
    ASSERT_SCREEN_CONTAINS(s, "of 30 message(s)");
    pty_send_key(s, PTY_KEY_DELETE);
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Message 19");
    pty_send_key(s, PTY_KEY_ESC);                     /* Esc is No */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash");
    ASSERT_SCREEN_CONTAINS(s, "message(s) in");
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Moved to Trash", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

static void test_lk_delete_asks(void) {
    lk_env(30);
    lk_delete_body();
    lk_env(0);
}

/* One message: every cursor key stays on it, Enter opens it, Space marks it
 * and does not step. */
static void test_lk_one_message(void) {
    lk_env(1);
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list one: opens");
    if (!s) { lk_env(0); return; }
    ASSERT_SCREEN_CONTAINS(s, "[1/1]");
    const char *keys[] = { "j", "k", "G", NULL };
    for (int i = 0; keys[i]; i++) pty_send_str(s, keys[i]);
    PtyKey nav[] = { PTY_KEY_DOWN, PTY_KEY_UP, PTY_KEY_PGDN, PTY_KEY_PGUP, PTY_KEY_END, PTY_KEY_HOME };
    for (size_t i = 0; i < sizeof nav / sizeof nav[0]; i++) pty_send_key(s, nav[i]);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1/1]");
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[1 marked]", WAIT_MS);
    pty_send_str(s, "x");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Message list shortcuts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1/1]");
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
    lk_env(0);
}

/* The empty folder (0 messages): the status line offers only keys that work;
 * ? opens the help; c, U, l, :, Esc/q/BS and ctrl-c work; the table header
 * fits the 80 columns (EMAIL-187, EMAIL-188, EMAIL-189). */
static void test_lk_empty_folder(void) {
    lk_env(0);
    PtySession *s = lk_open_empty();
    ASSERT(s != NULL, "list empty: opens");
    if (!s) return;
    ASSERT_ROW_CONTAINS(s, 23, "Esc/q/BS folders");
    ASSERT_ROW_CONTAINS(s, 23, "? help");
    ASSERT_ROW_CONTAINS(s, 23, "c compose");
    ASSERT_SCREEN_CONTAINS(s, "(empty)  U lists again after a sync.");   /* EMAIL-207 */
    ASSERT(!pty_row_contains(s, 23, "Enter open"), "list empty: no Enter hint");
    ASSERT(!pty_row_contains(s, 23, "j/k"), "list empty: no j/k hint");
    ASSERT(!pty_row_contains(s, 23, "d trash"), "list empty: no d hint");
    char r4[200];
    pty_row_text(s, 4, r4, sizeof r4);
    ASSERT(strstr(r4, "\xe2\x95\x90") == NULL, "list empty: the header rule does not wrap into the next row");
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Message list shortcuts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Message list shortcuts");
    ASSERT_SCREEN_CONTAINS(s, "0 of 0 message(s)");
    const char *dead[] = { "j", "k", " ", "x", "d", "r", "A", "F", "*", "+", "-", "!", "/", "n", "s", "g", "G", NULL };
    for (int i = 0; dead[i]; i++) pty_send_str(s, dead[i]);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_PGDN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "0 of 0 message(s)");
    ASSERT_SCREEN_NOT_CONTAINS(s, "From:");
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "0 of 0 message(s)");
    pty_send_str(s, "U");
    ASSERT_WAIT_FOR(s, "0 of 0 message(s)", WAIT_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "Rules for", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "0 of 0 message(s)", WAIT_MS);
    pty_send_str(s, "h");                             /* back, as q and Left (EMAIL-202) */
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}


/* ══════════════════════════════════════════════════════════════════════
 *  EMAIL-146: key study of the message reader of an IMAP account (80x24)
 *  docs/spec/tui-keys.md, "Message reader".  The mock's single message
 *  (lk_env(0)) is 25 lines long: 14 rows are shown, two pages, the last
 *  page starts at line 11.  lk_env(2) gives messages with a one-line body.
 * ══════════════════════════════════════════════════════════════════════ */

/** Opens the reader on the cursor message; NULL when it did not come up. */
static PtySession *rk_open(int count) {
    lk_env(count);
    PtySession *s = lk_open();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "From:", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/** The 24 rows of the screen as one text, to tell that a key changed nothing. */
static void rk_snap(PtySession *s, char *buf, size_t size) {
    size_t n = 0;
    buf[0] = '\0';
    for (int r = 0; r < 24 && n + 1 < size; r++) {
        char t[300];
        pty_row_text(s, r, t, sizeof t);
        n += (size_t)snprintf(buf + n, size - n, "%s\n", t);
    }
}

static void rk_end(PtySession *s) {
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
    lk_env(0);
}

/* Down j / Up k scroll a line and stop at the ends, PgDn PgUp a page (never
 * past the last full page), Home g / End G the ends; the status line shows the
 * page, the last one at the bottom (EMAIL-146). */
static void test_rk_scroll(void) {
    PtySession *s = rk_open(0);
    ASSERT(s != NULL, "reader scroll: opens");
    if (!s) return;
    ASSERT_SCREEN_CONTAINS(s, "[1/2]");
    ASSERT_ROW_CONTAINS(s, 9, "Hello from Mock Server");
    pty_send_str(s, "j");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 10, "Bold <styled>");
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Bold <styled>");
    pty_send_str(s, "k");
    pty_send_key(s, PTY_KEY_UP);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Hello from Mock Server");
    pty_send_key(s, PTY_KEY_UP);
    pty_send_str(s, "k");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Hello from Mock Server");       /* stops at the top */
    pty_send_key(s, PTY_KEY_PGUP);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Hello from Mock Server");
    pty_send_key(s, PTY_KEY_PGDN);
    ASSERT_WAIT_FOR(s, "[2/2]", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Dark italic");                  /* the last full page */
    ASSERT_SCREEN_CONTAINS(s, "https://click.example.com/test");
    pty_send_key(s, PTY_KEY_PGDN);
    pty_send_str(s, "j");
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Dark italic");                  /* stops at the bottom */
    ASSERT_SCREEN_CONTAINS(s, "[2/2]");
    pty_send_key(s, PTY_KEY_UP);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1/2]");
    pty_send_key(s, PTY_KEY_PGUP);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Hello from Mock Server");
    pty_send_key(s, PTY_KEY_END);
    ASSERT_WAIT_FOR(s, "[2/2]", WAIT_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Dark italic");
    pty_send_key(s, PTY_KEY_HOME);
    ASSERT_WAIT_FOR(s, "[1/2]", WAIT_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Hello from Mock Server");
    pty_send_str(s, "G");
    ASSERT_WAIT_FOR(s, "[2/2]", WAIT_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Dark italic");
    pty_send_str(s, "g");
    ASSERT_WAIT_FOR(s, "[1/2]", WAIT_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Hello from Mock Server");
    ASSERT_ROW_CONTAINS(s, 23, "j/k scroll");
    rk_end(s);
}

/* Esc, q, Backspace, Left and h return to the list with the cursor kept;
 * ctrl-c quits at once. */
static void test_rk_back_keys(void) {
    lk_env(30);
    PtySession *s = lk_open();
    ASSERT(s != NULL, "reader back: opens");
    if (!s) return;
    ASSERT(lk_char(s, "j", "[2/30]"), "reader back: j");
    for (int i = 0; i < 5; i++) {
        pty_send_key(s, PTY_KEY_ENTER);
        ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
        pty_settle(s, SETTLE_MS);
        if (i == 0) pty_send_key(s, PTY_KEY_ESC);
        else if (i == 1) pty_send_str(s, "q");
        else if (i == 2) pty_send_key(s, PTY_KEY_BACK);
        else if (i == 3) pty_send_key(s, PTY_KEY_LEFT);
        else pty_send_str(s, "h");
        ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
        pty_settle(s, SETTLE_MS);
        ASSERT_SCREEN_CONTAINS(s, "[2/30]");
    }
    /* The mock serves one connection at a time and the flag pushes of the
     * round trips queue behind the open one, so the quit is tried on a fresh
     * session (test_rk_ctrl_c), not after them. */
    rk_end(s);
}

/* ctrl-c in the reader quits at once. */
static void test_rk_ctrl_c(void) {
    PtySession *s = rk_open(2);
    ASSERT(s != NULL, "reader ctrl-c: opens");
    if (!s) return;
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, 5000) == 0, "reader ctrl-c: quits at once");
    pty_close(s);
    lk_env(0);
}

/* A message that fits the screen: no key scrolls, the page stays [1/1]. */
static void test_rk_short_message(void) {
    PtySession *s = rk_open(2);
    ASSERT(s != NULL, "reader short: opens");
    if (!s) return;
    ASSERT_SCREEN_CONTAINS(s, "[1/1]");
    char before[8192], after[8192];
    rk_snap(s, before, sizeof before);
    const char *keys[] = { "j", "k", "g", "G", NULL };
    for (int i = 0; keys[i]; i++) pty_send_str(s, keys[i]);
    PtyKey nav[] = { PTY_KEY_DOWN, PTY_KEY_UP, PTY_KEY_PGDN, PTY_KEY_PGUP, PTY_KEY_END, PTY_KEY_HOME };
    for (size_t i = 0; i < sizeof nav / sizeof nav[0]; i++) pty_send_key(s, nav[i]);
    pty_settle(s, SETTLE_MS);
    rk_snap(s, after, sizeof after);
    ASSERT(strcmp(before, after) == 0, "reader short: no scroll key changes the screen");
    ASSERT_SCREEN_CONTAINS(s, "[1/1]");
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    rk_end(s);
}

/* / searches the body: Enter finds the first match, n and N go on (No more
 * matches wrap round), Esc cancels, Backspace edits, a miss says so, ctrl-c
 * in the prompt quits at once. */
static void test_rk_search(void) {
    PtySession *s = rk_open(0);
    ASSERT(s != NULL, "reader search: opens");
    if (!s) return;
    pty_send_str(s, "n");                              /* no search yet: nothing */
    pty_send_str(s, "N");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "No more matches");
    pty_send_str(s, "/");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 23, "/");
    pty_send_str(s, "Itemx");
    pty_send_key(s, PTY_KEY_BACK);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 23, "/Item_");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Item one");              /* the first match is the top row */
    pty_send_str(s, "n");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Item two");
    pty_send_str(s, "n");                              /* the search wraps round */
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Item one");
    pty_send_str(s, "N");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Item two");
    pty_send_str(s, "N");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 9, "Item one");
    pty_send_str(s, "/");                              /* Esc cancels the prompt */
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 23, "/");
    pty_send_str(s, "Click");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "/Click");
    ASSERT_ROW_CONTAINS(s, 9, "Item one");
    pty_send_str(s, "/");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 23, "/");
    pty_send_str(s, "xyzzy");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "No match: xyzzy", WAIT_MS);
    pty_send_str(s, "/");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 23, "/");
    pty_send_str(s, "qh");                             /* letters are text in the prompt */
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 23, "/qh_");
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    rk_end(s);
    /* A fresh session: the mock serves one connection at a time. */
    s = rk_open(2);
    ASSERT(s != NULL, "reader search: opens again");
    if (!s) return;
    pty_send_str(s, "/");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 23, "/");
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, 5000) == 0, "reader search: ctrl-c in the prompt quits at once");
    pty_close(s);
    lk_env(0);
}

/* * + - ! act at once and say so; a second press undoes; v shows the source
 * and back; ? opens the help, only Esc/q close it; : opens the menu, Esc
 * closes it. */
static void test_rk_state_keys(void) {
    PtySession *s = rk_open(0);
    ASSERT(s != NULL, "reader state: opens");
    if (!s) return;
    pty_send_str(s, "*");
    ASSERT_WAIT_FOR(s, "Starred", WAIT_MS);
    pty_send_str(s, "*");
    ASSERT_WAIT_FOR(s, "Unstarred", WAIT_MS);
    pty_send_str(s, "-");                              /* the mock's message is unread */
    ASSERT_WAIT_FOR(s, "Already unread", WAIT_MS);
    pty_send_str(s, "+");
    ASSERT_WAIT_FOR(s, "Marked as read", WAIT_MS);
    pty_send_str(s, "+");
    ASSERT_WAIT_FOR(s, "Already read", WAIT_MS);
    pty_send_str(s, "-");
    ASSERT_WAIT_FOR(s, "Marked as unread", WAIT_MS);
    pty_send_str(s, "!");
    ASSERT_WAIT_FOR(s, "Marked as junk", WAIT_MS);
    pty_send_str(s, "!");
    ASSERT_WAIT_FOR(s, "Marked as not-junk", WAIT_MS);
    pty_send_str(s, "v");
    ASSERT_WAIT_FOR(s, "Content-Type", WAIT_MS);
    pty_send_str(s, "v");
    ASSERT_WAIT_FOR(s, "Hello from Mock Server", WAIT_MS);
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Actions");
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Message reader shortcuts", WAIT_MS);
    pty_send_str(s, "x");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Message reader shortcuts");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Message reader shortcuts");
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Message reader shortcuts", WAIT_MS);
    pty_send_str(s, "q");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Message reader shortcuts");
    ASSERT_SCREEN_CONTAINS(s, "From:");
    rk_end(s);
}

/* d and Delete ask first, naming the message, No is the default (Enter, Esc,
 * n); y moves it to the Trash and returns to the list.  Run on the
 * 30-message mock: the single-message one does not finish the move. */
static void test_rk_delete(void) {
    PtySession *s = rk_open(30);
    ASSERT(s != NULL, "reader delete: opens");
    if (!s) return;
    PtyKey no[] = { PTY_KEY_ENTER, PTY_KEY_ESC };
    for (int i = 0; i < 4; i++) {
        if (i % 2) pty_send_key(s, PTY_KEY_DELETE);
        else pty_send_str(s, "d");
        ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
        if (i < 2) pty_send_key(s, no[i]);
        else pty_send_str(s, "n");
        ASSERT_WAIT_FOR(s, "Not deleted", WAIT_MS);
        ASSERT_SCREEN_CONTAINS(s, "From:");
        pty_settle(s, SETTLE_MS);
    }
    rk_end(s);
    s = rk_open(30);                                   /* a fresh session (one connection at a time) */
    ASSERT(s != NULL, "reader delete: opens again");
    if (!s) return;
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "From:");
    rk_end(s);
}

/* r, A and F open the dialog for this message and return to the list. */
static void test_rk_reply_forward(void) {
    PtySession *s = rk_open(0);
    ASSERT(s != NULL, "reader reply: opens");
    if (!s) return;
    pty_send_str(s, "r");
    ASSERT(lk_dialog_and_back(s, "Re: Test Message"), "reader reply: r replies");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_send_str(s, "A");
    ASSERT(lk_dialog_and_back(s, "Re: Test Message"), "reader reply: A replies to all");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_send_str(s, "F");
    ASSERT(lk_dialog_and_back(s, "Fwd: Test Message"), "reader reply: F forwards");
    rk_end(s);
}

/* a: with two attachments the picker, Esc returns to the reader; with none
 * "No attachments"; the menu entry too. */
static void test_rk_attachments(void) {
    PtySession *s = rk_open(0);
    ASSERT(s != NULL, "reader attach: opens");
    if (!s) return;
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "notes.txt", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "data.bin");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "From:");
    pty_close(s);
    setenv("MOCK_IMAP_ATTACHMENTS", "0", 1);
    s = rk_open(0);
    ASSERT(s != NULL, "reader attach: opens without attachments");
    if (!s) { unsetenv("MOCK_IMAP_ATTACHMENTS"); return; }
    ASSERT(!pty_row_contains(s, 23, "save("), "reader attach: no save hint");
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "No attachments", WAIT_MS);
    pty_send_str(s, "A");
    ASSERT(lk_dialog_and_back(s, "Re: Test Message"), "reader attach: A is reply-all, not save-all");
    unsetenv("MOCK_IMAP_ATTACHMENTS");
    rk_end(s);
}

/* The keys the reader does not bind change nothing and show nothing. */
static void test_rk_unbound_keys(void) {
    PtySession *s = rk_open(0);
    ASSERT(s != NULL, "reader unbound: opens");
    if (!s) return;
    char before[8192], after[8192];
    rk_snap(s, before, sizeof before);
    const char *dead[] = { "f", "s", "m", "D", "R", "t", "u", "e", "i", "p", "o", "c", "l", "U", " ", "x", NULL };
    for (int i = 0; dead[i]; i++) pty_send_str(s, dead[i]);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_send_key(s, PTY_KEY_RIGHT);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_F1);
    pty_send_key(s, PTY_KEY_INSERT);
    pty_settle(s, SETTLE_MS);
    rk_snap(s, after, sizeof after);
    ASSERT(strcmp(before, after) == 0, "reader unbound: no other key changes the screen");
    rk_end(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  EMAIL-142: key study of the folder browser of an IMAP account (80x24)
 *  docs/spec/tui-keys.md, "Folder browser".  The mock lists INBOX with 7
 *  subfolders (16 table rows with the 8 virtual ones: [2/16] is Unread,
 *  [9/16] INBOX); MOCK_IMAP_FOLDERS=N lists INBOX and N-1 subfolders F01...
 * ══════════════════════════════════════════════════════════════════════ */

/** The folder list of the mock for the next test (-1: the fixed list), and
 *  a start without a stored cursor, view mode or cached folder list. */
static void fk_env(int folders) {
    char p[512];
    snprintf(p, sizeof p, "%s/.local/share/email-cli/ui.ini", g_test_home);
    remove(p);
    test_rmtree(TP("home/.local/share/email-cli/accounts/testuser"));
    if (folders >= 0) {
        char n[16];
        snprintf(n, sizeof n, "%d", folders);
        setenv("MOCK_IMAP_FOLDERS", n, 1);
    } else {
        unsetenv("MOCK_IMAP_FOLDERS");
    }
}

/** Opens email-tui at 80x24 on the folder browser of the account; when
 *  @p want is not NULL waits for that text instead of the "Folders" title. */
static PtySession *fk_open_closed_wait(const char *want) {
    restart_mock();
    const char *args[] = {g_tui_bin, NULL};
    PtySession *s = pty_open(80, 24);
    if (!s) return NULL;
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    if (pty_wait_for(s, "Email Account", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, want ? want : "Folders \xe2\x80\x94", WAIT_MS) != 0) {
        pty_close(s);
        return NULL;
    }
    pty_settle(s, SETTLE_MS);
    return s;
}

/** The browser as the key study of the expanded tree needs it: every branch
 *  opened with the menu's "Open all folders".  A browser that waits for
 *  @p want (an empty list) is left as it is. */
static PtySession *fk_open_wait(const char *want) {
    PtySession *s = fk_open_closed_wait(want);
    if (s && !want) {
        menu_pick(s, 2);                           /* Open all folders (EMAIL-200) */
        pty_settle(s, SETTLE_MS);
    }
    return s;
}

static PtySession *fk_open(void) { return fk_open_wait(NULL); }
static PtySession *fk_open_closed(void) { return fk_open_closed_wait(NULL); }

/** Sends a key and waits for the position @p pos, e.g. "[3/16]". */
static int fk_key(PtySession *s, PtyKey key, const char *pos) {
    pty_send_key(s, key);
    return pty_wait_for(s, pos, WAIT_MS) == 0;
}

static int fk_str(PtySession *s, const char *k, const char *pos) {
    pty_send_str(s, k);
    return pty_wait_for(s, pos, WAIT_MS) == 0;
}

/** The text of the row under the cursor ('>' in column 0), or "". */
static const char *fk_cursor_row(PtySession *s, char *buf, size_t size) {
    buf[0] = '\0';
    for (int r = 3; r < 23; r++) {
        pty_row_text(s, r, buf, size);
        if (buf[0] == '>') return buf;
    }
    buf[0] = '\0';
    return buf;
}

/** Leaves the program by ctrl-c and closes the session. */
static void fk_end(PtySession *s) {
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

/* Every cursor key, tree view, 40 folders (48 rows): the page is the 19 rows
 * of the table; the section heading shows whenever Unread is the top row. */
static void test_fk_navigation(void) {
    fk_env(40);
    PtySession *s = fk_open();
    ASSERT(s != NULL, "folders nav: opens");
    if (!s) { fk_env(-1); return; }
    char row[200];
    ASSERT_SCREEN_CONTAINS(s, "[2/48]");
    ASSERT(strstr(fk_cursor_row(s, row, sizeof row), "Unread") != NULL,
           "folders nav: the cursor starts on Unread");
    ASSERT_ROW_CONTAINS(s, 4, "Tags / Flags");
    ASSERT(fk_str(s, "j", "[3/48]"), "folders nav: j steps down");
    ASSERT(fk_key(s, PTY_KEY_DOWN, "[4/48]"), "folders nav: Down steps down");
    ASSERT(fk_str(s, "k", "[3/48]"), "folders nav: k steps up");
    ASSERT(fk_key(s, PTY_KEY_UP, "[2/48]"), "folders nav: Up steps up");
    pty_send_key(s, PTY_KEY_UP);
    pty_send_str(s, "k");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[2/48]");               /* stops at the first row */
    ASSERT(fk_key(s, PTY_KEY_PGDN, "[21/48]"), "folders nav: PgDn goes one page (19 rows) down");
    ASSERT(strstr(fk_cursor_row(s, row, sizeof row), "F") != NULL && row[0] == '>',
           "folders nav: the cursor row is on the screen after PgDn");
    ASSERT(fk_key(s, PTY_KEY_PGDN, "[40/48]"), "folders nav: PgDn again");
    ASSERT(fk_key(s, PTY_KEY_PGUP, "[21/48]"), "folders nav: PgUp goes one page up");
    ASSERT(fk_key(s, PTY_KEY_PGUP, "[2/48]"), "folders nav: PgUp stops at the first row");
    ASSERT_ROW_CONTAINS(s, 4, "Tags / Flags");          /* EMAIL-196 */
    ASSERT(fk_key(s, PTY_KEY_END, "[48/48]"), "folders nav: End goes to the last row");
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_str(s, "j");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[48/48]");              /* stops at the last row */
    ASSERT(fk_key(s, PTY_KEY_HOME, "[2/48]"), "folders nav: Home goes to the first row");
    ASSERT_ROW_CONTAINS(s, 4, "Tags / Flags");
    ASSERT(fk_str(s, "G", "[48/48]"), "folders nav: G goes to the last row (EMAIL-198)");
    ASSERT(fk_str(s, "g", "[2/48]"), "folders nav: g goes to the first row (EMAIL-198)");
    ASSERT_ROW_CONTAINS(s, 4, "Tags / Flags");
    /* climbing back by single steps shows the heading when Unread is reached */
    ASSERT(fk_key(s, PTY_KEY_END, "[48/48]"), "folders nav: End again");
    ASSERT(fk_key(s, PTY_KEY_PGUP, "[29/48]"), "folders nav: PgUp from the end");
    for (int i = 0; i < 27; i++) pty_send_str(s, "k");
    ASSERT(pty_wait_for(s, "[2/48]", WAIT_MS) == 0, "folders nav: 27 x k reach Unread");
    ASSERT_ROW_CONTAINS(s, 4, "Tags / Flags");          /* EMAIL-196 */
    fk_end(s);
    fk_env(-1);
}

/* Column of @p what in row @p row, or -1. */
static int fk_col(PtySession *s, int row, const char *what) {
    char buf[300];
    pty_row_text(s, row, buf, sizeof buf);
    const char *p = strstr(buf, what);
    return p ? (int)(p - buf) : -1;
}

/* The tree of the folder browser (EMAIL-200, policies/tui-interface-model.md,
 * "Trees"): closed on opening with a "+ " marker; l and Right open, h and Left
 * close, a leaf goes to its parent; Enter does not toggle; the children are
 * indented; the open state survives a reload; the menu opens and closes all. */
static void test_fk_tree_open_close(void) {
    fk_env(-1);
    PtySession *s = fk_open_closed();
    ASSERT(s != NULL, "folders tree: opens");
    if (!s) return;
    ASSERT_SCREEN_CONTAINS(s, "[2/9]");                /* INBOX closed: 9 rows */
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    ASSERT(pty_wait_for(s, "[9/9]", WAIT_MS) == 0, "folders tree: Down x6 reaches INBOX");
    ASSERT_SCREEN_CONTAINS(s, "+ INBOX");
    ASSERT_SCREEN_NOT_CONTAINS(s, "AT&T");
    ASSERT_SCREEN_NOT_CONTAINS(s, "\xe2\x94\x9c");           /* no box-drawing glyphs */
    ASSERT_SCREEN_NOT_CONTAINS(s, "\xe2\x94\x94");
    pty_send_str(s, "h");                              /* closed top level: nothing */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[9/9]");
    ASSERT(fk_str(s, "l", "[9/16]"), "folders tree: l opens INBOX");
    ASSERT_SCREEN_CONTAINS(s, "- INBOX");
    ASSERT_SCREEN_CONTAINS(s, "AT&T");
    int r_in = find_row(s, "- INBOX");
    int r_at = find_row(s, "AT&T");
    ASSERT(r_in >= 0 && r_at == r_in + 1, "folders tree: the child is the row under INBOX");
    if (r_in >= 0 && r_at >= 0) {
        int c_in = fk_col(s, r_in, "INBOX"), c_at = fk_col(s, r_at, "AT&T");
        ASSERT(c_at == c_in + 2, "folders tree: a child is one level (two cells) under its parent, past a blank marker");
    }
    ASSERT(fk_key(s, PTY_KEY_RIGHT, "[10/16]"), "folders tree: Right on an open row goes to the first child");
    ASSERT(fk_str(s, "l", "[10/16]"), "folders tree: l on a leaf does nothing");
    ASSERT(fk_str(s, "h", "[9/16]"), "folders tree: h on a leaf goes to the parent");
    ASSERT(fk_key(s, PTY_KEY_LEFT, "[9/9]"), "folders tree: Left closes the open row");
    ASSERT_SCREEN_CONTAINS(s, "+ INBOX");
    ASSERT_SCREEN_NOT_CONTAINS(s, "AT&T");
    ASSERT(fk_key(s, PTY_KEY_RIGHT, "[9/16]"), "folders tree: Right opens");
    /* Enter opens the folder's messages and does not toggle; coming
     * back, the branch is still open */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in INBOX", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Folders \xe2\x80\x94", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "- INBOX");
    ASSERT_SCREEN_CONTAINS(s, "AT&T");
    /* the menu: close all, open all */
    menu_pick(s, 3);
    ASSERT_WAIT_FOR(s, "[9/9]", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "+ INBOX");
    menu_pick(s, 2);
    ASSERT_WAIT_FOR(s, "[9/16]", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "- INBOX");
    /* a reload keeps the open branches: create a folder, delete it again */
    menu_pick(s, 0);
    ASSERT_WAIT_FOR(s, "New folder name:", WAIT_MS);
    pty_send_str(s, "INBOX.Projects");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Folder INBOX.Projects created", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "- INBOX");
    ASSERT_SCREEN_CONTAINS(s, "Projects");
    for (int i = 0; i < 12 && !pty_row_contains(s, find_row(s, "Projects"), ">"); i++) {
        pty_send_str(s, "j");
        pty_settle(s, 200);
    }
    menu_pick(s, 1);
    ASSERT_WAIT_FOR(s, "Delete the folder INBOX.Projects on the server?", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Folder INBOX.Projects deleted", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "- INBOX");
    /* the menu names the new actions, the help the new keys */
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Open all folders", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Close all folders");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Folder browser shortcuts", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Open branch");
    ASSERT_SCREEN_CONTAINS(s, "Close - tree: close");
    pty_send_key(s, PTY_KEY_ESC);
    fk_end(s);
    fk_env(-1);
}

/* Enter opens the row: a folder, a virtual list; the way back keeps the row. */
static void test_fk_enter(void) {
    fk_env(-1);
    PtySession *s = fk_open();
    ASSERT(s != NULL, "folders enter: opens");
    if (!s) return;
    ASSERT_SCREEN_CONTAINS(s, "[2/16]");
    pty_send_key(s, PTY_KEY_ENTER);                    /* Unread */
    ASSERT_WAIT_FOR(s, "message(s) in Unread", WAIT_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "[2/16]", WAIT_MS);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);   /* the heading is skipped */
    ASSERT(pty_wait_for(s, "[9/16]", WAIT_MS) == 0, "folders enter: Down x6 reaches INBOX");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in INBOX", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "[9/16]", WAIT_MS);              /* the same row */
    ASSERT(fk_key(s, PTY_KEY_DOWN, "[10/16]"), "folders enter: Down to a subfolder");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in INBOX.AT&T", WAIT_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "[10/16]", WAIT_MS);
    fk_end(s);
}

/* The flat view: one level at a time; t toggles; Esc, q, Backspace and h, Left
 * go up; Enter, l and Right go in (EMAIL-197). */
static void test_fk_flat(void) {
    fk_env(-1);
    PtySession *s = fk_open();
    ASSERT(s != NULL, "folders flat: opens");
    if (!s) return;
    ASSERT_ROW_CONTAINS(s, 23, "t flat");
    ASSERT(fk_str(s, "t", "[9/9]"), "folders flat: t switches to the flat view");
    ASSERT_ROW_CONTAINS(s, 23, "t tree");
    ASSERT_ROW_CONTAINS(s, 23, "Esc/q/BS back");
    ASSERT_SCREEN_CONTAINS(s, "INBOX/");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Esc/q/BS up", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "\xe2\x80\xba INBOX/");
    ASSERT_SCREEN_CONTAINS(s, "[9/16]");                /* the folder itself first */
    ASSERT(fk_str(s, "G", "[16/16]"), "folders flat: G goes to the last row");
    ASSERT(fk_str(s, "g", "[2/16]"), "folders flat: g goes to the first row");
    pty_send_str(s, "h");                              /* up, as Esc */
    ASSERT_WAIT_FOR(s, "Esc/q/BS back", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "\xe2\x80\xba INBOX/");
    pty_send_str(s, "l");                              /* in, as Enter */
    ASSERT_WAIT_FOR(s, "Esc/q/BS up", WAIT_MS);
    pty_send_key(s, PTY_KEY_LEFT);
    ASSERT_WAIT_FOR(s, "Esc/q/BS back", WAIT_MS);
    pty_send_key(s, PTY_KEY_RIGHT);
    ASSERT_WAIT_FOR(s, "Esc/q/BS up", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Esc/q/BS back", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Esc/q/BS up", WAIT_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Esc/q/BS back", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Esc/q/BS up", WAIT_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Esc/q/BS back", WAIT_MS);
    /* a folder inside the level opens its messages; the level is kept */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Esc/q/BS up", WAIT_MS);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in INBOX.AT&T", WAIT_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Esc/q/BS up", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "\xe2\x80\xba INBOX/");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Esc/q/BS back", WAIT_MS);
    ASSERT(fk_str(s, "t", "[9/16]"), "folders flat: t switches back to the tree");
    ASSERT_ROW_CONTAINS(s, 23, "t flat");
    /* h at the root of the flat view goes back to the accounts */
    pty_send_str(s, "t");
    ASSERT_WAIT_FOR(s, "t tree", WAIT_MS);
    pty_send_str(s, "h");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    fk_end(s);
}

/* Esc, q and Backspace leave the tree for the accounts; Esc there quits. */
static void test_fk_back_keys(void) {
    fk_env(-1);
    const struct { PtyKey key; const char *str; const char *name; } k[] = {
        { PTY_KEY_ESC, NULL, "Esc" }, { 0, "q", "q" }, { PTY_KEY_BACK, NULL, "Backspace" },
    };
    for (int i = 0; i < 3; i++) {
        PtySession *s = fk_open();
        ASSERT(s != NULL, "folders back: opens");
        if (!s) return;
        if (k[i].str) pty_send_str(s, k[i].str); else pty_send_key(s, k[i].key);
        ASSERT(pty_wait_for(s, "Email Accounts", WAIT_MS) == 0, k[i].name);
        ASSERT_SCREEN_NOT_CONTAINS(s, "Connection failed");
        pty_send_key(s, PTY_KEY_ESC);
        ASSERT(pty_wait_exit(s, 5000) == 0, "folders back: Esc on the accounts quits");
        pty_close(s);
    }
}

/* ctrl-c quits at once from the tree, the flat view and the help (EMAIL-190). */
static void test_fk_ctrl_c(void) {
    fk_env(-1);
    PtySession *s = fk_open();
    ASSERT(s != NULL, "folders ctrl-c: opens");
    if (!s) return;
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, 5000) == 0, "folders ctrl-c: the tree quits at once, status 0");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Connection failed");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Press any key");
    pty_close(s);
    s = fk_open();
    ASSERT(s != NULL, "folders ctrl-c: opens again");
    if (!s) { fk_env(-1); return; }
    pty_send_str(s, "t");
    ASSERT_WAIT_FOR(s, "t tree", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, 5000) == 0, "folders ctrl-c: the flat view quits at once");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Connection failed");
    pty_close(s);
    fk_env(-1);
}

/* '/': the prompt, its keys, its hints; Enter searches, Esc and an empty
 * Enter cancel, ctrl-c quits (EMAIL-191, EMAIL-192, EMAIL-195). */
static void test_fk_search(void) {
    fk_env(-1);
    PtySession *s = fk_open();
    ASSERT(s != NULL, "folders search: opens");
    if (!s) return;
    pty_send_str(s, "/");
    ASSERT_WAIT_FOR(s, "Search [Subject]: _", WAIT_MS);
    ASSERT_ROW_CONTAINS(s, 23, "Enter search");
    ASSERT_ROW_CONTAINS(s, 23, "Tab scope");
    ASSERT_ROW_CONTAINS(s, 23, "Esc cancel");
    ASSERT(!pty_row_contains(s, 23, "Esc/q/BS"), "folders search: not the browser's hints");
    pty_send_str(s, "qjtx");                           /* letters, not keys */
    ASSERT_WAIT_FOR(s, "Search [Subject]: qjtx_", WAIT_MS);
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "Search [From]: qjtx_", WAIT_MS);
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "Search [To]: qjtx_", WAIT_MS);
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "Search [Body]: qjtx_", WAIT_MS);
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "Search [Subject]: qjtx_", WAIT_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Search [Subject]: qjt_", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);                      /* cancels, stays */
    ASSERT_WAIT_FOR(s, "Esc/q/BS back", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Search [");
    ASSERT_SCREEN_CONTAINS(s, "[2/16]");
    pty_send_str(s, "/");
    ASSERT_WAIT_FOR(s, "Search [Subject]: _", WAIT_MS);
    pty_send_key(s, PTY_KEY_BACK);                     /* nothing to delete */
    pty_send_key(s, PTY_KEY_ENTER);                    /* empty: cancels */
    ASSERT_WAIT_FOR(s, "Esc/q/BS back", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Search [");
    /* a search result goes back to the row the search started from */
    for (int i = 0; i < 3; i++) pty_send_key(s, PTY_KEY_DOWN);
    ASSERT(pty_wait_for(s, "[5/16]", WAIT_MS) == 0, "folders search: Down x3");
    pty_send_str(s, "/");
    ASSERT_WAIT_FOR(s, "Search [Subject]: _", WAIT_MS);
    pty_send_str(s, "Test");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in Search: \"Test\"", WAIT_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "Folders \xe2\x80\x94", WAIT_MS);
    ASSERT(pty_wait_for(s, "[5/16]", WAIT_MS) == 0, "folders search: back on the row of the search");
    /* ctrl-c in the prompt quits */
    pty_send_str(s, "/");
    ASSERT_WAIT_FOR(s, "Search [Subject]: _", WAIT_MS);
    pty_send_str(s, "abc");
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, 5000) == 0, "folders search: ctrl-c in the prompt quits at once");
    pty_close(s);
    fk_env(-1);
}

/* c: the dialog, and the browser comes back on the same row, also when the
 * browser was reached from a message list (EMAIL-194). */
static void test_fk_compose_keeps_row(void) {
    fk_env(-1);
    PtySession *s = fk_open();
    ASSERT(s != NULL, "folders compose: opens");
    if (!s) return;
    for (int i = 0; i < 10; i++) pty_send_key(s, PTY_KEY_DOWN);
    ASSERT(pty_wait_for(s, "[13/16]", WAIT_MS) == 0, "folders compose: Down x10");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "[13/16]", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Folders \xe2\x80\x94");
    /* in the flat view the level is kept too */
    pty_send_str(s, "t");
    ASSERT_WAIT_FOR(s, "t tree", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Esc/q/BS up", WAIT_MS);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_DOWN);
    ASSERT(pty_wait_for(s, "[11/16]", WAIT_MS) == 0, "folders compose: flat, Down x2");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "[11/16]", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "\xe2\x80\xba INBOX/");
    fk_end(s);
    /* from a list: Backspace to the browser, c, Esc: the browser, not the list */
    fk_env(-1);
    s = fk_open();
    ASSERT(s != NULL, "folders compose: opens again");
    if (!s) return;
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    ASSERT(pty_wait_for(s, "[9/16]", WAIT_MS) == 0, "folders compose: on INBOX");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in INBOX", WAIT_MS);
    pty_send_key(s, PTY_KEY_BACK);
    ASSERT_WAIT_FOR(s, "[9/16]", WAIT_MS);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "[10/16]", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Folders \xe2\x80\x94");
    ASSERT_SCREEN_NOT_CONTAINS(s, "message(s) in");
    fk_end(s);
    fk_env(-1);
}

/* A list that was read and is empty is an empty list, not a failed
 * connection; the menu creates the first folder (EMAIL-193). */
static void test_fk_zero_folders(void) {
    fk_env(0);
    PtySession *s = fk_open_wait("No folders in this account");
    ASSERT(s != NULL, "folders zero: the browser opens and says so");
    if (!s) { fk_env(-1); return; }
    ASSERT_SCREEN_CONTAINS(s, "Create one in the : menu");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Connection failed");
    ASSERT_SCREEN_CONTAINS(s, "Folders \xe2\x80\x94 testuser  (8)");
    ASSERT_ROW_CONTAINS(s, 23, "[2/8]");
    pty_send_str(s, "jkgG");
    pty_send_key(s, PTY_KEY_PGDN);
    pty_send_key(s, PTY_KEY_PGUP);
    pty_send_key(s, PTY_KEY_END);
    pty_send_key(s, PTY_KEY_HOME);
    pty_send_str(s, "t");
    ASSERT_WAIT_FOR(s, "t tree", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "No folders in this account");
    pty_send_key(s, PTY_KEY_ENTER);                    /* Unread */
    ASSERT_WAIT_FOR(s, "message(s) in Unread", WAIT_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "No folders in this account", WAIT_MS);
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Create a folder", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);                      /* closes the menu */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Create a folder");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_close(s);
    fk_env(-1);
}

/* One folder: every cursor key stays on it, t and Enter work. */
static void test_fk_one_folder(void) {
    fk_env(1);
    PtySession *s = fk_open();
    ASSERT(s != NULL, "folders one: opens");
    if (!s) { fk_env(-1); return; }
    ASSERT_SCREEN_CONTAINS(s, "[2/9]");
    ASSERT(fk_str(s, "G", "[9/9]"), "folders one: G reaches the only folder");
    ASSERT(fk_key(s, PTY_KEY_END, "[9/9]"), "folders one: End");
    pty_send_str(s, "j");
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_PGDN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[9/9]");
    ASSERT(fk_key(s, PTY_KEY_PGUP, "[2/9]"), "folders one: PgUp goes to the top");
    ASSERT(fk_key(s, PTY_KEY_HOME, "[2/9]"), "folders one: Home");
    ASSERT(fk_str(s, "t", "[9/9]"), "folders one: t switches to the flat view");
    pty_send_key(s, PTY_KEY_ENTER);                    /* no subfolder: opens it */
    ASSERT_WAIT_FOR(s, "message(s) in INBOX", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "t tree", WAIT_MS);
    fk_end(s);
    fk_env(-1);
}

/* The help: every key of the view, readable at 80 columns; Esc, q and
 * Backspace close it; another key changes nothing (EMAIL-199). */
static void test_fk_help(void) {
    fk_env(-1);
    PtySession *s = fk_open();
    ASSERT(s != NULL, "folders help: opens");
    if (!s) return;
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Folder browser shortcuts", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "also g");
    ASSERT_SCREEN_CONTAINS(s, "also G");
    ASSERT_SCREEN_CONTAINS(s, "tree: open; flat view: as Enter");
    ASSERT_SCREEN_CONTAINS(s, "flat view: one level up");
    ASSERT_SCREEN_CONTAINS(s, "Search");
    ASSERT_SCREEN_CONTAINS(s, "Compose");
    pty_send_key(s, PTY_KEY_END);                      /* the help scrolls: the tail */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Tree / flat");
    ASSERT_SCREEN_CONTAINS(s, "switch between the folder tree");
    pty_send_str(s, "xtn");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Folder browser shortcuts");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Folder browser shortcuts");
    ASSERT_ROW_CONTAINS(s, 23, "t flat");                /* t did not toggle */
    const struct { PtyKey key; const char *str; } close[] = {
        { 0, "q" }, { PTY_KEY_BACK, NULL },
    };
    for (int i = 0; i < 2; i++) {
        pty_send_str(s, "?");
        ASSERT_WAIT_FOR(s, "Folder browser shortcuts", WAIT_MS);
        if (close[i].str) pty_send_str(s, close[i].str); else pty_send_key(s, close[i].key);
        pty_settle(s, SETTLE_MS);
        ASSERT_SCREEN_NOT_CONTAINS(s, "Folder browser shortcuts");
        ASSERT_SCREEN_CONTAINS(s, "Folders \xe2\x80\x94");
    }
    fk_end(s);
}

/* The ':' menu lists the view switch with its key, the creation and the
 * deletion; Esc closes it. */
static void test_fk_menu(void) {
    fk_env(-1);
    PtySession *s = fk_open();
    ASSERT(s != NULL, "folders menu: opens");
    if (!s) return;
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Tree / flat");
    ASSERT_SCREEN_CONTAINS(s, "Create a folder");
    ASSERT_SCREEN_CONTAINS(s, "Delete the folder");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Actions");
    ASSERT_SCREEN_CONTAINS(s, "Folders \xe2\x80\x94");
    fk_end(s);
}

/* Keys that mean nothing here change nothing and show nothing. */
static void test_fk_unbound_keys(void) {
    fk_env(-1);
    PtySession *s = fk_open();
    ASSERT(s != NULL, "folders unbound: opens");
    if (!s) return;
    char before[4096], after[4096];
    pty_screen_text(s, before, sizeof before);
    const char *dead[] = { "n", "f", "s", "m", "D", "u", "v", "a", "A", "d", "r", "x", "Z",
                           "1", "2", " ", "*", "+", "-", "!", "U", "N", NULL };
    for (int i = 0; dead[i]; i++) pty_send_str(s, dead[i]);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_DELETE);
    pty_send_key(s, PTY_KEY_F1);
    pty_settle(s, SETTLE_MS);
    pty_screen_text(s, after, sizeof after);
    ASSERT(strcmp(before, after) == 0, "folders unbound: the screen is unchanged");
    ASSERT_SCREEN_CONTAINS(s, "[2/16]");
    fk_end(s);
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
    /* Esc goes back to the list (and on up the stack) */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT(pty_wait_exit(s, 5000) == 0, "tui show ESC x4: program exits with status 0");
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
        "EMAIL_HOST=imaps://localhost:%d\n"
        "EMAIL_USER=%s\n"
        "EMAIL_PASS=pass2\n"
        "EMAIL_FOLDER=INBOX\n"
        "SSL_NO_VERIFY=1\n", g_imap_port, name);  /* TLS with self-signed cert */
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
    /* US-21: Esc on the accounts screen (the top level) exits the TUI with
     * status 0; q does not (it only goes back, and there is nowhere to go). */
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "accounts ESC quit: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "q");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT(pty_wait_exit(s, 5000) == 0, "accounts ESC: program exits with status 0");
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
    /* The ">" marker (column 0) stands on the second row of the list, the
     * account that was opened (the accounts sort domain-first, so that is
     * testuser, below second@example.com). */
    {
        int row = find_row(s, "testuser");
        ASSERT(row >= 0, "cursor restore: opened account row found");
        if (row >= 0)
            ASSERT(strcmp(pty_cell_text(s, row, 0), ">") == 0,
                   "cursor restore: > marks the account that was opened");
        int first = find_row(s, "second@example.com");
        if (first >= 0)
            ASSERT(strcmp(pty_cell_text(s, first, 0), ">") != 0,
                   "cursor restore: the first account is not marked");
    }
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

static void test_tui_accounts_colon_on_imap_account(void) {
    /* EMAIL-130: the ':' menu of the accounts screen holds the renewal of a
     * Gmail authorization; an IMAP account has none, and says so. */
    restart_mock();
    const char *args[] = {g_tui_bin, NULL};
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "accounts colon: pty_open");
    if (pty_run(s, args) != 0) { pty_close(s); return; }
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "actions");        /* no hint on an IMAP account */
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "No actions for an IMAP account", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Renew the authorization");
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
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
    pty_send_str(s, "h");                    /* h is not a help key */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Accounts shortcuts");
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Accounts shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Open selected account");
    /* a key that is not a closing key changes nothing; Esc closes it
     * (policies/tui-interface-model.md, "The help screen") */
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Accounts shortcuts");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Accounts shortcuts");
    /* should be back on accounts screen */
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_list_help_panel(void) {
    /* US-22 AC7: 'h' in message list shows help overlay.
     * Use a tall terminal so that every shortcut row fits. */
    restart_mock();
    PtySession *s = tui_open_to_list_size(COLS, 50);
    ASSERT(s != NULL, "list help: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "z");                    /* ? is the only help key */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Message list shortcuts");
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Message list shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Compose");
    ASSERT_SCREEN_CONTAINS(s, "Delete");
    ASSERT_SCREEN_CONTAINS(s, "Actions");
    pty_send_key(s, PTY_KEY_ENTER);          /* not a closing key */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Message list shortcuts");
    pty_send_str(s, "q");                    /* q closes it */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Message list shortcuts");
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
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Message reader shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Reply");
    pty_send_key(s, PTY_KEY_BACK);           /* Backspace closes it */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Message reader shortcuts");
    /* back in reader */
    ASSERT_SCREEN_CONTAINS(s, "r reply");
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
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Folder browser shortcuts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_END);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Tree / flat");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Folder browser shortcuts");
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
    pty_send_key(s, PTY_KEY_ESC);   /* Esc dismisses */
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
        "EMAIL_HOST=imap://localhost:%d\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n", g_imap_port);
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
        "EMAIL_HOST=imaps://localhost:%d\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SMTP_HOST=smtp://localhost:%d\n"
        "SMTP_PORT=%d\n"
        "SMTP_USER=testuser\n"
        "SMTP_PASS=testpass\n", g_imap_port, g_dead_port, g_dead_port);
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
    /* Enter confirms the dialog from any field; with an empty To it only
     * raises the inline guard, so fill To: first (Tab moves between fields). */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "To: is empty", WAIT_MS);
    pty_send_str(s, "someone@example.com");
    pty_send_key(s, PTY_KEY_ENTER);  /* confirm: no subject → y/n prompt */
    ASSERT_WAIT_FOR(s, "Subject is empty", WAIT_MS);
    pty_send_str(s, "y");
    /* US-85: the editor runs, then the review screen appears; 'q' discards. */
    ASSERT(pty_editor_type_and_leave(s, "Test body") == 0, "body editor opens");
    ASSERT_WAIT_FOR(s, "Review: Message", WAIT_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Cancelled. Draft discarded.", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[Press any key to return to inbox]", WAIT_MS);
    pty_send_str(s, " ");   /* any key — return to list */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_tui_list_reply_key(void) {
    /* US-20: 'r' in TUI list launches reply: dialog, body editor holding the
     * quote (no CR in it), review; discarding returns to the list. */
    restart_mock();
    char draft_path[600];
    snprintf(draft_path, sizeof(draft_path),
             "%s/.local/share/email-cli/compose-draft", g_test_home);
    unlink(draft_path);
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "tui list reply key: opens");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);    /* compose dialog title */
    pty_send_key(s, PTY_KEY_ENTER);  /* Cc → Bcc (To: pre-filled, starts at Cc) */
    pty_send_key(s, PTY_KEY_ENTER);  /* Bcc → Subject */
    pty_send_key(s, PTY_KEY_ENTER);  /* Subject → submit */
    /* US-85: the editor blanks To:, then the review screen appears; 'q' discards. */
    ASSERT(pty_editor_type_and_leave(s, "Test body") == 0, "body editor opens");
    ASSERT_WAIT_FOR(s, "Review: Message", WAIT_MS * 2);
    {
        /* Leaving the editor saved the draft: it holds the quote, no CR. */
        FILE *fp = fopen(draft_path, "r");
        ASSERT(fp != NULL, "reply: the editor saved a draft");
        if (fp) {
            char txt[8192];
            size_t n = fread(txt, 1, sizeof(txt) - 1, fp);
            fclose(fp);
            txt[n] = '\0';
            ASSERT(strstr(txt, "\n> ") != NULL, "reply: the draft holds the quote");
            ASSERT(strchr(txt, '\r') == NULL, "reply: no CR in the quoted body");
        }
    }
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Cancelled. Draft discarded.", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[Press any key to return to inbox]", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    ASSERT(access(draft_path, F_OK) != 0, "reply: discarding removed the draft");
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
    pty_send_str(s, "s");            /* 's' does nothing any more */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "syncing");
    menu_pick(s, MENU_IMAP_SYNC);    /* start background sync: asks first */
    ASSERT_WAIT_FOR(s, "Start a background sync", WAIT_MS);
    pty_send_str(s, "y");
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
    /* The menu arrives in several writes: wait for its last line (the
     * prompt), not for the first, before looking at the others. */
    ASSERT_WAIT_FOR(s, "Choice [1]", WAIT_MS);
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

/* EMAIL-84: any answer but y keeps the account, and says so. */
static void test_tui_accounts_delete_declined(void) {
    write_second_account("keepme@example.com");
    restart_mock();
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "accounts delete declined: opens");
    ASSERT_WAIT_FOR(s, "Email Accounts", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "Delete account keepme@example.com? (y/N)", WAIT_MS);
    pty_send_str(s, "n");
    ASSERT_WAIT_FOR(s, "Account keepme@example.com kept.", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "keepme@example.com");

    /* Enter and Esc decline as well */
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "Delete account keepme@example.com? (y/N)", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Account keepme@example.com kept.", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "keepme@example.com");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    /* the account is still on disk: remove the fixture */
    char path[500];
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/keepme@example.com/config.ini", g_test_home);
    unlink(path);
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/keepme@example.com", g_test_home);
    rmdir(path);
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
    ASSERT_WAIT_FOR(s, "Delete account todelete@example.com? (y/N)", WAIT_MS);
    /* nothing is deleted before the answer */
    ASSERT_SCREEN_CONTAINS(s, "todelete@example.com");
    pty_send_str(s, "y");
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

    /* Press '*' to toggle Flagged on the first message */
    pty_send_str(s, "*");
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

/* EMAIL-160: with no connection, 'D' must queue the move to the Trash for
 * the next sync (and say so), not drop the message locally and lose it. */
static void test_delete_offline_queues_move(void) {
    restart_mock();
    { const char *a[] = {NULL};
      PtySession *s = sync_run(a);
      ASSERT(s != NULL, "offline D: sync opens");
      ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
      pty_close(s); }

    char pm_path[512];
    snprintf(pm_path, sizeof(pm_path),
             "%s/.local/share/email-cli/accounts/testuser/pending_moves/INBOX.tsv",
             g_test_home);
    if (unlink(pm_path) != 0 && errno != ENOENT)
        perror("offline D: unlink stale pending_moves");

    write_config_with_interval(5);
    stop_mock_server();

    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "offline D: tui opens offline");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    pty_send_str(s, "y");
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_screen_contains(s, "Moved to Trash"),
           "offline D: feedback says the message was moved to Trash");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    ASSERT(access(pm_path, F_OK) == 0,
           "offline D: pending_moves/INBOX.tsv written for the next sync");

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

/* Helper: move the folder-browser cursor onto INBOX, whatever the saved
 * folder preference is.  email-tui pre-selects the __unread__ virtual label
 * when no preference exists, so tests must not assume INBOX is under the
 * cursor.  HOME lands on the first selectable row (Unread); six DOWN presses
 * step over the remaining virtual rows and the "Folders" header (VPREFIX=8:
 * one header + six virtual rows + one header). */
static void folders_goto_inbox(PtySession *s) {
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
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

    /* Press '+' to mark as read */
    pty_send_str(s, "+");
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

    /* Navigate to INBOX and open the message list */
    folders_goto_inbox(s);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Single Unread");

    /* Mark it as read */
    pty_send_str(s, "+");
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

    /* Open INBOX (cursor may start on a virtual row) */
    folders_goto_inbox(s);
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

    /* Open INBOX (cursor may start on a virtual row) */
    folders_goto_inbox(s);
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

    /* Open INBOX (cursor may start on a virtual row) */
    folders_goto_inbox(s);
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

    /* Open INBOX (cursor may start on a virtual row) */
    folders_goto_inbox(s);
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
        "\n"
        /* No-condition rule: when_from_flat returns NULL → rule->when = NULL
         * → mail_rule_matches takes the legacy flat-field path → glob_match covered. */
        "[rule \"AlwaysTag\"]\n"
        "then-add-label    = AutoTagged\n"
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
    ASSERT_SCREEN_CONTAINS(s, "a add");
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
    pty_settle(s, SETTLE_MS);   /* the status line is drawn after the frame */
    ASSERT_SCREEN_CONTAINS(s, "a add");
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
    pty_send_key(s, PTY_KEY_TAB);                   /* confirm when list, proceed to step 2 */
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
    pty_send_key(s, PTY_KEY_TAB);
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
    ASSERT_SCREEN_CONTAINS(s, "e edit");
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

static void test_tui_rules_detail_delete_cancel_stays(void) {
    /* EMAIL-165: 'd' then 'n' or an arrow key cancels the delete and stays in
     * the detail view; the answer is read through the key decoder. */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rule detail cancel: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "(y/N)", RULES_WAIT_MS);
    pty_send_str(s, "n");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Rule:");
    ASSERT(pty_screen_contains(s, "(y/N)") == 0,
           "rule detail cancel: prompt gone after 'n'");
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "(y/N)", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Rule:");
    ASSERT(pty_screen_contains(s, "(y/N)") == 0,
           "rule detail cancel: prompt gone after an arrow key");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "SpamFilter");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_list_delete_arrow_cancels(void) {
    /* EMAIL-165: in the rules list 'd' then an arrow key cancels the delete. */
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules list cancel: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "(y/N)", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "SpamFilter");
    ASSERT(pty_screen_contains(s, "(y/N)") == 0,
           "rules list cancel: prompt gone after an arrow key");
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
    pty_send_key(s, PTY_KEY_TAB);                                  /* confirm conditions, advance to step 2 */
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


/* ── EMAIL-115: the rule form, all the way to the saved file ─────────── */

/** 1 when rules.ini of the test account contains @p text. */
static int rules_ini_contains(const char *text) {
    char path[512], buf[8192];
    snprintf(path, sizeof(path), "%s/.config/email-cli/accounts/testuser/rules.ini", g_test_home);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return strstr(buf, text) != NULL;
}

/** Answer the nine fields of step 2 with Enter, typing @p extra into the
 *  second add-label field (the third one) first when it is not NULL. */
static void rule_form_fields(PtySession *s, const char *extra) {
    ASSERT_WAIT_FOR(s, "step 2/2: actions", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    for (int i = 0; i < 9; i++) {
        if (i == 2 && extra) pty_send_str(s, extra);
        pty_send_key(s, PTY_KEY_ENTER);
        pty_settle(s, 100);
    }
}

static void test_tui_rules_edit_form_saves(void) {
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rule form saves: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "e");
    ASSERT_WAIT_FOR(s, "conditions (step 1/2)", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "from:*@spam.example.com");

    pty_send_str(s, "a");                       /* add a condition */
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "subject:*offer*");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "subject:*offer*");
    pty_send_str(s, "k");                       /* up to the first one ... */
    pty_send_str(s, "k");                       /* ... and no further */
    pty_send_str(s, "j");                       /* down again */
    pty_send_str(s, "j");                       /* ... and no further */
    pty_send_str(s, "e");                       /* edit the second one */
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "!");                       /* an invalid condition is dropped */
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "subject:*offer*");
    pty_send_key(s, PTY_KEY_ENTER);             /* Enter edits, too */
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "x");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "?");                       /* the help popup */
    ASSERT_WAIT_FOR(s, "Conditions shortcuts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "d");                       /* delete the one under the cursor */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "subject:*offer*");
    pty_send_key(s, PTY_KEY_TAB);                       /* conditions done */

    rule_form_fields(s, "ExtraLabel");
    ASSERT_WAIT_FOR(s, "Save? (y/N)", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT(rules_ini_contains("ExtraLabel"), "rule form saves: the new label is in rules.ini");
    ASSERT(rules_ini_contains("SpamFilter"), "rule form saves: the rule keeps its name");
    ASSERT(rules_ini_contains("*@spam.example.com"), "rule form saves: and its condition");

    pty_send_key(s, PTY_KEY_ESC);
    pty_send_key(s, PTY_KEY_ESC);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_add_form_saves_new_rule(void) {
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "add rule: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Add new rule for", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "press 'a' to add a condition");
    pty_send_key(s, PTY_KEY_ENTER);             /* Enter on an empty list adds, too */
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "from:*@brand-new.example*");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "step 2/2: actions", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "BrandNew");                /* the name */
    pty_send_key(s, PTY_KEY_ENTER);
    pty_send_str(s, "Fresh");                   /* add-label [1] */
    pty_send_key(s, PTY_KEY_ENTER);
    for (int i = 0; i < 7; i++) { pty_send_key(s, PTY_KEY_ENTER); pty_settle(s, 100); }
    ASSERT_WAIT_FOR(s, "Save? (y/N)", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "(4 rules)", WAIT_MS);     /* saved, and the list shows it */
    ASSERT(rules_ini_contains("BrandNew"), "add rule: the rule is in rules.ini");
    ASSERT(rules_ini_contains("brand-new.example"), "add rule: with its condition");
    ASSERT(rules_ini_contains("SpamFilter"), "add rule: the others are kept");
    pty_send_key(s, PTY_KEY_ESC);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

/* EMAIL-126: the rules editor previews (dry run) and applies the rules. */
static void test_tui_rules_preview_and_apply(void) {
    remove_rules_ini();
    write_rules_ini();
    {   /* one rule that matches every cached message */
        char path[512];
        snprintf(path, sizeof(path),
                 "%s/.config/email-cli/accounts/testuser/rules.ini", g_test_home);
        FILE *fp = fopen(path, "a");
        ASSERT(fp != NULL, "rules apply: rules.ini is writable");
        if (fp) {
            fprintf(fp, "\n[rule \"Everything\"]\nwhen = from:*\n"
                        "then-add-label = Tagged\n");
            fclose(fp);
        }
    }
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "rules apply: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "p");                       /* preview: a dry run */
    ASSERT_WAIT_FOR(s, "Rules dry-run:", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "preview (dry run, nothing changed)");
    ASSERT_SCREEN_CONTAINS(s, "[dry-run]");     /* the matches are shown */
    ASSERT_SCREEN_CONTAINS(s, "+Tagged");
    pty_send_str(s, " ");                       /* any key goes back */
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);

    pty_send_str(s, "A");                       /* apply: asks first */
    ASSERT_WAIT_FOR(s, "Apply the rules", WAIT_MS);
    pty_send_str(s, "n");                       /* declined: nothing runs */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Rules applied:");
    ASSERT_SCREEN_CONTAINS(s, "SpamFilter");

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Apply the rules", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Rules applied:", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "[dry-run]");
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

static void test_tui_rules_add_form_needs_a_name(void) {
    remove_rules_ini();
    write_rules_ini();
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "nameless rule: reaches message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Add new rule for", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_TAB);                       /* no conditions, on to the actions */
    ASSERT_WAIT_FOR(s, "step 2/2: actions", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    for (int i = 0; i < 9; i++) { pty_send_key(s, PTY_KEY_ENTER); pty_settle(s, 100); }
    ASSERT_WAIT_FOR(s, "Rule name is required", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "SpamFilter", RULES_WAIT_MS);
    ASSERT(!rules_ini_contains("[rule \"\"]"), "nameless rule: nothing was saved");
    pty_send_key(s, PTY_KEY_ESC);
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
    pty_send_key(s, PTY_KEY_ESC);                          /* cancel edit (EMAIL-166: asks) */
    ASSERT_WAIT_FOR(s, "Discard the rule? (y/N)", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

/* EMAIL-2 / EMAIL-3: function keys and modified cursor keys are no keys at
 * all.  F1 (ESC O P) used to act as Esc ("go back") and leave a typed 'P'. */
static void test_tui_function_and_modified_keys_do_nothing(void) {
    restart_mock();
    PtySession *s = tui_open_to_list();
    ASSERT(s != NULL, "unknown keys: reaches the message list");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "\033OP");          /* F1 */
    pty_send_str(s, "\033[1;5A");       /* Ctrl+Up */
    pty_send_str(s, "\033[15~");        /* F5 */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "message(s) in");   /* still in the list */
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
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
    ASSERT_WAIT_FOR(s, "Discard the rule? (y/N)", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Rule:", RULES_WAIT_MS);            /* still in detail view */
    pty_settle(s, SETTLE_MS);                              /* let the redraw finish */
    ASSERT_SCREEN_CONTAINS(s, "SpamFilter");               /* rule unchanged */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Rules for", RULES_WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "SpamFilter");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "message(s) in", RULES_WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    remove_rules_ini();
}

#undef RULES_WAIT_MS

/* ── Main ────────────────────────────────────────────────────────────── */

#define RUN_TEST(fn) do { if (!test_selected(#fn)) break; printf("  %s...\n", #fn); fn(); } while(0)


/* ══════════════════════════════════════════════════════════════════════
 * EMAIL-115: the accounts screen and what happens when an account cannot be
 * opened.  Each test runs with ONE other account in place of the usual one.
 * ══════════════════════════════════════════════════════════════════════ */

static char g_acc_saved_home[512];

/** Make a fresh HOME whose only account is @p ini (a config.ini). */
static void only_account(const char *name, const char *ini) {
    static int n;
    char home[512], p[800], rel[32];
    snprintf(rel, sizeof(rel), "acchome-%d", ++n);
    snprintf(home, sizeof(home), "%s", test_path(rel));
    const char *old = getenv("HOME");
    snprintf(g_acc_saved_home, sizeof(g_acc_saved_home), "%s", old ? old : "");
    mkdir(home, 0700);
    snprintf(p, sizeof(p), "%s/.config", home);                       mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/.config/email-cli", home);             mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/.config/email-cli/accounts", home);    mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/.config/email-cli/accounts/%s", home, name); mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/.config/email-cli/accounts/%s/config.ini", home, name);
    FILE *f = fopen(p, "w");
    if (f) { fputs(ini, f); fclose(f); }
    chmod(p, 0600);
    setenv("HOME", home, 1);
}

/** Back to the usual HOME. */
static void restore_accounts(const char *name) {
    (void)name;
    setenv("HOME", g_acc_saved_home, 1);
}

static void test_account_unreachable_returns_to_accounts(void) {
    char ini[512];
    snprintf(ini, sizeof(ini),
             "EMAIL_HOST=imaps://localhost:%d\nEMAIL_USER=down@example.com\n"
             "EMAIL_PASS=x\nEMAIL_FOLDER=INBOX\nSSL_NO_VERIFY=1\n"
             "SMTP_HOST=smtps://localhost:%d\n", g_dead_port, g_dead_port);
    only_account("down", ini);
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "unreachable account: started");
    ASSERT_WAIT_FOR(s, "down@example.com", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Press any key to return to accounts", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "down@example.com", WAIT_MS);     /* the accounts screen again */
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    restore_accounts("down");
}

static void test_account_imap_gmail_host_is_explained(void) {
    only_account("gi", "EMAIL_HOST=imaps://mail.gmail.com.invalid\nEMAIL_USER=gi@example.com\n"
                       "EMAIL_PASS=x\nEMAIL_FOLDER=INBOX\n");
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "gmail host over IMAP: started");
    ASSERT_WAIT_FOR(s, "gi@example.com", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Press any key to return to accounts", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "gi@example.com", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    restore_accounts("gi");
}

/* 'n' adds an account through the wizard, 'i' and 'e' edit the IMAP and SMTP
 * settings of the selected one; a wizard that is given up changes nothing. */
static void test_accounts_screen_wizards_can_be_abandoned(void) {
    char ini[512];
    snprintf(ini, sizeof(ini),
             "EMAIL_HOST=imaps://localhost:%d\nEMAIL_USER=wz@example.com\n"
             "EMAIL_PASS=x\nEMAIL_FOLDER=INBOX\nSSL_NO_VERIFY=1\n"
             "SMTP_HOST=smtps://localhost:%d\n", g_imap_port, g_dead_port);
    only_account("wz", ini);
    PtySession *s = cli_run(NULL);
    ASSERT(s != NULL, "wizards: started");
    ASSERT_WAIT_FOR(s, "wz@example.com", WAIT_MS);

    pty_send_str(s, "n");                       /* new account: give up at once */
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);
    ASSERT_WAIT_FOR(s, "Wizard aborted.", WAIT_MS);

    pty_send_str(s, "i");                       /* IMAP settings: give up */
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);
    ASSERT_WAIT_FOR(s, "Cancelled", WAIT_MS);

    pty_send_str(s, "e");                       /* SMTP settings: give up */
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);
    ASSERT_WAIT_FOR(s, "Cancelled", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    restore_accounts("wz");
}


/* ══════════════════════════════════════════════════════════════════════
 *  EMAIL-141: key study of the accounts list (docs/spec/tui-keys.md)
 * ══════════════════════════════════════════════════════════════════════ */

/** Another account in the HOME that only_account() made. */
static void add_account_here(const char *name) {
    char p[900], ini[600];
    const char *home = getenv("HOME");
    snprintf(p, sizeof(p), "%s/.config/email-cli/accounts/%s", home, name);
    mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/.config/email-cli/accounts/%s/config.ini", home, name);
    snprintf(ini, sizeof(ini),
             "EMAIL_HOST=imaps://localhost:%d\nEMAIL_USER=%s\nEMAIL_PASS=x\n"
             "EMAIL_FOLDER=INBOX\nSSL_NO_VERIFY=1\nSMTP_HOST=smtps://localhost:%d\n",
             g_imap_port, name, g_dead_port);
    FILE *f = fopen(p, "w");
    if (f) { fputs(ini, f); fclose(f); }
    chmod(p, 0600);
}

static void study_account(const char *name) {
    char ini[600];
    snprintf(ini, sizeof(ini),
             "EMAIL_HOST=imaps://localhost:%d\nEMAIL_USER=%s\nEMAIL_PASS=x\n"
             "EMAIL_FOLDER=INBOX\nSSL_NO_VERIFY=1\nSMTP_HOST=smtps://localhost:%d\n",
             g_imap_port, name, g_dead_port);
    only_account(name, ini);
}

/* The cursor keys with three accounts (a@, b@, c@ sort in this order). */
static void test_accounts_keys_cursor(void) {
    restart_mock();
    study_account("a@example.org");
    add_account_here("b@example.org");
    add_account_here("c@example.org");
    PtySession *s = cli_open_size(80, 24, NULL);
    ASSERT(s != NULL, "accounts keys cursor: started");
    ASSERT_WAIT_FOR(s, "Email Accounts (3)", WAIT_MS);
    pty_send_key(s, PTY_KEY_HOME);
    ASSERT_WAIT_FOR(s, "[1/3]", WAIT_MS);
    pty_send_key(s, PTY_KEY_UP);                         /* stops at the first */
    pty_send_str(s, "k");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1/3]");
    pty_send_key(s, PTY_KEY_DOWN);
    ASSERT_WAIT_FOR(s, "[2/3]", WAIT_MS);
    pty_send_str(s, "j");
    ASSERT_WAIT_FOR(s, "[3/3]", WAIT_MS);
    pty_send_key(s, PTY_KEY_DOWN);                       /* stops at the last */
    pty_send_str(s, "j");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[3/3]");
    pty_send_str(s, "k");
    ASSERT_WAIT_FOR(s, "[2/3]", WAIT_MS);
    pty_send_key(s, PTY_KEY_UP);
    ASSERT_WAIT_FOR(s, "[1/3]", WAIT_MS);
    pty_send_key(s, PTY_KEY_END);
    ASSERT_WAIT_FOR(s, "[3/3]", WAIT_MS);
    pty_send_key(s, PTY_KEY_HOME);
    ASSERT_WAIT_FOR(s, "[1/3]", WAIT_MS);
    /* the hints at 80 columns keep Esc */
    ASSERT_SCREEN_CONTAINS(s, "ESC");
    /* a key that is bound to nothing changes nothing */
    pty_send_str(s, "x");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts (3)");
    ASSERT_SCREEN_CONTAINS(s, "[1/3]");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT(pty_wait_exit(s, 5000) == 0, "accounts keys cursor: Esc quits");
    pty_close(s);
    restore_accounts("a");
}

/* One account: Enter, ?, i and e save, d asks and removes, at 80x24. */
static void test_accounts_keys_one_account(void) {
    restart_mock();
    study_account("one@example.org");
    PtySession *s = cli_open_size(80, 24, NULL);
    ASSERT(s != NULL, "accounts keys one: started");
    ASSERT_WAIT_FOR(s, "Email Accounts (1)", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[1/1]", WAIT_MS);
    pty_send_str(s, "j");                                /* nowhere to go */
    pty_send_key(s, PTY_KEY_PGDN);
    pty_send_key(s, PTY_KEY_END);
    pty_send_key(s, PTY_KEY_PGUP);
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1/1]");

    pty_send_str(s, "?");                                /* help at 80x24 */
    ASSERT_WAIT_FOR(s, "Accounts shortcuts", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Edit SMTP for account");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Actions of a Gmail account");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Accounts shortcuts");
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts (1)");

    pty_send_str(s, "i");                                /* keep every field */
    ASSERT_WAIT_FOR(s, "IMAP Host [current:", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "IMAP Username [current:", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "IMAP Password [Enter=keep current]", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Default Folder [current:", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "IMAP settings saved.", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "one@example.org");

    pty_send_str(s, "e");                                /* keep every field */
    ASSERT_WAIT_FOR(s, "SMTP Host [", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "SMTP Port [", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "SMTP Username [", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "SMTP Password [", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "SMTP settings saved.", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "one@example.org");

    pty_send_str(s, "d");                                /* d, y: no accounts */
    ASSERT_WAIT_FOR(s, "Delete account one@example.org? (y/N)", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "No accounts configured.", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts (0)");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT(pty_wait_exit(s, 5000) == 0, "accounts keys one: Esc quits");
    pty_close(s);
    restore_accounts("one");
}

/* No account: every key that works on an account does nothing and breaks
 * nothing; n, ? and Esc still work. */
static void test_accounts_keys_no_account(void) {
    restart_mock();
    study_account("gone@example.org");
    PtySession *s = cli_open_size(80, 24, NULL);
    ASSERT(s != NULL, "accounts keys none: started");
    ASSERT_WAIT_FOR(s, "Email Accounts (1)", WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "(y/N)", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "No accounts configured.", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    const char *inert[] = { "d", "i", "e", ":", "j", "k", "x", NULL };
    for (int i = 0; inert[i]; i++) {
        pty_send_str(s, inert[i]);
        pty_settle(s, 150);
        ASSERT_SCREEN_CONTAINS(s, "Email Accounts (0)");
        ASSERT_SCREEN_CONTAINS(s, "No accounts configured.");
        ASSERT_SCREEN_NOT_CONTAINS(s, "(y/N)");
        ASSERT_SCREEN_NOT_CONTAINS(s, "current:");
    }
    pty_send_key(s, PTY_KEY_ENTER);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_UP);
    pty_send_key(s, PTY_KEY_PGDN);
    pty_send_key(s, PTY_KEY_PGUP);
    pty_send_key(s, PTY_KEY_HOME);
    pty_send_key(s, PTY_KEY_END);
    pty_send_key(s, PTY_KEY_BACK);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts (0)");
    ASSERT_SCREEN_CONTAINS(s, "No accounts configured.");

    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Accounts shortcuts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts (0)");

    pty_send_str(s, "n");
    ASSERT_WAIT_FOR(s, "Account type", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);
    ASSERT_WAIT_FOR(s, "Wizard aborted.", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts (0)");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT(pty_wait_exit(s, 5000) == 0, "accounts keys none: Esc quits");
    pty_close(s);
    restore_accounts("gone");
}

/* d, y at the end and at the start of the list: the account goes, the cursor
 * stays on the list. */
static void test_accounts_keys_delete_position(void) {
    restart_mock();
    study_account("a@example.org");
    add_account_here("b@example.org");
    add_account_here("c@example.org");
    PtySession *s = cli_open_size(80, 24, NULL);
    ASSERT(s != NULL, "accounts keys delete: started");
    ASSERT_WAIT_FOR(s, "Email Accounts (3)", WAIT_MS);
    pty_send_key(s, PTY_KEY_END);
    ASSERT_WAIT_FOR(s, "[3/3]", WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "Delete account c@example.org? (y/N)", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Email Accounts (2)", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "c@example.org");
    ASSERT_WAIT_FOR(s, "Local messages preserved", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[2/2]", WAIT_MS);                /* the new last row */
    pty_send_key(s, PTY_KEY_HOME);
    ASSERT_WAIT_FOR(s, "[1/2]", WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "Delete account a@example.org? (y/N)", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Email Accounts (1)", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "b@example.org");
    ASSERT_WAIT_FOR(s, "[1/1]", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);                      /* the one left opens */
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
    restore_accounts("a");
}

/* ══════════════════════════════════════════════════════════════════════
 *  EMAIL-152: key study of the setup wizard and the bulk-action prompt
 * ══════════════════════════════════════════════════════════════════════ */


/* The wizard is a line dialog, one line per answer, not a key-driven screen:
 * letters and Space are text.  Esc cancels the wizard, ctrl-c quits and
 * Ctrl-D ends the input and gives the wizard up (EMAIL-223). */
static PtySession *wk_open_wizard(const char *name) {
    study_account(name);
    PtySession *s = cli_open_size(80, 24, NULL);
    if (!s) return NULL;
    char acc[64];
    snprintf(acc, sizeof acc, "%s", name);
    if (pty_wait_for(s, acc, WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_str(s, "n");
    if (pty_wait_for(s, "Choice [1]:", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* Space, m r u f F l L D are text at the type prompt: each is refused with a
 * message and the question is asked again.  Esc cancels the wizard
 * (EMAIL-223); the accounts list returns unchanged and Esc there quits. */
static void test_wk_wizard_type_prompt(void) {
    PtySession *s = wk_open_wizard("wk1");
    ASSERT(s != NULL, "wizard type: opens");
    if (!s) return;
    ASSERT_SCREEN_CONTAINS(s, "email-cli Configuration Wizard");
    ASSERT_SCREEN_CONTAINS(s, "[1] IMAP");
    ASSERT_SCREEN_CONTAINS(s, "[2] Gmail");
    ASSERT_ROW_CONTAINS(s, 23, "Choice [1]:");
    pty_send_str(s, " \n");
    ASSERT_WAIT_FOR(s, "Unknown account type ' '", WAIT_MS);
    const char *keys[] = { "m", "r", "u", "f", "F", "l", "L", "D", NULL };
    for (int i = 0; keys[i]; i++) {
        char want[64];
        pty_send_str(s, keys[i]);
        pty_send_str(s, "\n");
        snprintf(want, sizeof want, "Unknown account type '%s'", keys[i]);
        ASSERT_WAIT_FOR(s, want, WAIT_MS);
    }
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 23, "Choice [1]:");
    pty_send_key(s, PTY_KEY_ESC);                    /* EMAIL-223: cancels */
    ASSERT_WAIT_FOR(s, "Wizard aborted.", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "^[");
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts (1)");
    ASSERT_SCREEN_CONTAINS(s, "wk1");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT(pty_wait_exit(s, 5000) == 0, "wizard type: Esc on the list quits");
    pty_close(s);
    restore_accounts("wk1");
}

/* IMAP flow: Enter takes the default type, the port and the folder; Esc in
 * the host line is text (EMAIL-223); Ctrl-D at the hidden password prompt
 * gives the wizard up; an empty password is said to be empty; the finished
 * wizard adds the account. */
static void wk_to_password(PtySession *s) {
    pty_send_str(s, "n");
    pty_wait_for(s, "Account type", WAIT_MS);
    pty_send_str(s, "\n");
    pty_wait_for(s, "IMAP Host", WAIT_MS);
    send_host(s, "localhost:%d\n");
    pty_wait_for(s, "IMAP Port [993]", WAIT_MS);
    pty_send_str(s, "\n");
    pty_wait_for(s, "Email Username", WAIT_MS);
    pty_send_str(s, "wk3@example.org\n");
    pty_wait_for(s, "Email Password", WAIT_MS);
}

/* Esc in the host line cancels the wizard; a wizard given up with Ctrl-D does
 * not end the later ones of the session (EMAIL-223). */
static void test_wk_wizard_imap_flow(void) {
    PtySession *s = wk_open_wizard("wk2");
    ASSERT(s != NULL, "wizard flow: opens");
    if (!s) return;
    pty_send_str(s, "\n");                           /* Enter: IMAP */
    ASSERT_WAIT_FOR(s, "IMAP Host", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);                    /* EMAIL-223: cancels */
    ASSERT_WAIT_FOR(s, "Wizard aborted.", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts (1)");
    pty_send_str(s, "n");                            /* again: Ctrl-D gives up */
    ASSERT_WAIT_FOR(s, "Choice [1]:", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);
    ASSERT_WAIT_FOR(s, "Wizard aborted.", WAIT_MS);
    pty_send_str(s, "n");                            /* a wizard after a given-up one */
    ASSERT_WAIT_FOR(s, "Choice [1]:", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);
    ASSERT_WAIT_FOR(s, "Wizard aborted.", WAIT_MS);
    pty_close(s);

    s = cli_open_size(80, 24, NULL);                 /* Ctrl-D at the password */
    ASSERT(s != NULL, "wizard password: opens");
    if (!s) { restore_accounts("wk2"); return; }
    ASSERT_WAIT_FOR(s, "wk2", WAIT_MS);
    wk_to_password(s);
    pty_send_key(s, PTY_KEY_CTRL_D);
    ASSERT_WAIT_FOR(s, "Wizard aborted.", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "wk3@example.org  ");
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts (1)");
    pty_close(s);

    s = cli_open_size(80, 24, NULL);                 /* the whole flow */
    ASSERT(s != NULL, "wizard flow: reopens");
    if (!s) { restore_accounts("wk2"); return; }
    ASSERT_WAIT_FOR(s, "wk2", WAIT_MS);
    wk_to_password(s);
    pty_send_str(s, "\n");                           /* empty password */
    ASSERT_WAIT_FOR(s, "no password entered", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Default Folder [INBOX]", WAIT_MS);
    pty_send_str(s, "\n");
    ASSERT_WAIT_FOR(s, "SMTP Host [Enter=skip]", WAIT_MS);
    pty_send_str(s, "\n");                           /* Enter skips SMTP */
    ASSERT_WAIT_FOR(s, "Email Accounts (2)", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "wk3@example.org");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT(pty_wait_exit(s, 5000) == 0, "wizard flow: Esc on the list quits");
    pty_close(s);
    restore_accounts("wk2");
}

/* ctrl-c at a wizard prompt quits the program at once (EMAIL-223). */
static void test_wk_wizard_ctrl_c(void) {
    PtySession *s = wk_open_wizard("wk4");
    ASSERT(s != NULL, "wizard ctrl-c: opens");
    if (!s) return;
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT_WAIT_FOR(s, "^C", WAIT_MS);
    ASSERT(pty_wait_exit(s, 5000) == 0, "wizard ctrl-c: quits (EMAIL-223)");
    pty_close(s);
    restore_accounts("wk4");
}

/* First start without any account: the wizard runs before the TUI; Ctrl-D
 * ends the program with a failure status and a message. */
static void test_wk_wizard_first_run(void) {
    char wiz_home[300];
    snprintf(wiz_home, sizeof(wiz_home), "%s/wizard_first_run", g_test_home);
    mkdir(wiz_home, 0700);
    setenv("HOME", wiz_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    PtySession *s = cli_open_size(80, 24, NULL);
    ASSERT(s != NULL, "wizard first run: opens");
    if (!s) { setenv("HOME", g_test_home, 1); return; }
    ASSERT_WAIT_FOR(s, "Account type", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "email-cli Configuration Wizard");
    pty_send_str(s, "x\n");
    ASSERT_WAIT_FOR(s, "Unknown account type 'x'", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);
    ASSERT_WAIT_FOR(s, "Configuration aborted. Exiting.", WAIT_MS);
    ASSERT(pty_wait_exit(s, 5000) == 1, "wizard first run: exits with failure");
    pty_close(s);
    setenv("HOME", g_test_home, 1);
}

/* The bulk-action prompt: Space marks, d asks once for all the marked
 * messages.  Only y, n, Esc, q, Backspace, Enter, Left/Right/Tab and ctrl-c
 * do anything in it; every other key (Space m r u f F l L D j) is ignored. */
static void test_bk_prompt_keys(void) {
    lk_env(30);
    PtySession *s = lk_open();
    ASSERT(s != NULL, "bulk prompt: opens");
    if (!s) { lk_env(0); return; }
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[1 marked]", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[2 marked]", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[3/30]", WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "Move 2 marked messages to Trash?", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "[ Yes ]  [ No ]");
    ASSERT_ROW_CONTAINS(s, 23, "y yes  n no  Esc/q/BS cancel");
    const char *ignored[] = { " ", "m", "r", "u", "f", "F", "l", "L", "D", "j", "x", "d", NULL };
    for (int i = 0; ignored[i]; i++) pty_send_str(s, ignored[i]);
    PtyKey nav[] = { PTY_KEY_UP, PTY_KEY_DOWN, PTY_KEY_PGDN, PTY_KEY_PGUP,
                     PTY_KEY_HOME, PTY_KEY_END, PTY_KEY_F1 };
    for (size_t i = 0; i < sizeof nav / sizeof nav[0]; i++) pty_send_key(s, nav[i]);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Move 2 marked messages to Trash?");
    ASSERT_SCREEN_CONTAINS(s, "[2 marked]");
    /* each way of saying no: the marks and the cursor stay */
    for (int i = 0; i < 5; i++) {
        if (i == 0) pty_send_key(s, PTY_KEY_ESC);
        else if (i == 1) pty_send_str(s, "q");
        else if (i == 2) pty_send_key(s, PTY_KEY_BACK);
        else if (i == 3) pty_send_str(s, "n");
        else pty_send_key(s, PTY_KEY_ENTER);          /* the default is No */
        ASSERT_WAIT_FOR(s, "Not deleted", WAIT_MS);
        pty_settle(s, SETTLE_MS);
        ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash?");
        ASSERT_SCREEN_CONTAINS(s, "[2 marked]");
        ASSERT_SCREEN_CONTAINS(s, "[3/30]");
        ASSERT_SCREEN_CONTAINS(s, "30 message(s) in");
        pty_send_str(s, "d");
        ASSERT_WAIT_FOR(s, "Move 2 marked messages to Trash?", WAIT_MS);
    }
    pty_send_key(s, PTY_KEY_CTRL_C);                  /* quits at once */
    ASSERT(pty_wait_exit(s, 5000) == 0, "bulk prompt: ctrl-c quits at once");
    pty_close(s);
    lk_env(0);
}

/** The server drops an idle connection (the mock does after 3 s); quitting
 *  then writes LOGOUT to a closed socket, which must not kill the program
 *  with SIGPIPE (EMAIL-152): it exits with status 0. */
static void test_bk_quit_after_server_dropped(void) {
    lk_env(0);
    PtySession *s = lk_open();
    ASSERT(s != NULL, "list opens");
    pty_settle(s, 4500);
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, 5000) == 0, "ctrl-c after the server dropped the connection exits cleanly");
    pty_close(s);
    lk_env(0);
}

/* y moves every marked message at once; Y too; the marks are gone afterwards. */
static void test_bk_prompt_yes(void) {
    lk_env(30);
    PtySession *s = lk_open();
    ASSERT(s != NULL, "bulk yes: opens");
    if (!s) { lk_env(0); return; }
    pty_send_str(s, " ");
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[2 marked]", WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "Move 2 marked messages to Trash?", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "2 message(s) moved to Trash", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
    lk_env(0);
}

/* The buttons: Left, Right and Tab move between [ Yes ] and [ No ], Enter
 * takes the one in focus (No at first).  Not on the status line. */
static void test_bk_prompt_buttons(void) {
    lk_env(30);
    PtySession *s = lk_open();
    ASSERT(s != NULL, "bulk buttons: opens");
    if (!s) { lk_env(0); return; }
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[1 marked]", WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash?", WAIT_MS);
    pty_send_key(s, PTY_KEY_LEFT);
    pty_send_key(s, PTY_KEY_RIGHT);                   /* back on No */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Not deleted", WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash?", WAIT_MS);
    pty_send_key(s, PTY_KEY_TAB);                     /* No -> Yes */
    pty_send_key(s, PTY_KEY_TAB);                     /* Yes -> No */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Not deleted", WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash?", WAIT_MS);
    pty_send_key(s, PTY_KEY_LEFT);                    /* No -> Yes */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Moved to Trash", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
    lk_env(0);
}

/* One message: Space marks it (no step), d names the message, not a count. */
static void test_bk_prompt_one_item(void) {
    lk_env(1);
    PtySession *s = lk_open();
    ASSERT(s != NULL, "bulk one: opens");
    if (!s) { lk_env(0); return; }
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[1 marked]", WAIT_MS);
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash?", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Test Message");
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked messages");
    pty_send_str(s, "m");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "to Trash?");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Not deleted", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1 marked]");
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash?", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Moved to Trash", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
    lk_env(0);
}

/* No message: Space, d and the letters m r u f F l L D do nothing. */
static void test_bk_prompt_no_item(void) {
    lk_env(0);
    PtySession *s = lk_open_empty();
    ASSERT(s != NULL, "bulk none: opens");
    if (!s) return;
    const char *keys[] = { " ", "d", "m", "r", "u", "f", "F", "l", "L", "D", NULL };
    for (int i = 0; keys[i]; i++) {
        if (strcmp(keys[i], "l") == 0) continue;     /* l opens the rules editor */
        pty_send_str(s, keys[i]);
    }
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "0 of 0 message(s)");
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Folders", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
}

/* With rows marked, the old letters m u f D L do nothing (the marks stay),
 * Space on a marked row unmarks it, and Esc drops the marks first. */
static void test_bk_marks_and_old_letters(void) {
    lk_env(30);
    PtySession *s = lk_open();
    ASSERT(s != NULL, "bulk letters: opens");
    if (!s) { lk_env(0); return; }
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[1 marked]", WAIT_MS);
    pty_send_key(s, PTY_KEY_HOME);
    pty_send_str(s, " ");                             /* Space on a marked row */
    ASSERT_WAIT_FOR(s, "[2/30]", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");         /* unmarked: a toggle */
    pty_send_key(s, PTY_KEY_HOME);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "[1 marked]", WAIT_MS);
    const char *old[] = { "m", "u", "f", "D", "L", NULL };
    for (int i = 0; old[i]; i++) pty_send_str(s, old[i]);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[1 marked]");
    ASSERT_SCREEN_CONTAINS(s, "[2/30]");
    ASSERT_SCREEN_CONTAINS(s, "30 message(s) in");
    ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "marked]");
    ASSERT_SCREEN_CONTAINS(s, "30 message(s) in");
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_wait_exit(s, 5000);
    pty_close(s);
    lk_env(0);
}

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

    snprintf(g_test_home, sizeof(g_test_home), "%s", TP("home"));
    setenv("HOME", g_test_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_DATA_HOME");

    /* The mock first: the configs written next name its port. */
    g_dead_port = test_unused_port();
    printf("--- Starting mock IMAP server ---\n");
    if (start_mock_server() != 0 || g_dead_port < 0) {
        fprintf(stderr, "FATAL: cannot start mock server\n");
        g_tests_failed++;           /* a fixture that cannot start is a failure */
        goto done;
    }
    write_config();

    printf("--- Help pages ---\n");
    RUN_TEST(test_help_general);
    RUN_TEST(test_help_list);
    RUN_TEST(test_help_show);
    RUN_TEST(test_help_folders);
    RUN_TEST(test_help_sync);
    RUN_TEST(test_help_cron);


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
    RUN_TEST(test_batch_rules_apply);
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
    RUN_TEST(test_interactive_list_q_goes_back);
    RUN_TEST(test_accounts_pages_by_the_visible_rows);
    RUN_TEST(test_interactive_list_ctrl_c_quits_at_once);
    RUN_TEST(test_interactive_header_bar_palette);
    RUN_TEST(test_interactive_list_nav);
    RUN_TEST(test_interactive_list_flags);
    RUN_TEST(test_tui_list_search_filter);
    RUN_TEST(test_tui_list_date_filter);

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
    RUN_TEST(test_interactive_show_search_then_unread_keys);
    RUN_TEST(test_interactive_show_url_rendered);

    printf("\n--- Interactive list-folders ---\n");
    RUN_TEST(test_interactive_folders_content);
    RUN_TEST(test_interactive_folders_statusbar);
    RUN_TEST(test_interactive_folders_toggle);
    RUN_TEST(test_interactive_folders_select);
    RUN_TEST(test_interactive_folders_nav);
    RUN_TEST(test_interactive_folders_back_to_list);
    RUN_TEST(test_interactive_folders_flat_navigate_up);
    RUN_TEST(test_interactive_folders_flat_parent_openable);
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
    RUN_TEST(test_picker_back_keys);
    RUN_TEST(test_picker_ctrl_c_quits);
    RUN_TEST(test_picker_navigation);
    RUN_TEST(test_picker_help);
    RUN_TEST(test_picker_unbound_key);
    RUN_TEST(test_a_one_attachment);
    RUN_TEST(test_a_without_attachments);
    RUN_TEST(test_picker_choose_second_then_cancel);

    printf("\n--- Empty folder ---\n");
    RUN_TEST(test_interactive_empty_folder);
    RUN_TEST(test_folder_tree_wide_characters_one_row_each);
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
    RUN_TEST(test_lk_navigation);
    RUN_TEST(test_lk_header_range_matches_rows);
    RUN_TEST(test_lk_compose_reply_forward);
    RUN_TEST(test_lk_marks);
    RUN_TEST(test_lk_enter_opens_reader);
    RUN_TEST(test_lk_back_keys);
    RUN_TEST(test_lk_refresh_and_rules);
    RUN_TEST(test_lk_help_overlay);
    RUN_TEST(test_lk_unbound_keys);
    RUN_TEST(test_lk_filter_without_match);
    RUN_TEST(test_lk_delete_asks);
    RUN_TEST(test_lk_one_message);
    RUN_TEST(test_lk_empty_folder);
    RUN_TEST(test_rk_scroll);
    RUN_TEST(test_rk_back_keys);
    RUN_TEST(test_rk_ctrl_c);
    RUN_TEST(test_rk_short_message);
    RUN_TEST(test_rk_search);
    RUN_TEST(test_rk_state_keys);
    RUN_TEST(test_rk_delete);
    RUN_TEST(test_rk_reply_forward);
    RUN_TEST(test_rk_attachments);
    RUN_TEST(test_rk_unbound_keys);
    RUN_TEST(test_fk_navigation);
    RUN_TEST(test_fk_enter);
    RUN_TEST(test_fk_tree_open_close);
    RUN_TEST(test_fk_flat);
    RUN_TEST(test_fk_back_keys);
    RUN_TEST(test_fk_ctrl_c);
    RUN_TEST(test_fk_search);
    RUN_TEST(test_fk_compose_keeps_row);
    RUN_TEST(test_fk_zero_folders);
    RUN_TEST(test_fk_one_folder);
    RUN_TEST(test_fk_help);
    RUN_TEST(test_fk_menu);
    RUN_TEST(test_fk_unbound_keys);
    RUN_TEST(test_tui_list_content);
    RUN_TEST(test_resize_list_redraws_at_once);
    RUN_TEST(test_statusline_one_row_hints_position);
    RUN_TEST(test_statusline_message_replaces_hints);
    RUN_TEST(test_statusline_narrow_keeps_back_and_position);
    RUN_TEST(test_resize_keeps_selection);
    RUN_TEST(test_resize_below_minimum_shows_notice);
    RUN_TEST(test_start_below_minimum_shows_notice);
    RUN_TEST(test_resize_on_every_screen);
    RUN_TEST(test_tui_list_space_marks);
    RUN_TEST(test_tui_list_esc_clears_marks);
    RUN_TEST(test_tui_list_bulk_star_marked);
    RUN_TEST(test_tui_list_bulk_cursor_row);
    RUN_TEST(test_tui_list_bulk_cancel);
    RUN_TEST(test_tui_list_bulk_delete_confirm);
    RUN_TEST(test_keys_registry_routine);
    RUN_TEST(test_keys_junk_toggle_imap);
    RUN_TEST(test_keys_old_letters_unbound);
    RUN_TEST(test_keys_delete_dialog);
    RUN_TEST(test_keys_action_menu);
    RUN_TEST(test_keys_sync_asks);
    RUN_TEST(test_folder_menu_lists_actions);
    RUN_TEST(test_folder_help_lists_menu);
    RUN_TEST(test_folder_create);
    RUN_TEST(test_folder_create_cancelled);
    RUN_TEST(test_folder_delete_asks);
    RUN_TEST(test_folder_delete_needs_a_folder);
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
    RUN_TEST(test_tui_accounts_colon_on_imap_account);
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
    RUN_TEST(test_tui_accounts_delete_declined);
    RUN_TEST(test_tui_accounts_imap_edit_key);

    printf("\n--- email-sync: account filter (US-25) ---\n");
    restart_mock();
    RUN_TEST(test_sync_account_filter_known);
    RUN_TEST(test_sync_account_filter_unknown);

    printf("\n--- pending flags offline queue (US-26) ---\n");
    restart_mock();
    RUN_TEST(test_pending_flags_offline_queue);
    RUN_TEST(test_delete_offline_queues_move);

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
    RUN_TEST(test_tui_rules_detail_delete_cancel_stays);
    RUN_TEST(test_tui_rules_list_delete_arrow_cancels);

    /* ── TUI rule edit form (US-80) ─────────────────────────────────── */
    printf("\n--- email-tui: rule edit form inline editing (US-80) ---\n");
    restart_mock();
    RUN_TEST(test_tui_rules_edit_form_opens);
    RUN_TEST(test_tui_rules_edit_form_prefill);
    RUN_TEST(test_tui_rules_edit_form_saves);
    RUN_TEST(test_tui_rules_add_form_saves_new_rule);
    RUN_TEST(test_tui_rules_preview_and_apply);
    RUN_TEST(test_tui_rules_add_form_needs_a_name);
    RUN_TEST(test_tui_rules_edit_form_esc_cancel);
    RUN_TEST(test_tui_function_and_modified_keys_do_nothing);

    printf("\n--- email-tui: accounts screen and accounts that cannot be opened (EMAIL-115) ---\n");
    RUN_TEST(test_account_unreachable_returns_to_accounts);
    RUN_TEST(test_account_imap_gmail_host_is_explained);
    RUN_TEST(test_accounts_screen_wizards_can_be_abandoned);
    RUN_TEST(test_accounts_keys_cursor);
    RUN_TEST(test_accounts_keys_one_account);
    RUN_TEST(test_accounts_keys_no_account);
    RUN_TEST(test_accounts_keys_delete_position);
    printf("\n--- email-tui: setup wizard and bulk-action prompt (EMAIL-152) ---\n");
    RUN_TEST(test_wk_wizard_type_prompt);
    RUN_TEST(test_wk_wizard_imap_flow);
    RUN_TEST(test_wk_wizard_ctrl_c);
    RUN_TEST(test_wk_wizard_first_run);
    RUN_TEST(test_bk_prompt_keys);
    RUN_TEST(test_bk_quit_after_server_dropped);
    RUN_TEST(test_bk_prompt_yes);
    RUN_TEST(test_bk_prompt_buttons);
    RUN_TEST(test_bk_prompt_one_item);
    RUN_TEST(test_bk_prompt_no_item);
    RUN_TEST(test_bk_marks_and_old_letters);

done:
    stop_mock_server();
    if (g_old_home[0]) setenv("HOME", g_old_home, 1);

    printf("\n--- PTY Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
