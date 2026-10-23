/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-22 */
/* PTY tests of the history and revert pages (EMAIL-226). */
/**
 * @file test_pty_history.c
 * @brief The history view of email-tui (H in the message list, H in the
 *        reader, Revert with a confirmation whose default is No, the notice on
 *        a step that is not revertable) and the history / revert pages of
 *        email-cli, all at 80x24 with their colours.  Spec:
 *        docs/spec/operation-history.md.
 *
 * Usage: test-pty-history <email-tui> <mock-imap-server> <email-cli>
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ptytest.h"
#include "pty_assert.h"
#include "wait_scale.h"
#include "test_scratch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

int g_tests_run = 0;
int g_tests_failed = 0;

#define RUN_TEST(fn) \
    do { \
        if (!test_selected(#fn)) break; \
        fprintf(stdout, "  %s...\n", #fn); \
        fflush(stdout); \
        fn(); \
    } while (0)

#define ROWS 24
#define COLS 80
#define WAIT_MS (6000 * wait_scale())
#define SETTLE_MS 400

static pid_t g_mock_pid = -1;
static int   g_imap_port = 0;
static char  g_home[256];
static char  g_tui_bin[512];
static char  g_cli_bin[512];
static char  g_mock_bin[512];
static int   g_seq = 0;

static void restart_mock(void) {
    if (g_mock_pid > 0) {
        kill(g_mock_pid, SIGKILL);
        waitpid(g_mock_pid, NULL, 0);
        g_mock_pid = -1;
    }
    int port = 0;
    g_mock_pid = test_spawn_mock(g_mock_bin, "MOCK_IMAP", g_imap_port, &port);
    if (g_mock_pid > 0) g_imap_port = port;
}

static void make_home(const char *name) {
    char dir[400], path[450];
    snprintf(g_home, sizeof(g_home), "%s", TP("h"));
    size_t n = strlen(g_home);
    snprintf(g_home + n, sizeof(g_home) - n, "-%s-%d", name, ++g_seq);
    snprintf(dir, sizeof(dir), "%s", g_home);                                  mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.config", g_home);                          mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.config/email-cli", g_home);                mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.config/email-cli/accounts", g_home);       mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.config/email-cli/accounts/hist@example.org", g_home); mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", dir);
    FILE *fp = fopen(path, "w");
    if (!fp) { g_tests_failed++; return; }
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:%d\n"
        "EMAIL_USER=hist@example.org\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SSL_NO_VERIFY=1\n", g_imap_port);
    fclose(fp);
    chmod(path, 0600);
}

/* Runs email-cli in the test home with @p args (a shell fragment); -q is the
 * caller's.  Its output is left in the test's own output. */
static int cli(const char *args) {
    char cmd[1200];
    snprintf(cmd, sizeof(cmd),
             "HOME='%s' TZ=UTC XDG_CONFIG_HOME= XDG_DATA_HOME= XDG_CACHE_HOME= "
             "'%s' hist@example.org %s </dev/null", g_home, g_cli_bin, args);
    return system(cmd);
}

/* A home with one revertable step (message 1 starred) and one that is not
 * (message 3 deleted for good from the Trash folder). */
static void seed(void) {
    restart_mock();
    make_home("hist");
    cli("list >/dev/null");
    cli("mark-starred 1 >/dev/null");
    cli("trash 3 --folder Trash -q >/dev/null");
}

static PtySession *tui_open(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    pty_setenv(s, "TZ", "UTC");
    pty_setenv(s, "HOME", g_home);
    pty_setenv(s, "XDG_CONFIG_HOME", NULL);
    pty_setenv(s, "XDG_DATA_HOME", NULL);
    pty_setenv(s, "XDG_CACHE_HOME", NULL);
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    return s;
}

