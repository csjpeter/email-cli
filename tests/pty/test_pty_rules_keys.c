/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-20 */
/* PTY key study of the rules list, the rule detail and the condition editor. */
/**
 * @file test_pty_rules_keys.c
 * @brief Every key of the rules editor of email-tui at 80x24 (EMAIL-148).
 *
 * The rules list (`l` in the message list), the rule detail (`Enter`) and the
 * condition editor (`a`, or `e` in the detail) are driven with the keys a user
 * presses, with 0, 1 and many rules.  The table of what each key does is in
 * docs/spec/tui-keys.md, section "Rules list, rule detail and condition
 * editor"; the test names there are the functions below.
 *
 * Usage: test-pty-rules-keys <email-tui> <mock-imap-server>
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

/* The number of rules in the file. */
static int rules_in_file(void) {
    char path[512], line[1024];
    rules_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int n = 0;
    while (fgets(line, sizeof(line), fp)) if (strncmp(line, "[rule ", 6) == 0) n++;
    fclose(fp);
    return n;
}

static int file_has(const char *needle) {
    char path[512], line[1024];
    rules_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int found = 0;
    while (fgets(line, sizeof(line), fp)) if (strstr(line, needle)) found = 1;
    fclose(fp);
    return found;
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

/* The text of the row the selection marker '>' is on ("" when none). */
static const char *cursor_text(PtySession *s) {
    static char buf[256];
    for (int r = 1; r < STATUS_ROW; r++) {
        pty_row_text(s, r, buf, sizeof(buf));
        if (buf[0] == '>') return buf;
    }
    buf[0] = '\0';
    return buf;
}

static int cursor_has(PtySession *s, const char *needle) {
    return strstr(cursor_text(s), needle) != NULL;
}

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

/* ── The rules list ──────────────────────────────────────────────────── */

/** j k Down Up Home End g G PgUp PgDn on 30 rules; they stop at the ends. */
static void test_rk_list_navigation(void) {
    PtySession *s = at_rules(30);
    ASSERT(s != NULL, "rules list nav: opens");
    CHECK(s, pty_screen_contains(s, "(30 rules)"), "title counts 30 rules");
    CHECK(s, cursor_has(s, "R01"), "cursor starts on the first rule");
    press(s, "k"); press(s, "Up");
    CHECK(s, cursor_has(s, "R01"), "k and Up stop at the first rule");
    press(s, "j");
    CHECK(s, cursor_has(s, "R02"), "j moves down");
    press(s, "Down");
    CHECK(s, cursor_has(s, "R03"), "Down moves down");
    press(s, "k");
    CHECK(s, cursor_has(s, "R02"), "k moves up");
    press(s, "Up");
    CHECK(s, cursor_has(s, "R01"), "Up moves up");
    press(s, "End");
    CHECK(s, cursor_has(s, "R30"), "End goes to the last rule");
    press(s, "j"); press(s, "Down");
    CHECK(s, cursor_has(s, "R30"), "j and Down stop at the last rule");
    press(s, "Home");
    CHECK(s, cursor_has(s, "R01"), "Home goes to the first rule");
    press(s, "G");
    CHECK(s, cursor_has(s, "R30"), "G goes to the last rule");
    press(s, "g");
    CHECK(s, cursor_has(s, "R01"), "g goes to the first rule");
    press(s, "PgDn");
    CHECK(s, cursor_has(s, "R21"), "PgDn moves a page of 20 rows");
    press(s, "PgDn");
    CHECK(s, cursor_has(s, "R30"), "PgDn stops at the last rule");
    press(s, "PgUp");
    CHECK(s, cursor_has(s, "R10"), "PgUp moves a page back");
    press(s, "PgUp"); press(s, "PgUp");
    CHECK(s, cursor_has(s, "R01"), "PgUp stops at the first rule");
    pty_close(s);
}

/** 0 rules: the empty message; 1 rule: nothing to move to. */
static void test_rk_list_zero_and_one(void) {
    PtySession *s = at_rules(0);
    ASSERT(s != NULL, "rules list 0: opens");
    CHECK(s, pty_screen_contains(s, "(0 rules)"), "0 rules: title");
    CHECK(s, pty_screen_contains(s, "(no rules -- press 'a' to add one)"), "0 rules: the empty message");
    const char *keys[] = { "j", "k", "Down", "Up", "Home", "End", "PgUp", "PgDn", "g", "G",
                           "Enter", "d", "e", NULL };
    for (int i = 0; keys[i]; i++) {
        press(s, keys[i]);
        CHECK(s, pty_screen_contains(s, "(0 rules)") && !pty_screen_contains(s, "(y/N)")
                 && !pty_screen_contains(s, "Rule:"),
              "0 rules: %s changes nothing", keys[i]);
    }
    press(s, "p");
    CHECK(s, pty_wait_for(s, "No rules found", WAIT_MS) == 0, "0 rules: p previews (no rules found)");
    press(s, "x");
    CHECK(s, pty_wait_for(s, "(0 rules)", WAIT_MS) == 0, "0 rules: any key leaves the preview");
    pty_close(s);

    s = at_rules(1);
    ASSERT(s != NULL, "rules list 1: opens");
    CHECK(s, pty_screen_contains(s, "(1 rule)"), "1 rule: title is singular");
    const char *k1[] = { "j", "k", "Down", "Up", "Home", "End", "PgUp", "PgDn", "g", "G", NULL };
    for (int i = 0; k1[i]; i++) {
        press(s, k1[i]);
        CHECK(s, cursor_has(s, "R01"), "1 rule: %s keeps the cursor on the rule", k1[i]);
    }
    pty_close(s);
}

/** Esc, q, Backspace, Left, h go back to the message list; ctrl-c quits. */
static void test_rk_list_back_keys(void) {
    const char *keys[] = { "Esc", "q", "BS", "Left", "h", NULL };
    for (int i = 0; keys[i]; i++) {
        PtySession *s = at_rules(2);
        ASSERT(s != NULL, "rules list back: opens");
        CHECK(s, pty_row_contains(s, STATUS_ROW, "Esc/q/BS back"), "status line names Esc/q/BS back");
        press(s, keys[i]);
        CHECK(s, pty_wait_for(s, "message(s) in", WAIT_MS) == 0 && !pty_screen_contains(s, "Rules for"),
              "rules list: %s goes back to the message list", keys[i]);
        pty_close(s);
    }
    PtySession *s = at_rules(2);
    ASSERT(s != NULL, "rules list ctrl-c: opens");
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT_EXIT_STATUS(s, 0, 5000);
    pty_close(s);
}

/** Keys with no meaning change nothing and show nothing. */
static void test_rk_list_unbound_keys(void) {
    PtySession *s = at_rules(3);
    ASSERT(s != NULL, "rules list unbound: opens");
    const char *keys[] = { " ", "x", "c", "r", "e", "l", "t", "D", "Z", "u", "U", "1", "-", "/", ":",
                           "Tab", "Right", "Del", "Ins", "F1", NULL };
    for (int i = 0; keys[i]; i++) {
        press(s, keys[i]);
        CHECK(s, pty_screen_contains(s, "(3 rules)") && cursor_has(s, "R01")
                 && pty_row_contains(s, STATUS_ROW, "Esc/q/BS back") && !pty_screen_contains(s, "(y/N)"),
              "rules list: %s changes nothing", keys[i]);
    }
    pty_close(s);
}

/** The status line at 80x24 and the help overlay. */
static void test_rk_list_help_and_status(void) {
    PtySession *s = at_rules(2);
    ASSERT(s != NULL, "rules list help: opens");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "Enter open") && pty_row_contains(s, STATUS_ROW, "a add")
             && pty_row_contains(s, STATUS_ROW, "d delete") && pty_row_contains(s, STATUS_ROW, "? help"),
          "status line shows the keys");
    const char *closers[] = { "Esc", "q", "BS", NULL };
    for (int i = 0; closers[i]; i++) {
        press(s, "?");
        CHECK(s, pty_wait_for(s, "Rules shortcuts", WAIT_MS) == 0, "? opens the help");
        CHECK(s, pty_screen_contains(s, "Add a rule") && pty_screen_contains(s, "also g / G")
                 && pty_screen_contains(s, "Apply the rules now") && pty_screen_contains(s, "(2 rules)"),
              "the help lists the keys and the list stays behind it");
        press(s, closers[i]);
        CHECK(s, !pty_screen_contains(s, "Rules shortcuts") && pty_screen_contains(s, "(2 rules)"),
              "%s closes the help and stays in the list", closers[i]);
    }
    pty_close(s);
}

