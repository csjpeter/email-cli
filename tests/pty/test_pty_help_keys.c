/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-20 */
/* PTY key study of the help overlay (?) in every view and of the status hints. */
/**
 * @file test_pty_help_keys.c
 * @brief The help overlay of every view of email-tui at 80x24 (EMAIL-153,
 *        study EMAIL-140): `?` opens it, Esc / q / Backspace close it, the
 *        movement keys scroll it, any other key changes nothing, and the
 *        status line names the key.  Table: docs/spec/tui-keys.md, section
 *        "Help overlays".
 *
 * Usage: test-pty-help-keys <email-tui> <mock-imap-server>
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ptytest.h"
#include "pty_assert.h"
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
#define STATUS_ROW (ROWS - 1)
#define WAIT_MS 6000
/* A positive screen assertion waits for the text (EMAIL-224): under ASan or
 * Valgrind the program redraws slowly, and a read in between sees half a screen. */
#undef ASSERT_SCREEN_CONTAINS
#define ASSERT_SCREEN_CONTAINS(s, text) ASSERT_WAIT_FOR(s, text, WAIT_MS)
#define SETTLE_MS 400

static pid_t g_mock_pid = -1;
static int   g_imap_port = 0;
static char  g_home[256];
static char  g_tui_bin[512];
static char  g_mock_bin[512];
static int   g_seq = 0;

/* ── Fixture ─────────────────────────────────────────────────────────── */

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
    snprintf(dir, sizeof(dir), "%s/.config/email-cli/accounts/testuser", g_home); mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", dir);
    FILE *fp = fopen(path, "w");
    if (!fp) { g_tests_failed++; return; }
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:%d\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SSL_NO_VERIFY=1\n", g_imap_port);
    fclose(fp);
    chmod(path, 0600);
}

static void rules_path(char *path, size_t sz) {
    snprintf(path, sz, "%s/.config/email-cli/accounts/testuser/rules.ini", g_home);
}

/* n rules R01..Rnn; R01 has three conditions, the others one. */
static void write_rules_n(int n) {
    char path[512];
    rules_path(path, sizeof(path));
    if (n == 0) { unlink(path); return; }
    FILE *fp = fopen(path, "w");
    if (!fp) { g_tests_failed++; return; }
    for (int i = 1; i <= n; i++) {
        if (i == 1)
            fprintf(fp, "[rule \"R%02d\"]\nwhen = from:*@one.example.org or "
                        "subject:\"*two*\" or to:*@three.example.org\n"
                        "then-add-label = L%02d\n\n", i, i);
        else
            fprintf(fp, "[rule \"R%02d\"]\nwhen = from:*@r%02d.example.org\n"
                        "then-add-label = L%02d\n\n", i, i, i);
    }
    fclose(fp);
}

/* ── Opening a view ──────────────────────────────────────────────────── */

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