/* The message list of the mock's INBOX. */
static PtySession *at_list(void) {
    PtySession *s = tui_open();
    if (!s) return NULL;
    if (pty_wait_for(s, "Email Accounts", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "message(s) in", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

#define CHECK(s, cond, ...) do { \
    char _w[200]; snprintf(_w, sizeof(_w), __VA_ARGS__); \
    if (!(cond)) pty_screen_dump((s), stdout); \
    ASSERT((cond), _w); \
} while (0)

/* ── The TUI ─────────────────────────────────────────────────────────── */

static int row_has(PtySession *s, int row, const char *text) {
    return pty_row_contains(s, row, text);
}

/* The row (0..23) of the screen that holds @p text, or -1. */
static int find_row(PtySession *s, const char *text) {
    for (int r = 0; r < ROWS; r++) if (row_has(s, r, text)) return r;
    return -1;
}

static int style_has(PtySession *s, int row, int col, const char *what) {
    char d[200];
    pty_cell_describe(s, row, col, d, sizeof(d));
    return strstr(d, what) != NULL;
}

/* Opens the recent changes from the message list. */
static PtySession *at_recent(void) {
    seed();
    PtySession *s = at_list();
    if (!s) return NULL;
    pty_send_str(s, "H");
    if (pty_wait_for(s, "Recent changes", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static void test_hist_tui_recent(void) {
    PtySession *s = at_recent();
    ASSERT(s != NULL, "H in the message list opens the recent changes");
    if (!s) return;
    CHECK(s, row_has(s, 0, "Recent changes"), "the title bar names the view");
    CHECK(s, row_has(s, 1, "Time") && row_has(s, 1, "Who") && row_has(s, 1, "Operation")
             && row_has(s, 1, "Object") && row_has(s, 1, "Thing")
             && row_has(s, 1, "Old") && row_has(s, 1, "New") && row_has(s, 1, "Note"),
          "the header names every column");
    CHECK(s, row_has(s, 2, "delete") && row_has(s, 2, "not revertable"),
          "the newest step (the permanent delete) is first and not revertable");
    CHECK(s, row_has(s, 3, "flag") && row_has(s, 3, "starr"),
          "the flag step is second");
    CHECK(s, !row_has(s, 3, "not revertable"), "a revertable step carries no marker");
    /* colours */
    CHECK(s, pty_cell_fg(s, 0, 2) == 37 && pty_cell_bg(s, 0, 2) == 44 &&
             (pty_cell_attr(s, 0, 2) & PTY_ATTR_BOLD), "title bar: bold white on blue");
    CHECK(s, (pty_cell_attr(s, 1, 3) & PTY_ATTR_BOLD), "header row is bold");
    CHECK(s, strcmp(pty_cell_text(s, 2, 0), ">") == 0, "the selection mark is on the first row");
    CHECK(s, style_has(s, 2, 3, "fg bright white (97)") && style_has(s, 2, 3, "bg palette 17"),
          "selected row: bright white on the selection background");
    CHECK(s, style_has(s, 2, 66, "fg bright yellow (93)"),
          "the 'not revertable' marker is bright yellow");
    CHECK(s, style_has(s, 3, 3, "plain"), "an unselected row is plain");
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 300);
    CHECK(s, strcmp(pty_cell_text(s, 3, 0), ">") == 0, "Down moves the selection");
    CHECK(s, style_has(s, 2, 66, "fg yellow") || style_has(s, 2, 66, "fg bright yellow"),
          "the marker stays yellow on an unselected row");
    pty_send_key(s, PTY_KEY_ESC);
    CHECK(s, pty_wait_for(s, "message(s) in", WAIT_MS) == 0, "Esc leaves the view");
    pty_close(s);
}

static void test_hist_tui_help_empty(void) {
    restart_mock();
    make_home("empty");
    PtySession *s = at_list();
    ASSERT(s != NULL, "the message list opens");
    if (!s) return;
    pty_send_str(s, "H");
    CHECK(s, pty_wait_for(s, "Recent changes", WAIT_MS) == 0, "the empty view opens");
    pty_settle(s, SETTLE_MS);
    CHECK(s, row_has(s, 1, "no recorded changes"), "an empty history says so");
    pty_send_str(s, "?");
    pty_settle(s, SETTLE_MS);
    CHECK(s, find_row(s, "Revert") >= 0 || find_row(s, "Back") >= 0 || find_row(s, "Esc") >= 0,
          "? opens a help made from the key table");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, 300);
    pty_send_key(s, PTY_KEY_ESC);
    CHECK(s, pty_wait_for(s, "message(s) in", WAIT_MS) == 0, "Esc leaves the view");
    pty_close(s);
}

static void test_hist_tui_not_revertable(void) {
    PtySession *s = at_recent();
    ASSERT(s != NULL, "the recent changes open");
    if (!s) return;
    pty_send_key(s, PTY_KEY_ENTER);   /* on the permanent delete */
    pty_settle(s, 400);
    CHECK(s, row_has(s, 23, "step 2 is not revertable"), "Enter shows the notice");
    CHECK(s, find_row(s, "[ Yes ]") < 0, "no confirmation is opened for it");
    pty_close(s);
}

static void test_hist_tui_confirm_default_no(void) {
    PtySession *s = at_recent();
    ASSERT(s != NULL, "the recent changes open");
    if (!s) return;
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_str(s, ":");
    pty_settle(s, 400);
    CHECK(s, find_row(s, "Actions") >= 0 && find_row(s, "Revert - undo the selected step") >= 0,
          ": opens the action menu with Revert");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, 400);
    int r = find_row(s, "Revert step 1");
    CHECK(s, r >= 0, "the confirmation names the step");
    CHECK(s, find_row(s, "starred of mail INBOX/0000000000000001") >= 0 || find_row(s, "starred") >= 0,
          "it names the thing");
    CHECK(s, find_row(s, "[ Yes ]") >= 0 && find_row(s, "[ No ]") >= 0, "it offers Yes and No");
    pty_send_key(s, PTY_KEY_ENTER);   /* the default answer */
    pty_settle(s, 400);
    CHECK(s, find_row(s, "[ Yes ]") < 0, "Enter (the default, No) closes the dialog");
    CHECK(s, row_has(s, 3, "flag") && row_has(s, 23, "") && find_row(s, "overtaken") < 0
             && find_row(s, "reverted") < 0, "nothing was reverted");
    pty_close(s);
}

static void test_hist_tui_overtaken(void) {
    /* The mock server forgets the flag when the TUI reloads the folder, so the
     * step is overtaken by the time Revert is confirmed: the refusal is shown
     * in words, at the bottom. */
    PtySession *s = at_recent();
    ASSERT(s != NULL, "the recent changes open");
    if (!s) return;
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, 400);
    pty_send_str(s, "y");
    pty_settle(s, 600);
    CHECK(s, row_has(s, 23, "was overtaken"), "an overtaken step is refused in words");
    CHECK(s, find_row(s, "reverts") < 0, "no step was added");
    pty_close(s);
}

/* Stars message 1 in the list, then opens the recent changes. */
static PtySession *star_then_history(void) {
    restart_mock();
    make_home("star");
    PtySession *s = at_list();
    if (!s) return NULL;
    pty_send_str(s, "*");
    pty_settle(s, 600);
    pty_send_str(s, "H");
    if (pty_wait_for(s, "Recent changes", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static void test_hist_tui_revert_and_revert_again(void) {
    PtySession *s = star_then_history();
    ASSERT(s != NULL, "star a message, then open the history");
    if (!s) return;
    CHECK(s, row_has(s, 2, "flag") && row_has(s, 2, "starr") && row_has(s, 2, "no"),
          "the star is recorded");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, 400);
    CHECK(s, find_row(s, "Revert step 1") >= 0, "Enter opens the confirmation");
    pty_send_str(s, "y");
    pty_wait_for(s, "reverts 1", WAIT_MS);
    pty_settle(s, 300);
    CHECK(s, row_has(s, 2, "reverts 1") && row_has(s, 2, "flag"),
          "the revert is the newest step and names the step it reverts");
    CHECK(s, row_has(s, 3, "flag") && !row_has(s, 3, "reverts"), "the first step stays");
    pty_send_key(s, PTY_KEY_UP);      /* the selection stays on step 1 */
    pty_settle(s, 300);
    pty_send_key(s, PTY_KEY_ENTER);   /* the revert, on the first row */
    pty_settle(s, 400);
    CHECK(s, find_row(s, "Revert step 2") >= 0, "the revert can be reverted");
    pty_send_str(s, "y");
    pty_wait_for(s, "reverts 2", WAIT_MS);
    pty_settle(s, 300);
    CHECK(s, row_has(s, 2, "reverts 2"), "reverting the revert is a third step");
    pty_close(s);
}

static void test_hist_tui_reader(void) {
    seed();
    PtySession *s = at_list();
    ASSERT(s != NULL, "the message list opens");
    if (!s) return;
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "H");
    CHECK(s, pty_wait_for(s, "History of mail INBOX/", WAIT_MS) == 0,
          "H in the reader opens the history of this message");
    pty_settle(s, SETTLE_MS);
    CHECK(s, !row_has(s, 1, "Object"), "the history of one object has no object column");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, 400);
    CHECK(s, find_row(s, "History of mail") < 0, "Esc returns to the message");
    pty_close(s);
}

/* ── The command line ────────────────────────────────────────────────── */

static PtySession *cli_open(const char *a1, const char *a2, const char *a3) {
    const char *args[] = { g_cli_bin, "hist@example.org", a1, a2, a3, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    pty_setenv(s, "TZ", "UTC");
    pty_setenv(s, "TERM", "xterm");
    pty_setenv(s, "HOME", g_home);
    pty_setenv(s, "XDG_CONFIG_HOME", NULL);
    pty_setenv(s, "XDG_DATA_HOME", NULL);
    pty_setenv(s, "XDG_CACHE_HOME", NULL);
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    return s;
}

static void test_hist_cli_history_page(void) {
    seed();
    PtySession *s = cli_open("history", NULL, NULL);
    ASSERT(s != NULL, "email-cli history runs");
    if (!s) return;
    CHECK(s, pty_wait_for(s, "not revertable", WAIT_MS) == 0, "the page is printed");
    pty_settle(s, 300);
    int h = find_row(s, "ID ");
    CHECK(s, h >= 0 && row_has(s, h, "WHEN") && row_has(s, h, "WHO") && row_has(s, h, "WHAT"),
          "the header names the columns");
    CHECK(s, h >= 0 && (pty_cell_attr(s, h, 0) & PTY_ATTR_BOLD), "the header is bold");
    int d = find_row(s, "delete");
    CHECK(s, d >= 0 && (pty_cell_attr(s, d, 0) & PTY_ATTR_DIM),
          "a step that is not revertable is dimmed");
    int f = find_row(s, "flag");
    CHECK(s, f >= 0 && !(pty_cell_attr(s, f, 0) & PTY_ATTR_DIM), "a revertable step is not");
    /* a line longer than the terminal wraps: the text is checked in its parts */
    CHECK(s, find_row(s, "mail INBOX/0000000000000001: starr") >= 0 &&
             find_row(s, "ed: no -> yes [revertable]") >= 0,
          "the flag step shows what changed and its state");
    pty_close(s);
}

static void test_hist_cli_revert_page(void) {
    restart_mock();
    make_home("clirev");
    cli("list >/dev/null");
    cli("mark-starred 1 >/dev/null");
    PtySession *s = cli_open("revert", "1", NULL);
    ASSERT(s != NULL, "email-cli revert 1 runs");
    if (!s) return;
    CHECK(s, pty_wait_for(s, "Continue? [y/N]", WAIT_MS) == 0, "it asks first");
    CHECK(s, find_row(s, "Revert step 1") >= 0, "it names the step");
    pty_send_key(s, PTY_KEY_ENTER);   /* the default: No */
    CHECK(s, pty_wait_for(s, "Not done.", WAIT_MS) == 0, "Enter answers No");
    pty_close(s);
    s = cli_open("revert", "1", NULL);
    ASSERT(s != NULL, "email-cli revert 1 runs again");
    if (!s) return;
    CHECK(s, pty_wait_for(s, "Continue? [y/N]", WAIT_MS) == 0, "it asks again");
    pty_send_str(s, "y");
    pty_send_key(s, PTY_KEY_ENTER);
    CHECK(s, pty_wait_for(s, "Step 1 reverted.", WAIT_MS) == 0, "y reverts the step");
    pty_close(s);
    s = cli_open("history", NULL, NULL);
    ASSERT(s != NULL, "history after the revert");
    if (!s) return;
    CHECK(s, pty_wait_for(s, "revert of 1", WAIT_MS) == 0, "the revert is a step of its own");
    pty_settle(s, 300);   /* the older step is printed after the newer one */
    CHECK(s, find_row(s, "reverted by 2") >= 0, "the first step says it was reverted");
    pty_close(s);
}

static void test_hist_cli_irreversible_confirms(void) {
    restart_mock();
    make_home("cliirr");
    cli("list >/dev/null");
    const char *args[] = { g_cli_bin, "hist@example.org", "trash", "3", "--folder", "Trash", NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "a terminal");
    if (!s) return;
    pty_setenv(s, "TZ", "UTC");
    pty_setenv(s, "TERM", "xterm");
    pty_setenv(s, "HOME", g_home);
    pty_setenv(s, "XDG_CONFIG_HOME", NULL);
    pty_setenv(s, "XDG_DATA_HOME", NULL);
    pty_setenv(s, "XDG_CACHE_HOME", NULL);
    ASSERT(pty_run(s, args) == 0, "email-cli trash runs");
    CHECK(s, pty_wait_for(s, "not revertable. Continue? [y/N]", WAIT_MS) == 0,
          "a permanent delete confirms and says it is not revertable");
    pty_send_key(s, PTY_KEY_ENTER);
    CHECK(s, pty_wait_for(s, "Not done.", WAIT_MS) == 0, "the default is No");
    pty_close(s);
}

int main(int argc, char *argv[]) {
    printf("--- email-tui and email-cli history (80x24) ---\n\n");
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <email-tui> <mock-imap-server> <email-cli>\n", argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(g_tui_bin, sizeof(g_tui_bin), "%s", argv[1]);
    snprintf(g_mock_bin, sizeof(g_mock_bin), "%s", argv[2]);
    snprintf(g_cli_bin, sizeof(g_cli_bin), "%s", argv[3]);
    snprintf(g_home, sizeof(g_home), "%s", TP("home"));
    test_set_home(g_home);
    mkdir(g_home, 0700);
    setenv("TZ", "UTC", 1);
    setenv("MOCK_IMAP_COUNT", "5", 1);
    restart_mock();
    if (g_mock_pid < 0) {
        fprintf(stderr, "FATAL: cannot start the mock server\n");
        return EXIT_FAILURE;
    }

    RUN_TEST(test_hist_tui_recent);
    RUN_TEST(test_hist_tui_help_empty);
    RUN_TEST(test_hist_tui_not_revertable);
    RUN_TEST(test_hist_tui_confirm_default_no);
    RUN_TEST(test_hist_tui_overtaken);
    RUN_TEST(test_hist_tui_revert_and_revert_again);
    RUN_TEST(test_hist_tui_reader);
    RUN_TEST(test_hist_cli_history_page);
    RUN_TEST(test_hist_cli_revert_page);
    RUN_TEST(test_hist_cli_irreversible_confirms);

    if (g_mock_pid > 0) { kill(g_mock_pid, SIGKILL); waitpid(g_mock_pid, NULL, 0); }
    printf("\n--- History PTY results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