/** d asks first, naming the rule; No is the default; y deletes. */
static void test_rk_list_delete(void) {
    const char *no[] = { "n", "Esc", "q", "BS", "x", "Enter", NULL };
    for (int i = 0; no[i]; i++) {
        PtySession *s = at_rules(3);
        ASSERT(s != NULL, "rules list delete: opens");
        press(s, "d");
        CHECK(s, pty_wait_for(s, "Delete \"R01\"? (y/N)", WAIT_MS) == 0, "d asks and names the rule");
        press(s, no[i]);
        CHECK(s, pty_screen_contains(s, "(3 rules)") && rules_in_file() == 3,
              "rules list delete: %s refuses", no[i]);
        pty_close(s);
    }
    PtySession *s = at_rules(3);
    ASSERT(s != NULL, "rules list delete yes: opens");
    press(s, "j");
    press(s, "d");
    CHECK(s, pty_wait_for(s, "Delete \"R02\"? (y/N)", WAIT_MS) == 0, "d names the selected rule");
    press(s, "y");
    CHECK(s, pty_wait_for(s, "(2 rules)", WAIT_MS) == 0 && rules_in_file() == 2 && !file_has("R02"),
          "y deletes the rule");
    CHECK(s, cursor_has(s, "R03"), "the cursor stays on the row, now R03");
    press(s, "End"); press(s, "d"); press(s, "y");
    CHECK(s, pty_wait_for(s, "(1 rule)", WAIT_MS) == 0 && cursor_has(s, "R01"),
          "deleting the last row moves the cursor up");
    press(s, "d"); press(s, "y");
    CHECK(s, pty_wait_for(s, "(0 rules)", WAIT_MS) == 0 && rules_in_file() == 0,
          "deleting the only rule leaves the empty list");
    pty_close(s);

    s = at_rules(3);
    ASSERT(s != NULL, "rules list delete ctrl-c: opens");
    press(s, "d");
    pty_wait_for(s, "(y/N)", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT_EXIT_STATUS(s, 0, 5000);
    CHECK(s, rules_in_file() == 3, "ctrl-c at the question deletes nothing");
    pty_close(s);
}

/** p previews, A asks and applies; any key leaves the report; ctrl-c quits. */
static void test_rk_list_preview_apply(void) {
    PtySession *s = at_rules(2);
    ASSERT(s != NULL, "rules list p: opens");
    press(s, "p");
    CHECK(s, pty_wait_for(s, "Rules preview (dry run, nothing changed)", WAIT_MS) == 0
             && pty_row_contains(s, STATUS_ROW, "any key back"), "p shows the dry run");
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "(2 rules)", WAIT_MS) == 0, "a key leaves the report");
    press(s, "A");
    CHECK(s, pty_wait_for(s, "Apply the rules to the cached messages now? (y/N)", WAIT_MS) == 0, "A asks first");
    press(s, "n");
    CHECK(s, pty_screen_contains(s, "(2 rules)") && !pty_screen_contains(s, "Rules applied"), "n refuses");
    press(s, "A"); press(s, "y");
    CHECK(s, pty_wait_for(s, "Rules applied:", WAIT_MS) == 0, "y applies and reports");
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT_EXIT_STATUS(s, 0, 5000);
    pty_close(s);
}