/* The message list of the mock's INBOX, then `l`: the rules list. */
static PtySession *at_rules(int n) {
    restart_mock();
    make_home("r");
    write_rules_n(n);
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
    pty_send_str(s, "l");
    if (pty_wait_for(s, "Rules for", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* The rules list of n rules, then Enter on the first: the rule's detail. */
static PtySession *at_detail(int n) {
    PtySession *s = at_rules(n);
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Rule: R01", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* The condition editor of the new rule (from the list) or of R01 (detail). */
static PtySession *at_conditions(int n, int from_detail) {
    PtySession *s = from_detail ? at_detail(n) : at_rules(n);
    if (!s) return NULL;
    pty_send_str(s, from_detail ? "e" : "a");
    if (pty_wait_for(s, "conditions", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* ── Reading the screen ──────────────────────────────────────────────── */

static void press(PtySession *s, const char *k) {
    if (strcmp(k, "Enter") == 0)      pty_send_key(s, PTY_KEY_ENTER);
    else if (strcmp(k, "Esc") == 0)   pty_send_key(s, PTY_KEY_ESC);
    else if (strcmp(k, "BS") == 0)    pty_send_key(s, PTY_KEY_BACK);
    else if (strcmp(k, "Left") == 0)  pty_send_key(s, PTY_KEY_LEFT);
    else if (strcmp(k, "Right") == 0) pty_send_key(s, PTY_KEY_RIGHT);
    else if (strcmp(k, "Down") == 0)  pty_send_key(s, PTY_KEY_DOWN);
    else if (strcmp(k, "Up") == 0)    pty_send_key(s, PTY_KEY_UP);
    else if (strcmp(k, "Home") == 0)  pty_send_key(s, PTY_KEY_HOME);
    else if (strcmp(k, "End") == 0)   pty_send_key(s, PTY_KEY_END);
    else if (strcmp(k, "PgUp") == 0)  pty_send_key(s, PTY_KEY_PGUP);
    else if (strcmp(k, "PgDn") == 0)  pty_send_key(s, PTY_KEY_PGDN);
    else if (strcmp(k, "Tab") == 0)   pty_send_key(s, PTY_KEY_TAB);
    else if (strcmp(k, "Del") == 0)   pty_send_key(s, PTY_KEY_DELETE);
    else if (strcmp(k, "Ins") == 0)   pty_send_key(s, PTY_KEY_INSERT);
    else if (strcmp(k, "F1") == 0)    pty_send_key(s, PTY_KEY_F1);
    else if (strcmp(k, "^C") == 0)    pty_send_key(s, PTY_KEY_CTRL_C);
    else                              pty_send_str(s, k);
    pty_settle(s, 250);
}

#define CHECK(s, cond, ...) do { \
    char _w[200]; snprintf(_w, sizeof(_w), __VA_ARGS__); \
    if (!(cond)) pty_screen_dump((s), stdout); \
    ASSERT((cond), _w); \
} while (0)

/* ── The other views ─────────────────────────────────────────────────── */

static PtySession *at_accounts(void) {
    restart_mock();
    make_home("a");
    PtySession *s = tui_open();
    if (!s) return NULL;
    if (pty_wait_for(s, "Email Accounts", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static PtySession *at_folders(void) {
    PtySession *s = at_accounts();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static PtySession *at_list(void) {
    PtySession *s = at_folders();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "message(s) in", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static PtySession *at_reader(void) {
    PtySession *s = at_list();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "From:", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* ── The overlay ─────────────────────────────────────────────────────── */

/*
 * The help of the view on screen: `?` opens it (title, a row of the table, the
 * view still behind it), each of Esc / q / Backspace closes it and leaves the
 * view as it was, and keys that are no part of the overlay (a letter, `h`,
 * Enter, Tab, F1, `?` again, Left, Right) change nothing: the overlay stays and
 * the view behind it does not move.
 */
static void help_cycle(PtySession *s, const char *title, const char *row,
                       const char *behind, const char *desc) {
    const char *closers[] = { "Esc", "q", "BS", NULL };
    const char *inert[]   = { "x", "h", "Enter", "Tab", "F1", "?", "Left", "Right", "Del", NULL };
    for (int i = 0; closers[i]; i++) {
        press(s, "?");
        CHECK(s, pty_wait_for(s, title, WAIT_MS) == 0, "%s: ? opens '%s'", closers[i], title);
        CHECK(s, pty_screen_contains(s, row), "%s: the help lists '%s'", title, row);
        CHECK(s, pty_screen_contains(s, desc), "%s: the help opens with the description '%s'",
              title, desc);
        CHECK(s, pty_screen_contains(s, behind), "%s: the view stays behind", title);
        for (int k = 0; inert[k]; k++) {
            press(s, inert[k]);
            CHECK(s, pty_screen_contains(s, title) && pty_screen_contains(s, row),
                  "%s: %s does not close or change the help", title, inert[k]);
        }
        press(s, closers[i]);
        CHECK(s, !pty_screen_contains(s, title) && pty_screen_contains(s, behind),
              "%s: %s closes the help, the view is as before", title, closers[i]);
    }
}

/* PgDn End Down Up Home PgUp j k on an open overlay: no crash, no close. */
static void help_scroll(PtySession *s, const char *title) {
    const char *keys[] = { "PgDn", "End", "Down", "Up", "Home", "PgUp", "j", "k", NULL };
    press(s, "?");
    CHECK(s, pty_wait_for(s, title, WAIT_MS) == 0, "%s: opens for scrolling", title);
    for (int i = 0; keys[i]; i++) {
        press(s, keys[i]);
        CHECK(s, pty_screen_contains(s, title), "%s: %s keeps the overlay open", title, keys[i]);
    }
    press(s, "Esc");
    CHECK(s, !pty_screen_contains(s, title), "%s: Esc closes after scrolling", title);
}

/* EMAIL-183: the help lists Backspace (with the Back entry; on the top level
 * it says it does nothing).  A long help is looked through with PgDn. */
static void help_lists_backspace(PtySession *s, const char *title) {
    press(s, "?");
    CHECK(s, pty_wait_for(s, title, WAIT_MS) == 0, "%s: opens for the Backspace row", title);
    int found = pty_screen_contains(s, "Backspace");
    for (int i = 0; !found && i < 6; i++) {
        press(s, "PgDn");
        found = pty_screen_contains(s, "Backspace");
    }
    CHECK(s, found, "%s: the help lists Backspace", title);
    press(s, "Esc");
}

static void test_hk_accounts(void) {
    PtySession *s = at_accounts();
    ASSERT(s != NULL, "accounts help: opens");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "? help"), "status line names ?");
    help_cycle(s, "Accounts shortcuts", "Add new account", "Email Accounts",
               "The accounts of the program.");
    help_scroll(s, "Accounts shortcuts");
    help_lists_backspace(s, "Accounts shortcuts");
    pty_close(s);
}

static void test_hk_folders(void) {
    PtySession *s = at_folders();
    ASSERT(s != NULL, "folders help: opens");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "? help"), "status line names ?");
    help_cycle(s, "Folder browser shortcuts", "Actions", "Folders",
               "The folders of the IMAP account");
    help_scroll(s, "Folder browser shortcuts");
    help_lists_backspace(s, "Folder browser shortcuts");
    pty_close(s);
}

static void test_hk_list(void) {
    PtySession *s = at_list();
    ASSERT(s != NULL, "list help: opens");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "? help"), "status line names ?");
    help_cycle(s, "Message list shortcuts", "Compose", "message(s) in",
               "The messages of one folder");
    help_scroll(s, "Message list shortcuts");
    help_lists_backspace(s, "Message list shortcuts");
    /* The list help is longer than the 80x24 screen: End shows its last rows,
     * Home the first ones again. */
    press(s, "?");
    CHECK(s, pty_wait_for(s, "Message list shortcuts", WAIT_MS) == 0, "long help opens");
    press(s, "End");
    CHECK(s, pty_screen_contains(s, "Help") && !pty_screen_contains(s, "move the cursor down"),
          "End scrolls the long help to its last rows");
    press(s, "Home");
    CHECK(s, pty_screen_contains(s, "move the cursor down"), "Home scrolls back to the first rows");
    press(s, "Esc");
    pty_close(s);
}

static void test_hk_reader(void) {
    PtySession *s = at_reader();
    ASSERT(s != NULL, "reader help: opens");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "? help"), "status line names ?");
    help_cycle(s, "Message reader shortcuts", "Down", "From:",
               "One message, rendered as text.");
    help_scroll(s, "Message reader shortcuts");
    help_lists_backspace(s, "Message reader shortcuts");
    pty_close(s);
}

static void test_hk_rules_list(void) {
    for (int n = 0; n <= 1; n++) {          /* 0 and 1 rules */
        PtySession *s = at_rules(n);
        ASSERT(s != NULL, "rules help: opens");
        CHECK(s, pty_row_contains(s, STATUS_ROW, "? help"), "status line names ?");
        help_cycle(s, "Rules shortcuts", "Add a rule", "Rules for",
               "The mail rules that sort");
        help_scroll(s, "Rules shortcuts");
        pty_close(s);
    }
}

static void test_hk_rule_detail(void) {
    PtySession *s = at_detail(1);
    ASSERT(s != NULL, "detail help: opens");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "? help"), "status line names ?");
    help_cycle(s, "Rule shortcuts", "Edit the rule", "Rule: R01",
               "One mail rule: its conditions");
    help_scroll(s, "Rule shortcuts");
    pty_close(s);
}

static void test_hk_conditions(void) {
    PtySession *s = at_conditions(1, 1);
    ASSERT(s != NULL, "conditions help: opens");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "? help"), "status line names ?");
    help_cycle(s, "Conditions shortcuts", "Cancel the rule", "(step 1/2)",
               "The conditions of the rule being");
    help_scroll(s, "Conditions shortcuts");
    pty_close(s);
}