/** a opens the condition editor of a new rule; Enter opens the detail. */
static void test_rk_list_open_keys(void) {
    PtySession *s = at_rules(3);
    ASSERT(s != NULL, "rules list open: opens");
    press(s, "j");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Rule: R02", WAIT_MS) == 0, "Enter opens the selected rule");
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "Rules for", WAIT_MS) == 0 && cursor_has(s, "R02"),
          "the way back keeps the cursor row");
    press(s, "a");
    CHECK(s, pty_wait_for(s, "Add new rule for testuser", WAIT_MS) == 0, "a opens the new-rule form");
    pty_close(s);
}

/* ── The rule detail ─────────────────────────────────────────────────── */

static void test_rk_detail_view(void) {
    PtySession *s = at_detail(1);
    ASSERT(s != NULL, "detail: opens");
    CHECK(s, pty_screen_contains(s, "Account: testuser") && pty_screen_contains(s, "when: from:*@one.example.org")
             && pty_screen_contains(s, "then-add-label:   L01"), "detail shows the rule");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "e edit") && pty_row_contains(s, STATUS_ROW, "d delete")
             && pty_row_contains(s, STATUS_ROW, "Esc/q/BS back"), "status line shows e, d and the back keys");
    const char *keys[] = { "j", "k", "Down", "Up", "Home", "End", "PgUp", "PgDn", "g", "G", "a", "p", "A",
                           " ", "x", "Tab", "Right", "Del", "Ins", "F1", "Enter", NULL };
    for (int i = 0; keys[i]; i++) {
        press(s, keys[i]);
        CHECK(s, pty_screen_contains(s, "Rule: R01") && !pty_screen_contains(s, "(y/N)"),
              "detail: %s changes nothing", keys[i]);
    }
    press(s, "?");
    CHECK(s, pty_wait_for(s, "Rule shortcuts", WAIT_MS) == 0 && pty_screen_contains(s, "Edit the rule"),
          "? opens the help");
    press(s, "q");
    CHECK(s, !pty_screen_contains(s, "Rule shortcuts") && pty_screen_contains(s, "Rule: R01"),
          "q closes the help only");
    pty_close(s);
}

static void test_rk_detail_back_keys(void) {
    const char *keys[] = { "Esc", "q", "BS", "Left", "h", NULL };
    for (int i = 0; keys[i]; i++) {
        PtySession *s = at_detail(2);
        ASSERT(s != NULL, "detail back: opens");
        press(s, keys[i]);
        CHECK(s, pty_wait_for(s, "Rules for", WAIT_MS) == 0 && !pty_screen_contains(s, "Rule: R01")
                 && cursor_has(s, "R01"), "detail: %s goes back to the list", keys[i]);
        pty_close(s);
    }
    PtySession *s = at_detail(2);
    ASSERT(s != NULL, "detail ctrl-c: opens");
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT_EXIT_STATUS(s, 0, 5000);
    pty_close(s);
}

static void test_rk_detail_delete(void) {
    const char *no[] = { "n", "Esc", "q", "BS", "x", NULL };
    for (int i = 0; no[i]; i++) {
        PtySession *s = at_detail(2);
        ASSERT(s != NULL, "detail delete: opens");
        press(s, "d");
        CHECK(s, pty_wait_for(s, "Delete \"R01\"? (y/N)", WAIT_MS) == 0, "d asks and names the rule");
        press(s, no[i]);
        CHECK(s, pty_wait_for(s, "Rule: R01", WAIT_MS) == 0 && rules_in_file() == 2,
              "detail delete: %s refuses and stays in the detail", no[i]);
        pty_close(s);
    }
    PtySession *s = at_detail(2);
    ASSERT(s != NULL, "detail delete yes: opens");
    press(s, "d"); press(s, "y");
    CHECK(s, pty_wait_for(s, "(1 rule)", WAIT_MS) == 0 && rules_in_file() == 1 && !file_has("R01"),
          "y deletes the rule and returns to the list");
    pty_close(s);

    s = at_detail(2);
    ASSERT(s != NULL, "detail delete ctrl-c: opens");
    press(s, "d");
    pty_wait_for(s, "(y/N)", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT_EXIT_STATUS(s, 0, 5000);
    CHECK(s, rules_in_file() == 2, "ctrl-c at the question deletes nothing");
    pty_close(s);

    s = at_detail(1);
    ASSERT(s != NULL, "detail delete only: opens");
    press(s, "d"); press(s, "y");
    CHECK(s, pty_wait_for(s, "(0 rules)", WAIT_MS) == 0 && rules_in_file() == 0,
          "deleting the only rule leaves the empty list");
    pty_close(s);
}

/* ── The condition editor ────────────────────────────────────────────── */

/** j k Down Up Home End g G PgUp PgDn over the 3 conditions of R01. */
static void test_rk_cond_navigation(void) {
    PtySession *s = at_conditions(1, 1);
    ASSERT(s != NULL, "conditions nav: opens");
    CHECK(s, pty_screen_contains(s, "Edit rule \"R01\" for testuser") && pty_screen_contains(s, "(step 1/2)"),
          "title names the rule and the step");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "Enter/e edit") && pty_row_contains(s, STATUS_ROW, "a add")
             && pty_row_contains(s, STATUS_ROW, "d delete") && pty_row_contains(s, STATUS_ROW, "Tab next")
             && pty_row_contains(s, STATUS_ROW, "Esc/q cancel"), "status line shows the keys");
    CHECK(s, cursor_has(s, "1. from:*@one"), "cursor starts on the first condition");
    press(s, "k"); press(s, "Up");
    CHECK(s, cursor_has(s, "1. from:"), "k and Up stop at the first");
    press(s, "j");
    CHECK(s, cursor_has(s, "2. subject:"), "j moves down");
    press(s, "Down");
    CHECK(s, cursor_has(s, "3. to:"), "Down moves down");
    press(s, "j"); press(s, "Down");
    CHECK(s, cursor_has(s, "3. to:"), "j and Down stop at the last");
    press(s, "k");
    CHECK(s, cursor_has(s, "2. subject:"), "k moves up");
    press(s, "Home");
    CHECK(s, cursor_has(s, "1. from:"), "Home goes to the first");
    press(s, "End");
    CHECK(s, cursor_has(s, "3. to:"), "End goes to the last");
    press(s, "g");
    CHECK(s, cursor_has(s, "1. from:"), "g goes to the first");
    press(s, "G");
    CHECK(s, cursor_has(s, "3. to:"), "G goes to the last");
    press(s, "PgUp");
    CHECK(s, cursor_has(s, "1. from:"), "PgUp goes to the first");
    press(s, "PgDn");
    CHECK(s, cursor_has(s, "3. to:"), "PgDn goes to the last");
    pty_close(s);
}