/* The keys that close a view are not help keys: h is "back" in a list. */
static void test_hk_h_is_not_help(void) {
    PtySession *s = at_list();
    ASSERT(s != NULL, "h: list opens");
    press(s, "h");
    CHECK(s, !pty_screen_contains(s, "shortcuts") && pty_screen_contains(s, "Folders"),
          "h in the list goes back, it shows no help");
    pty_close(s);
}

int main(int argc, char *argv[]) {
    printf("--- email-tui help overlays (80x24) ---\n\n");
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <email-tui> <mock-imap-server>\n", argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(g_tui_bin, sizeof(g_tui_bin), "%s", argv[1]);
    snprintf(g_mock_bin, sizeof(g_mock_bin), "%s", argv[2]);
    snprintf(g_home, sizeof(g_home), "%s", TP("home"));
    test_set_home(g_home);
    mkdir(g_home, 0700);
    setenv("TZ", "UTC", 1);
    setenv("MOCK_IMAP_COUNT", "3", 1);
    restart_mock();
    if (g_mock_pid < 0) {
        fprintf(stderr, "FATAL: cannot start the mock server\n");
        return EXIT_FAILURE;
    }

    RUN_TEST(test_hk_accounts);
    RUN_TEST(test_hk_folders);
    RUN_TEST(test_hk_list);
    RUN_TEST(test_hk_reader);
    RUN_TEST(test_hk_rules_list);
    RUN_TEST(test_hk_rule_detail);
    RUN_TEST(test_hk_conditions);
    RUN_TEST(test_hk_h_is_not_help);

    if (g_mock_pid > 0) { kill(g_mock_pid, SIGKILL); waitpid(g_mock_pid, NULL, 0); }
    printf("\n--- Help overlay study results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