/** An empty list (0 conditions) and a list of one. */
static void test_rk_cond_zero_and_one(void) {
    PtySession *s = at_conditions(0, 0);
    ASSERT(s != NULL, "conditions 0: opens");
    CHECK(s, pty_screen_contains(s, "(empty") && pty_screen_contains(s, "press 'a' to add a condition"),
          "0 conditions: the empty message");
    const char *keys[] = { "j", "k", "Down", "Up", "Home", "End", "PgUp", "PgDn", "g", "G", "d", "x", " ", NULL };
    for (int i = 0; keys[i]; i++) {
        press(s, keys[i]);
        CHECK(s, pty_screen_contains(s, "(empty") && pty_screen_contains(s, "conditions"),
              "0 conditions: %s changes nothing", keys[i]);
    }
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "new:", WAIT_MS) == 0, "Enter on the empty list opens the new-condition prompt");
    press(s, "Esc");
    pty_close(s);

    s = at_conditions(2, 0);
    ASSERT(s != NULL, "conditions 1: opens");
    pty_send_str(s, "a");
    pty_wait_for(s, "new:", WAIT_MS);
    pty_send_str(s, "from:*@only.example.org");
    press(s, "Enter");
    CHECK(s, cursor_has(s, "1. from:*@only.example.org"), "a adds the first condition and selects it");
    const char *k1[] = { "j", "k", "Down", "Up", "Home", "End", "PgUp", "PgDn", "g", "G", NULL };
    for (int i = 0; k1[i]; i++) {
        press(s, k1[i]);
        CHECK(s, cursor_has(s, "1. from:*@only"), "1 condition: %s keeps the cursor", k1[i]);
    }
    pty_close(s);
}

/** a adds, e and Enter edit, d deletes; an invalid condition is refused. */
static void test_rk_cond_edit_keys(void) {
    PtySession *s = at_conditions(1, 1);
    ASSERT(s != NULL, "conditions edit: opens");
    press(s, "a");
    CHECK(s, pty_wait_for(s, "new:", WAIT_MS) == 0, "a opens the prompt");
    pty_send_str(s, "subject:\"*four*\"");
    press(s, "Enter");
    CHECK(s, cursor_has(s, "4. subject:\"*four*\""), "the new condition is the 4th and selected");
    press(s, "e");
    CHECK(s, pty_wait_for(s, "edit:", WAIT_MS) == 0, "e opens the edit prompt on the condition");
    pty_send_str(s, "x");
    press(s, "Esc");
    CHECK(s, cursor_has(s, "4. subject:\"*four*\"") && !pty_screen_contains(s, "four*\"x"),
          "Esc in the prompt keeps the condition");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "edit:", WAIT_MS) == 0, "Enter opens the edit prompt too");
    press(s, "Esc");
    press(s, "a");
    pty_wait_for(s, "new:", WAIT_MS);
    pty_send_str(s, "(((");
    press(s, "Enter");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "Not a valid condition"), "an invalid condition is refused with a reason");
    CHECK(s, !pty_screen_contains(s, "5. ((("), "an invalid condition is not added");
    press(s, "G"); press(s, "d");
    CHECK(s, !pty_screen_contains(s, "four*") && cursor_has(s, "3. to:"), "d deletes the selected one");
    press(s, "Home"); press(s, "d");
    CHECK(s, !pty_screen_contains(s, "one.example.org") && cursor_has(s, "1. subject:"),
          "d on the first deletes it and the cursor stays");
    pty_close(s);
}

/** Esc, Backspace, Left, h cancel the rule; q is as Esc, Tab/Right go on; ctrl-c quits. */
static void test_rk_cond_leave_keys(void) {
    const char *cancel[] = { "Esc", "BS", "Left", "h", NULL };
    for (int i = 0; cancel[i]; i++) {
        PtySession *s = at_conditions(2, 0);
        ASSERT(s != NULL, "conditions cancel: opens");
        press(s, cancel[i]);
        CHECK(s, pty_wait_for(s, "(2 rules)", WAIT_MS) == 0 && rules_in_file() == 2,
              "conditions: %s cancels and returns to the list", cancel[i]);
        pty_close(s);
    }
    PtySession *s = at_conditions(2, 1);
    ASSERT(s != NULL, "conditions q: opens");
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "Discard the rule? (y/N)", WAIT_MS) == 0
             && pty_screen_contains(s, "(step 1/2)"), "Esc with conditions asks before discarding");
    press(s, "n");
    CHECK(s, pty_screen_contains(s, "(step 1/2)") && cursor_has(s, "1. from:"), "n keeps the rule form open");
    press(s, "BS");
    pty_wait_for(s, "Discard the rule? (y/N)", WAIT_MS);
    press(s, "y");
    CHECK(s, pty_wait_for(s, "Rule: R01", WAIT_MS) == 0 && rules_in_file() == 2,
          "y discards: back at the detail, rules file unchanged");
    press(s, "e");
    pty_wait_for(s, "conditions", WAIT_MS);
    press(s, "q");
    CHECK(s, pty_wait_for(s, "Discard the rule? (y/N)", WAIT_MS) == 0
             && !pty_screen_contains(s, "step 2/2"), "q is as Esc: asks before discarding, does not go on");
    press(s, "y");
    CHECK(s, pty_wait_for(s, "Rule: R01", WAIT_MS) == 0 && rules_in_file() == 2, "q then y goes back");
    press(s, "e");
    pty_wait_for(s, "conditions", WAIT_MS);
    press(s, "Right");
    CHECK(s, pty_wait_for(s, "step 2/2: actions", WAIT_MS) == 0, "Right goes on to the actions");
    press(s, "Esc");
    pty_close(s);

    s = at_conditions(0, 0);
    ASSERT(s != NULL, "conditions q empty: opens");
    press(s, "q");
    CHECK(s, pty_wait_for(s, "(0 rules)", WAIT_MS) == 0 && !pty_screen_contains(s, "step 2/2"),
          "q with no conditions goes back to the rules list");
    pty_close(s);

    s = at_conditions(0, 0);
    ASSERT(s != NULL, "conditions Tab: opens");
    press(s, "Tab");
    CHECK(s, pty_wait_for(s, "step 2/2: actions", WAIT_MS) == 0, "Tab goes on to the actions");
    press(s, "Esc");
    pty_close(s);

    s = at_conditions(2, 0);
    ASSERT(s != NULL, "conditions ctrl-c: opens");
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT_EXIT_STATUS(s, 0, 5000);
    pty_close(s);

    s = at_conditions(2, 0);
    ASSERT(s != NULL, "conditions prompt ctrl-c: opens");
    press(s, "a");
    pty_wait_for(s, "new:", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT_EXIT_STATUS(s, 0, 5000);
    pty_close(s);
}

static void test_rk_cond_help_and_unbound(void) {
    PtySession *s = at_conditions(1, 1);
    ASSERT(s != NULL, "conditions help: opens");
    const char *keys[] = { "x", " ", "r", "c", "l", "p", "A", "Del", "Ins", "F1", NULL };
    for (int i = 0; keys[i]; i++) {
        press(s, keys[i]);
        CHECK(s, pty_screen_contains(s, "(step 1/2)") && cursor_has(s, "1. from:")
                 && pty_screen_contains(s, "3. to:"), "conditions: %s changes nothing", keys[i]);
    }
    const char *closers[] = { "Esc", "q", "BS", NULL };
    for (int i = 0; closers[i]; i++) {
        press(s, "?");
        CHECK(s, pty_wait_for(s, "Conditions shortcuts", WAIT_MS) == 0 && pty_screen_contains(s, "Cancel the rule"),
              "? opens the help");
        press(s, closers[i]);
        CHECK(s, !pty_screen_contains(s, "Conditions shortcuts") && pty_screen_contains(s, "(step 1/2)"),
              "%s closes the help only", closers[i]);
    }
    pty_close(s);
}

/** Step 2: the fields, Save? and ctrl-c. */
static void test_rk_actions_form(void) {
    PtySession *s = at_conditions(0, 0);
    ASSERT(s != NULL, "actions: opens");
    press(s, "a");
    pty_wait_for(s, "new:", WAIT_MS);
    pty_send_str(s, "from:*@new.example.org");
    press(s, "Enter");
    press(s, "Tab");
    CHECK(s, pty_wait_for(s, "step 2/2: actions", WAIT_MS) == 0 && pty_row_contains(s, STATUS_ROW, "Enter next field")
             && pty_row_contains(s, STATUS_ROW, "Esc cancel"), "step 2 and its status line");
    pty_send_str(s, "Fresh");
    press(s, "Enter");
    for (int i = 0; i < 8; i++) press(s, "Enter");      /* the other 8 fields */
    CHECK(s, pty_wait_for(s, "Save? (y/N)", WAIT_MS) == 0, "after the last field it asks to save");
    press(s, "n");
    CHECK(s, pty_wait_for(s, "(0 rules)", WAIT_MS) == 0 && rules_in_file() == 0, "n saves nothing");
    press(s, "a"); pty_wait_for(s, "new:", WAIT_MS);
    pty_send_str(s, "from:*@new.example.org");
    press(s, "Enter"); press(s, "Tab");
    pty_wait_for(s, "step 2/2", WAIT_MS);
    pty_send_str(s, "Fresh");
    for (int i = 0; i < 9; i++) press(s, "Enter");
    pty_wait_for(s, "Save? (y/N)", WAIT_MS);
    press(s, "y");
    CHECK(s, pty_wait_for(s, "(1 rule)", WAIT_MS) == 0 && file_has("[rule \"Fresh\"]"), "y saves the rule");
    pty_close(s);

    s = at_conditions(0, 0);
    ASSERT(s != NULL, "actions empty name: opens");
    press(s, "Tab");
    pty_wait_for(s, "step 2/2", WAIT_MS);
    for (int i = 0; i < 9; i++) press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Rule name is required", WAIT_MS) == 0, "an empty name is refused");
    press(s, "x");
    CHECK(s, pty_wait_for(s, "(0 rules)", WAIT_MS) == 0 && rules_in_file() == 0, "nothing is saved");
    pty_close(s);

    s = at_conditions(0, 0);
    ASSERT(s != NULL, "actions esc: opens");
    press(s, "Tab");
    pty_wait_for(s, "step 2/2", WAIT_MS);
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "(0 rules)", WAIT_MS) == 0, "Esc in a field cancels the rule");
    pty_close(s);

    s = at_conditions(0, 0);
    ASSERT(s != NULL, "actions ctrl-c: opens");
    press(s, "Tab");
    pty_wait_for(s, "step 2/2", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT_EXIT_STATUS(s, 0, 5000);
    pty_close(s);

    s = at_conditions(0, 0);
    ASSERT(s != NULL, "actions save ctrl-c: opens");
    press(s, "Tab");
    pty_wait_for(s, "step 2/2", WAIT_MS);
    pty_send_str(s, "Zed");
    for (int i = 0; i < 9; i++) press(s, "Enter");
    pty_wait_for(s, "Save? (y/N)", WAIT_MS);
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT_EXIT_STATUS(s, 0, 5000);
    CHECK(s, rules_in_file() == 0, "ctrl-c at Save? saves nothing");
    pty_close(s);
}

/* The step-2 form of a new rule (name given) or of the edit of R01. */
static PtySession *at_form(int edit) {
    PtySession *s = at_conditions(edit ? 1 : 0, edit);
    if (!s) return NULL;
    if (!edit) {
        press(s, "a");
        pty_wait_for(s, "new:", WAIT_MS);
        pty_send_str(s, "from:*@new.example.org");
        press(s, "Enter");
    }
    press(s, "Tab");
    if (pty_wait_for(s, "step 2/2: actions", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/** The form's layout, the prefilled edit, and keeping every field with Enter. */
static void test_rk_form_layout_and_edit(void) {
    PtySession *s = at_form(0);
    ASSERT(s != NULL, "form: opens");
    CHECK(s, pty_screen_contains(s, "Add new rule for testuser") && pty_screen_contains(s, "Name:")
             && pty_screen_contains(s, "add-label [1]:") && pty_screen_contains(s, "rm-label  [3]:")
             && pty_screen_contains(s, "then-move-folder:") && pty_screen_contains(s, "then-forward-to:"),
          "the add form lists the nine fields");
    pty_close(s);

    s = at_form(1);
    ASSERT(s != NULL, "form edit: opens");
    CHECK(s, pty_screen_contains(s, "Edit rule for testuser") && pty_screen_contains(s, "Name:             R01")
             && pty_screen_contains(s, "add-label [1]:    L01"), "the edit form is prefilled");
    for (int i = 0; i < 9; i++) press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Save? (y/N)", WAIT_MS) == 0, "Enter through all fields asks to save");
    press(s, "y");
    CHECK(s, pty_wait_for(s, "Rule: R01", WAIT_MS) == 0 && rules_in_file() == 1 && file_has("[rule \"R01\"]")
             && file_has("then-add-label    = L01") && file_has("two"), "the unchanged edit keeps the rule");
    pty_close(s);

    s = at_form(1);
    ASSERT(s != NULL, "form edit text: opens");
    press(s, "Enter");                                  /* name kept */
    pty_send_str(s, "Extra");                           /* add-label [1]: L01 + Extra */
    press(s, "BS");
    press(s, "BS");
    for (int i = 0; i < 8; i++) press(s, "Enter");
    pty_wait_for(s, "Save? (y/N)", WAIT_MS);
    press(s, "Y");
    CHECK(s, pty_wait_for(s, "Rule: R01", WAIT_MS) == 0 && file_has("then-add-label    = L01Ext"),
          "typing and Backspace edit a field, Y saves");
    pty_close(s);
}

/** Esc in any field cancels without a change; Save? takes only y/Y as yes. */
static void test_rk_form_esc_and_save_keys(void) {
    for (int field = 0; field < 9; field += 4) {
        PtySession *s = at_form(1);
        ASSERT(s != NULL, "form esc: opens");
        for (int i = 0; i < field; i++) press(s, "Enter");
        pty_send_str(s, "ZZZ");
        press(s, "Esc");
        CHECK(s, pty_wait_for(s, "Rule: R01", WAIT_MS) == 0 || pty_wait_for(s, "(1 rule)", WAIT_MS) == 0,
              "Esc in field %d leaves the form", field + 1);
        CHECK(s, !file_has("ZZZ") && rules_in_file() == 1, "Esc in field %d changes nothing", field + 1);
        pty_close(s);
    }
    const char *no_keys[] = { "n", "N", "Esc", "Enter", "x", " ", NULL };
    for (int i = 0; no_keys[i]; i++) {
        PtySession *s = at_form(1);
        ASSERT(s != NULL, "form save keys: opens");
        press(s, "Enter");
        pty_send_str(s, "Q");
        for (int k = 0; k < 8; k++) press(s, "Enter");
        pty_wait_for(s, "Save? (y/N)", WAIT_MS);
        press(s, no_keys[i]);
        CHECK(s, !pty_screen_contains(s, "Save? (y/N)"), "Save? answered with %s closes", no_keys[i]);
        CHECK(s, !file_has("QL01") && rules_in_file() == 1, "Save? %s is a No", no_keys[i]);
        pty_close(s);
    }
}

/** Keys inside a field: Up, Down, Tab do not move between fields or leave. */
static void test_rk_form_field_keys(void) {
    PtySession *s = at_form(0);
    ASSERT(s != NULL, "form keys: opens");
    pty_send_str(s, "Nm");
    const char *keys[] = { "Up", "Down", "Tab", "PgUp", "PgDn", "F1", NULL };
    for (int i = 0; keys[i]; i++) {
        press(s, keys[i]);
        CHECK(s, pty_screen_contains(s, "step 2/2: actions") && pty_screen_contains(s, "Name:             Nm"),
              "form: %s stays in the name field", keys[i]);
    }
    press(s, "Left"); press(s, "Left"); pty_send_str(s, "A"); press(s, "End"); pty_send_str(s, "Z");
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "Name:             ANmZ"), "Left, End and typing edit the field");
    pty_close(s);
}

/** An empty name is refused; any key then returns, nothing is saved. */
static void test_rk_form_name_required_and_resize(void) {
    PtySession *s = at_form(0);
    ASSERT(s != NULL, "form resize: opens");
    pty_resize(s, 100, 30);
    pty_settle(s, 600);
    CHECK(s, pty_screen_contains(s, "step 2/2: actions") && pty_screen_contains(s, "Name:"),
          "a resize keeps the form");
    pty_close(s);

    const char *any[] = { "Esc", "Enter", "q", NULL };
    for (int i = 0; any[i]; i++) {
        s = at_form(0);
        ASSERT(s != NULL, "form name: opens");
        for (int k = 0; k < 9; k++) press(s, "Enter");
        CHECK(s, pty_wait_for(s, "Rule name is required", WAIT_MS) == 0, "empty name is refused");
        press(s, any[i]);
        CHECK(s, pty_wait_for(s, "(0 rules)", WAIT_MS) == 0 && rules_in_file() == 0,
              "%s after the refusal returns to the list", any[i]);
        pty_close(s);
    }
}

/** The editor redraws at once on a resize, the status line on the last row. */
static void test_rk_resize(void) {
    PtySession *s = at_rules(3);
    ASSERT(s != NULL, "resize: opens");
    pty_resize(s, 100, 30);
    pty_settle(s, 600);
    CHECK(s, pty_screen_contains(s, "(3 rules)") && pty_row_contains(s, 29, "Esc/q/BS back"),
          "the list is redrawn with the status line on the new last row");
    pty_close(s);
}

int main(int argc, char *argv[]) {
    printf("--- email-tui rules editor keys (80x24) ---\n\n");
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

    RUN_TEST(test_rk_list_navigation);
    RUN_TEST(test_rk_list_zero_and_one);
    RUN_TEST(test_rk_list_back_keys);
    RUN_TEST(test_rk_list_unbound_keys);
    RUN_TEST(test_rk_list_help_and_status);
    RUN_TEST(test_rk_list_delete);
    RUN_TEST(test_rk_list_preview_apply);
    RUN_TEST(test_rk_list_open_keys);
    RUN_TEST(test_rk_detail_view);
    RUN_TEST(test_rk_detail_back_keys);
    RUN_TEST(test_rk_detail_delete);
    RUN_TEST(test_rk_cond_navigation);
    RUN_TEST(test_rk_cond_zero_and_one);
    RUN_TEST(test_rk_cond_edit_keys);
    RUN_TEST(test_rk_cond_leave_keys);
    RUN_TEST(test_rk_cond_help_and_unbound);
    RUN_TEST(test_rk_actions_form);
    RUN_TEST(test_rk_form_layout_and_edit);
    RUN_TEST(test_rk_form_esc_and_save_keys);
    RUN_TEST(test_rk_form_field_keys);
    RUN_TEST(test_rk_form_name_required_and_resize);
    RUN_TEST(test_rk_resize);

    if (g_mock_pid > 0) { kill(g_mock_pid, SIGKILL); waitpid(g_mock_pid, NULL, 0); }
    printf("\n--- Rules editor key study results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
