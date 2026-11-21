/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-20 */
/* PTY tests of the rules overview, the rule form, the preview and the undo. */
/**
 * @file test_pty_rules_keys.c
 * @brief The rules screens of email-tui at 80x24 (EMAIL-148, EMAIL-312).
 *
 * The rules overview (`l` in the message list) and the rule form (`a`, or
 * Enter on a rule) are driven with the keys a user presses, with 0, 1 and
 * many rules: the overview with its counts, switching, reordering, copying
 * and deleting; the form with its condition rows, the text view, the actions
 * and the live preview; the dry run, the apply and the undo.  The table of
 * what each key does is in docs/spec/tui-keys.md, section "Rules overview and
 * rule form"; the test names there are the functions below.
 *
 * Usage: test-pty-rules-keys <email-tui> <mock-imap-server>
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
#define STATUS_ROW (ROWS - 1)
#define WAIT_MS (6000 * wait_scale())
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


/* The number of the rule the cursor is on (R07 -> 7), 0 when none. */
static int cursor_no(PtySession *s) {
    const char *t = cursor_text(s);
    const char *r = strstr(t, " R");
    return r && r[2] >= '0' && r[2] <= '9' ? atoi(r + 2) : 0;
}

/* The row of the screen that holds @p needle, or -1. */
static int row_of(PtySession *s, const char *needle) {
    char buf[256];
    for (int r = 1; r < ROWS; r++) {
        pty_row_text(s, r, buf, sizeof(buf));
        if (strstr(buf, needle)) return r;
    }
    return -1;
}

static const char *row_text(PtySession *s, const char *needle) {
    static char buf[256];
    int r = row_of(s, needle);
    if (r < 0) { buf[0] = '\0'; return buf; }
    pty_row_text(s, r, buf, sizeof(buf));
    return buf;
}

static void applied_labels_path(char *path, size_t sz) {
    snprintf(path, sz, "%s/.local/share/email-cli/accounts/testuser/applied_labels.tsv", g_home);
}

static int labels_file_has(const char *needle) {
    char path[512], line[1024];
    applied_labels_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int found = 0;
    while (fgets(line, sizeof(line), fp)) if (strstr(line, needle)) found = 1;
    fclose(fp);
    return found;
}

/* Rules that match the mock's three messages (from senderN@example.com) or none. */
static void write_rules_mixed(void) {
    char path[512];
    rules_path(path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) { g_tests_failed++; return; }
    fprintf(fp, "[rule \"R01\"]\nwhen = from:*example*\nthen-add-label = Tagged\n\n"
                "[rule \"R02\"]\nwhen = from:*@nowhere.invalid\nthen-add-label = Never\n\n"
                "[rule \"R03\"]\nenabled = no\nwhen = !to:*z*\n"
                "then-add-label = O\nthen-remove-label = INBOX\n");
    fclose(fp);
}

/* ── The overview ────────────────────────────────────────────────────── */

/** j k Down Up Home End g G PgUp PgDn on 30 rules; they stop at the ends. */
static void test_rk_list_navigation(void) {
    PtySession *s = at_rules(30);
    ASSERT(s != NULL, "rules list nav: opens");
    CHECK(s, pty_screen_contains(s, "(30 rules)"), "title counts 30 rules");
    CHECK(s, cursor_no(s) == 1, "cursor starts on the first rule");
    press(s, "k"); press(s, "Up");
    CHECK(s, cursor_no(s) == 1, "k and Up stop at the first rule");
    press(s, "j");
    CHECK(s, cursor_no(s) == 2, "j moves down");
    press(s, "Down");
    CHECK(s, cursor_no(s) == 3, "Down moves down");
    press(s, "k");
    CHECK(s, cursor_no(s) == 2, "k moves up");
    press(s, "Up");
    CHECK(s, cursor_no(s) == 1, "Up moves up");
    press(s, "End");
    CHECK(s, cursor_no(s) == 30, "End goes to the last rule");
    press(s, "j"); press(s, "Down");
    CHECK(s, cursor_no(s) == 30, "j and Down stop at the last rule");
    press(s, "Home");
    CHECK(s, cursor_no(s) == 1, "Home goes to the first rule");
    press(s, "G");
    CHECK(s, cursor_no(s) == 30, "G goes to the last rule");
    press(s, "g");
    CHECK(s, cursor_no(s) == 1, "g goes to the first rule");
    press(s, "PgDn");
    int page = cursor_no(s);
    CHECK(s, page > 10 && page < 30, "PgDn moves a page (cursor on R%02d)", page);
    press(s, "PgDn"); press(s, "PgDn");
    CHECK(s, cursor_no(s) == 30, "PgDn stops at the last rule");
    press(s, "PgUp");
    CHECK(s, cursor_no(s) < 30 && cursor_no(s) > 1, "PgUp moves a page back");
    press(s, "PgUp"); press(s, "PgUp"); press(s, "PgUp");
    CHECK(s, cursor_no(s) == 1, "PgUp stops at the first rule");
    pty_close(s);
}

/** 0 rules: the empty message; 1 rule: nothing to move to. */
static void test_rk_list_zero_and_one(void) {
    PtySession *s = at_rules(0);
    ASSERT(s != NULL, "rules list 0: opens");
    CHECK(s, pty_screen_contains(s, "(0 rules)"), "0 rules: title");
    CHECK(s, pty_screen_contains(s, "(no rules -- press 'a' to add one)"), "0 rules: the empty message");
    const char *keys[] = { "j", "k", "Down", "Up", "Home", "End", "PgUp", "PgDn", "g", "G",
                           "Enter", "d", "e", "c", "J", "K", " ", NULL };
    for (int i = 0; keys[i]; i++) {
        press(s, keys[i]);
        CHECK(s, pty_screen_contains(s, "(0 rules)") && !pty_screen_contains(s, "Delete a rule")
                 && !pty_screen_contains(s, "Edit rule"),
              "0 rules: %s changes nothing", keys[i]);
    }
    press(s, "p");
    CHECK(s, pty_wait_for(s, "No rules found", WAIT_MS) == 0, "0 rules: p previews (no rules found)");
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "(0 rules)", WAIT_MS) == 0, "0 rules: Esc leaves the preview");
    pty_close(s);

    s = at_rules(1);
    ASSERT(s != NULL, "rules list 1: opens");
    CHECK(s, pty_screen_contains(s, "(1 rule)"), "1 rule: title is singular");
    const char *k1[] = { "j", "k", "Down", "Up", "Home", "End", "PgUp", "PgDn", "g", "G", "J", "K", NULL };
    for (int i = 0; k1[i]; i++) {
        press(s, k1[i]);
        CHECK(s, cursor_no(s) == 1, "1 rule: %s keeps the cursor on the rule", k1[i]);
    }
    CHECK(s, rules_in_file() == 1 && !file_has("enabled"), "1 rule: J and K did not touch the file");
    pty_close(s);
}

/** Esc, q, Backspace, Left, h go back to the message list; ctrl-c quits. */
static void test_rk_list_back_keys(void) {
    const char *keys[] = { "Esc", "q", "BS", "Left", "h", NULL };
    for (int i = 0; keys[i]; i++) {
        PtySession *s = at_rules(2);
        ASSERT(s != NULL, "rules list back: opens");
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
    const char *keys[] = { "x", "r", "l", "t", "D", "Z", "U", "1", "-", "/", ":",
                           "Tab", "Right", "Del", "Ins", "F1", NULL };
    for (int i = 0; keys[i]; i++) {
        press(s, keys[i]);
        CHECK(s, pty_screen_contains(s, "(3 rules)") && cursor_no(s) == 1
                 && !pty_screen_contains(s, "Delete a rule") && !pty_screen_contains(s, "Edit rule"),
              "rules list: %s changes nothing", keys[i]);
    }
    CHECK(s, rules_in_file() == 3 && !file_has("(copy)") && !file_has("enabled"),
          "rules list: the unbound keys left the file as it was");
    pty_close(s);
}

/** The status line at 80x24 and the help overlay. */
static void test_rk_list_help_and_status(void) {
    PtySession *s = at_rules(2);
    ASSERT(s != NULL, "rules list help: opens");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "a add") && pty_row_contains(s, STATUS_ROW, "d delete")
             && pty_row_contains(s, STATUS_ROW, "c copy") && pty_row_contains(s, STATUS_ROW, "? help"),
          "status line shows the keys of the overview");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "[1/2]"), "status line shows the position");
    const char *closers[] = { "Esc", "q", "BS", NULL };
    for (int i = 0; closers[i]; i++) {
        press(s, "?");
        CHECK(s, pty_wait_for(s, "Rules shortcuts", WAIT_MS) == 0, "? opens the help");
        CHECK(s, pty_screen_contains(s, "Add a rule") && pty_screen_contains(s, "Copy the rule")
                 && pty_screen_contains(s, "Undo the last apply") && pty_screen_contains(s, "(2 rules)"),
              "the help lists the keys and the list stays behind it");
        press(s, closers[i]);
        CHECK(s, !pty_screen_contains(s, "Rules shortcuts") && pty_screen_contains(s, "(2 rules)"),
              "%s closes the help and stays in the list", closers[i]);
    }
    pty_close(s);
}

/** The columns, the plain description, the counts, on and off, and their colours. */
static void test_rk_overview_columns_and_counts(void) {
    restart_mock();
    make_home("ov");
    write_rules_mixed();
    PtySession *s = tui_open();
    ASSERT(s != NULL, "overview: opens");
    pty_wait_for(s, "Email Accounts", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "Folders", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "message(s) in", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    ASSERT(pty_wait_for(s, "Rules for", WAIT_MS) == 0, "overview: the rules screen opens");
    pty_settle(s, SETTLE_MS);
    CHECK(s, pty_screen_contains(s, "What it does") && pty_screen_contains(s, "Matches")
             && pty_screen_contains(s, "On"), "the column heads");
    CHECK(s, strstr(row_text(s, "R01"), "from contains example") != NULL,
          "the condition is in words: [%s]", row_text(s, "R01"));
    CHECK(s, strstr(row_text(s, "R01"), "+Tagged") != NULL, "the action is on the row: [%s]", row_text(s, "R01"));
    CHECK(s, strstr(row_text(s, "R03"), "does not contain z") != NULL, "a negated condition is in words: [%s]",
          row_text(s, "R03"));
    CHECK(s, strstr(row_text(s, "R03"), "-INBOX") != NULL, "a removed label is on the row");
    CHECK(s, strstr(row_text(s, "R01"), " on ") != NULL && strstr(row_text(s, "R03"), " off ") != NULL,
          "on and off: [%s] [%s]", row_text(s, "R01"), row_text(s, "R03"));
    CHECK(s, strstr(row_text(s, "R01"), "  3 ") != NULL, "R01 matches the 3 cached messages: [%s]", row_text(s, "R01"));
    CHECK(s, strstr(row_text(s, "R02"), "  0 ") != NULL, "R02 matches none: [%s]", row_text(s, "R02"));
    CHECK(s, strstr(row_text(s, "R03"), "  3 ") != NULL, "an off rule shows what it would match");
    CHECK(s, strstr(row_text(s, "R01"), "never") != NULL, "a rule that changed nothing yet says never");
    /* colours: the header bar is bold white on blue; "on" is coloured, "off" is dim */
    CHECK(s, pty_cell_fg(s, 0, 2) == 37 && pty_cell_bg(s, 0, 2) == 44 && (pty_cell_attr(s, 0, 2) & PTY_ATTR_BOLD),
          "the title bar is bold white on blue");
    int r1 = row_of(s, "R01"), r3 = row_of(s, "R03");
    const char *t1 = row_text(s, "R01");
    int on_col = (int)(strstr(t1, " on ") - t1) + 1;
    const char *t3 = row_text(s, "R03");
    int off_col = (int)(strstr(t3, " off ") - t3) + 1;
    CHECK(s, pty_cell_fg(s, r1, on_col) != 0, "on is written in a colour of its own");
    CHECK(s, (pty_cell_attr(s, r3, off_col) & PTY_ATTR_DIM) != 0, "an off rule's row is dim");
    pty_close(s);
}

/** d asks first, naming the rule; No is the default; y deletes. */
static void test_rk_list_delete(void) {
    const char *no[] = { "n", "Esc", "q", "BS", NULL };
    for (int i = 0; no[i]; i++) {
        PtySession *s = at_rules(3);
        ASSERT(s != NULL, "rules list delete: opens");
        press(s, "d");
        CHECK(s, pty_wait_for(s, "Delete the rule \"R01\"?", WAIT_MS) == 0, "d asks and names the rule");
        press(s, no[i]);
        CHECK(s, pty_screen_contains(s, "(3 rules)") && rules_in_file() == 3 && !pty_screen_contains(s, "Delete a rule"),
              "rules list delete: %s refuses", no[i]);
        pty_close(s);
    }
    PtySession *s = at_rules(3);
    ASSERT(s != NULL, "rules list delete default: opens");
    press(s, "d");
    pty_wait_for(s, "Delete the rule", WAIT_MS);
    press(s, "Enter");
    CHECK(s, rules_in_file() == 3, "Enter takes the default: No");
    pty_close(s);

    s = at_rules(3);
    ASSERT(s != NULL, "rules list delete yes: opens");
    press(s, "j");
    press(s, "d");
    CHECK(s, pty_wait_for(s, "Delete the rule \"R02\"?", WAIT_MS) == 0, "d names the selected rule");
    press(s, "y");
    CHECK(s, pty_wait_for(s, "(2 rules)", WAIT_MS) == 0 && rules_in_file() == 2 && !file_has("R02"),
          "y deletes the rule");
    CHECK(s, cursor_no(s) == 3, "the cursor stays on the row, now R03");
    press(s, "End"); press(s, "d"); press(s, "y");
    CHECK(s, pty_wait_for(s, "(1 rule)", WAIT_MS) == 0 && cursor_no(s) == 1,
          "deleting the last row moves the cursor up");
    press(s, "d"); press(s, "y");
    CHECK(s, pty_wait_for(s, "(0 rules)", WAIT_MS) == 0 && rules_in_file() == 0,
          "deleting the only rule leaves the empty list");
    pty_close(s);
}

/** Space switches a rule off and on; J and K reorder; c copies. */
static void test_rk_toggle_reorder_copy(void) {
    PtySession *s = at_rules(3);
    ASSERT(s != NULL, "toggle: opens");
    press(s, "j");
    press(s, " ");
    CHECK(s, strstr(row_text(s, "R02"), " off ") != NULL && file_has("enabled = no"),
          "Space switches the rule off and writes 'enabled = no': [%s]", row_text(s, "R02"));
    CHECK(s, pty_screen_contains(s, "Rule switched off: R02"), "the bottom line says so");
    press(s, " ");
    CHECK(s, strstr(row_text(s, "R02"), " on ") != NULL && !file_has("enabled"),
          "Space again switches it on and the file has no 'enabled' line");
    /* reorder */
    press(s, "J");
    CHECK(s, cursor_no(s) == 2 && strstr(cursor_text(s), "3 R02") != NULL,
          "J moves the rule down and the cursor follows it: [%s]", cursor_text(s));
    {
        char path[512], line[1024];
        rules_path(path, sizeof(path));
        FILE *fp = fopen(path, "r");
        int order[3] = {0}, n = 0;
        while (fp && fgets(line, sizeof(line), fp))
            if (strncmp(line, "[rule \"R", 8) == 0 && n < 3) order[n++] = atoi(line + 8);
        if (fp) fclose(fp);
        CHECK(s, order[0] == 1 && order[1] == 3 && order[2] == 2, "J wrote the new order: %d %d %d",
              order[0], order[1], order[2]);
    }
    press(s, "K"); press(s, "K");
    CHECK(s, pty_screen_contains(s, "Rule moved to place 1: R02") || cursor_no(s) == 2, "K moves it up again");
    press(s, "Home"); press(s, "K");
    CHECK(s, cursor_no(s) == 2 && strstr(cursor_text(s), "1 R02") != NULL, "K on the first rule moves nothing");
    /* copy */
    press(s, "Home");
    press(s, "c");
    CHECK(s, pty_screen_contains(s, "(4 rules)") && file_has("[rule \"R02 (copy)\"]")
             && strstr(cursor_text(s), "2 R02 (copy)") != NULL,
          "c adds a copy right under the rule, and the cursor goes to it: [%s]", cursor_text(s));
    {
        char path[512], line[1024];
        int n = 0;
        rules_path(path, sizeof(path));
        FILE *fp = fopen(path, "r");
        while (fp && fgets(line, sizeof(line), fp)) if (strstr(line, "= L02")) n++;
        if (fp) fclose(fp);
        CHECK(s, n == 2, "the copy has the same actions (%d lines with L02)", n);
    }
    pty_close(s);
}

/** p shows the dry run and changes nothing; A previews, asks, applies and offers the undo; u undoes. */
static void test_rk_preview_apply_undo(void) {
    restart_mock();
    make_home("ap");
    write_rules_mixed();
    PtySession *s = tui_open();
    ASSERT(s != NULL, "apply: opens");
    pty_wait_for(s, "Email Accounts", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "Folders", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "message(s) in", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    pty_wait_for(s, "Rules for", WAIT_MS); pty_settle(s, SETTLE_MS);

    press(s, "p");
    CHECK(s, pty_wait_for(s, "Rules preview (dry run, nothing changed)", WAIT_MS) == 0
             && pty_row_contains(s, STATUS_ROW, "y apply"), "p shows the dry run, with the apply key on the status line");
    CHECK(s, pty_screen_contains(s, "[dry-run]") && pty_screen_contains(s, "R01")
             && pty_screen_contains(s, "+Tagged") && pty_screen_contains(s, "3 message(s) would be modified"),
          "the report names the rule, its label and the count");
    CHECK(s, pty_screen_contains(s, "Rule \"R01\": 3 message(s)"), "the report has the per-rule count");
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "(3 rules)", WAIT_MS) == 0 && pty_screen_contains(s, "nothing was changed")
             && !labels_file_has("Tagged"), "Esc leaves the preview and nothing was changed");

    press(s, "A");
    pty_wait_for(s, "Rules preview", WAIT_MS);
    press(s, "y");
    CHECK(s, pty_wait_for(s, "Change 3 messages now?", WAIT_MS) == 0, "after the preview, y asks before changing");
    press(s, "n");
    CHECK(s, pty_wait_for(s, "(3 rules)", WAIT_MS) == 0 && !labels_file_has("Tagged"), "No changes nothing");

    press(s, "A");
    pty_wait_for(s, "Rules preview", WAIT_MS);
    press(s, "y");
    pty_wait_for(s, "Change 3 messages now?", WAIT_MS);
    press(s, "y");
    CHECK(s, pty_wait_for(s, "Rules applied: testuser (3 messages changed)", WAIT_MS) == 0
             && pty_screen_contains(s, "[rule]"), "y applies and shows the report");
    CHECK(s, labels_file_has("Tagged"), "the labels were written");
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "Undo this apply now?", WAIT_MS) == 0 && pty_screen_contains(s, "3 messages were changed"),
          "the undo is offered with the count of changes");
    press(s, "n");
    CHECK(s, pty_wait_for(s, "(3 rules)", WAIT_MS) == 0 && labels_file_has("Tagged")
             && pty_screen_contains(s, "Applied: 3 messages changed"), "No keeps the apply");
    CHECK(s, strstr(row_text(s, "R01"), "never") == NULL, "the last change of the rule is shown now: [%s]", row_text(s, "R01"));

    press(s, "u");
    CHECK(s, pty_wait_for(s, "Take back the last rules apply", WAIT_MS) == 0, "u asks first");
    press(s, "y");
    CHECK(s, pty_wait_for(s, "3 message(s) restored", WAIT_MS) == 0 && !labels_file_has("Tagged"),
          "y takes the apply back");
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "Apply undone", WAIT_MS) == 0, "the bottom line says it was undone");
    pty_close(s);

    /* the offer right after an apply: Yes undoes at once */
    s = tui_open();
    ASSERT(s != NULL, "apply yes: opens");
    pty_wait_for(s, "Email Accounts", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "Folders", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "message(s) in", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    pty_wait_for(s, "Rules for", WAIT_MS); pty_settle(s, SETTLE_MS);
    press(s, "A"); pty_wait_for(s, "Rules preview", WAIT_MS);
    press(s, "y"); pty_wait_for(s, "Change 3 messages now?", WAIT_MS);
    press(s, "y"); pty_wait_for(s, "Rules applied", WAIT_MS);
    press(s, "Esc"); pty_wait_for(s, "Undo this apply now?", WAIT_MS);
    press(s, "y");
    CHECK(s, pty_wait_for(s, "3 message(s) restored", WAIT_MS) == 0 && !labels_file_has("Tagged"),
          "Yes at the offer takes the apply back at once");
    pty_close(s);
}

/* ── The rule form ───────────────────────────────────────────────────── */

/** a opens the form; Esc leaves it (nothing changed: no question). */
static void test_rk_form_opens_and_leaves(void) {
    PtySession *s = at_rules(3);
    ASSERT(s != NULL, "form: opens");
    press(s, "j");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Edit rule for testuser", WAIT_MS) == 0, "Enter opens the selected rule in the form");
    CHECK(s, pty_screen_contains(s, "R02") && pty_screen_contains(s, "from matches glob *@r02.example.org")
             && pty_screen_contains(s, "from:*@r02.example.org"), "the form shows the name, the clause and the text");
    CHECK(s, pty_screen_contains(s, "as text") && pty_screen_contains(s, "add label")
             && pty_screen_contains(s, "L02") && pty_screen_contains(s, "+ add an action")
             && pty_screen_contains(s, "+ add a condition"), "the form has the condition, the text and the actions");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "s save") && pty_row_contains(s, STATUS_ROW, "Esc cancel"),
          "the status line has the keys of the form");
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "Rules for", WAIT_MS) == 0 && cursor_no(s) == 2 && !pty_screen_contains(s, "Edit rule"),
          "Esc leaves the form (nothing changed) and keeps the cursor row");
    press(s, "a");
    CHECK(s, pty_wait_for(s, "New rule for testuser", WAIT_MS) == 0, "a opens the form of a new rule");
    press(s, "q");
    CHECK(s, pty_wait_for(s, "(3 rules)", WAIT_MS) == 0, "q leaves the new form");
    pty_close(s);
}

/** A new rule from widgets: name, a condition in a form, an action list, saved to the file. */
static void test_rk_form_new_rule(void) {
    PtySession *s = at_rules(0);
    ASSERT(s != NULL, "new rule: opens");
    press(s, "a");
    pty_wait_for(s, "New rule for testuser", WAIT_MS);
    CHECK(s, pty_screen_contains(s, "(not named yet)") && pty_screen_contains(s, "(none yet)")
             && pty_screen_contains(s, "Preview: (no condition yet)"), "an empty form says what is missing");
    press(s, "s");
    CHECK(s, pty_wait_for(s, "The rule needs a name.", WAIT_MS) == 0, "s on an empty form refuses: the name");
    press(s, "Enter");
    /* the name */
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Rule name", WAIT_MS) == 0, "Enter on the name row asks for the name");
    pty_send_str(s, "Acme mails");
    pty_settle(s, 200);
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Acme mails", WAIT_MS) == 0 && !pty_screen_contains(s, "(not named yet)"),
          "the name is on the row");
    press(s, "s");
    CHECK(s, pty_wait_for(s, "The rule needs a condition.", WAIT_MS) == 0, "s without a condition refuses");
    press(s, "Enter");
    /* a condition: the fieldset */
    press(s, "j"); press(s, "j");
    CHECK(s, cursor_has(s, "+ add a condition"), "the cursor reaches the add-a-condition row");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Add a condition", WAIT_MS) == 0 && pty_screen_contains(s, "Field:")
             && pty_screen_contains(s, "Not:") && pty_screen_contains(s, "Operator:") && pty_screen_contains(s, "Value:"),
          "the condition form: field, not, operator, value");
    CHECK(s, pty_screen_contains(s, "contains") && pty_screen_contains(s, "from"), "the defaults are 'from contains'");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "The value is empty.", WAIT_MS) == 0, "an empty value is refused with a reason");
    pty_send_str(s, "example.com");
    pty_settle(s, 200);
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "from contains example.com", WAIT_MS) == 0
             && pty_screen_contains(s, "from:*example.com*"), "the clause is a row and the text view shows the expression");
    /* the live preview */
    CHECK(s, pty_wait_for(s, "Preview: 3 of 3 cached messages match", WAIT_MS) == 0
             && pty_screen_contains(s, "Sender 1"), "the live preview counts the matches and shows the first ones");
    /* a second clause joined with and, negated */
    press(s, "a");
    pty_wait_for(s, "Add a condition", WAIT_MS);
    CHECK(s, pty_screen_contains(s, "Join:"), "the second clause has a join field");
    press(s, "Up"); press(s, "Up"); press(s, "Up");   /* Value -> Operator -> Not -> Field */
    press(s, "Right"); press(s, "Right"); /* from -> to -> subject */
    press(s, "Down");                      /* Field -> Not */
    press(s, "Right");                     /* Not: yes */
    press(s, "Down"); press(s, "Down");    /* Operator -> Value */
    pty_send_str(s, "zzz");
    pty_settle(s, 200);
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "not subject contains zzz", WAIT_MS) == 0
             && pty_row_contains(s, row_of(s, "not subject contains zzz"), "and")
             && pty_screen_contains(s, "from:*example.com* and !subject:*zzz*"),
          "the second clause is joined with and and negated, in both views");
    CHECK(s, pty_wait_for(s, "Preview: 3 of 3 cached messages match", WAIT_MS) == 0, "the preview follows the edit");
    CHECK(s, cursor_has(s, "not subject contains zzz"), "the cursor is on the clause just added");
    /* actions: pick labels from a list, any number of them */
    press(s, "j"); press(s, "j"); press(s, "j");
    CHECK(s, cursor_has(s, "+ add an action"), "the cursor reaches the add-an-action row");
    press(s, "a");
    CHECK(s, pty_wait_for(s, "Add an action", WAIT_MS) == 0 && pty_screen_contains(s, "Add labels")
             && pty_screen_contains(s, "Remove labels") && pty_screen_contains(s, "Move to a folder")
             && pty_screen_contains(s, "Forward to an address"), "a on an action row offers the actions as a list");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Labels to add", WAIT_MS) == 0 && pty_screen_contains(s, "UNREAD")
             && pty_screen_contains(s, "(a new label...)"), "the labels are a list to pick from");
    press(s, "j");
    press(s, " ");                           /* tick the second label */
    press(s, "End");
    press(s, " ");                           /* (a new label...) */
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "New labels", WAIT_MS) == 0, "a new label is asked for");
    pty_send_str(s, "Work, Clients, Urgent, Four");
    pty_settle(s, 200);
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Four", WAIT_MS) == 0 && pty_screen_contains(s, "Clients") && pty_screen_contains(s, "Urgent")
             && pty_screen_contains(s, "add label"), "more than three labels are kept: [Work Clients Urgent Four]");
    press(s, "s");
    CHECK(s, pty_wait_for(s, "Rule added: Acme mails", WAIT_MS) == 0 && pty_screen_contains(s, "(1 rule)"),
          "s saves and the overview shows the new rule");
    CHECK(s, file_has("[rule \"Acme mails\"]") && file_has("when = from:*example.com* and !subject:*zzz*")
             && file_has("= _flagged") && file_has("= Work") && file_has("= Clients") && file_has("= Urgent") && file_has("= Four"),
          "the file has the rule: the condition and every label");
    pty_close(s);
}

/** The text view and the clause view are the same condition; a text that has no clause form is kept as text. */
static void test_rk_form_text_view(void) {
    PtySession *s = at_rules(3);
    ASSERT(s != NULL, "text view: opens");
    press(s, "Enter");                     /* R01: three conditions joined with or */
    pty_wait_for(s, "Edit rule for testuser", WAIT_MS);
    CHECK(s, pty_screen_contains(s, "from contains") || pty_screen_contains(s, "from matches")
             , "R01's three conditions are rows: [from ...]");
    CHECK(s, pty_screen_contains(s, "or  to matches *@three.example.org") || pty_screen_contains(s, "or"),
          "the conditions are joined with or");
    press(s, "j"); press(s, "j"); press(s, "j"); press(s, "j"); press(s, "j");
    CHECK(s, cursor_has(s, "as text") || cursor_has(s, "+ add a condition"), "the cursor can reach the text row");
    while (!cursor_has(s, "as text")) press(s, "j");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "The condition as text", WAIT_MS) == 0, "Enter on the text row edits the text");
    pty_send_key(s, PTY_KEY_END);
    pty_send_str(s, " and from:x");
    press(s, "Enter");                     /* "or ... and from:x" parses */
    CHECK(s, pty_wait_for(s, "and  from is x", WAIT_MS) == 0 || pty_wait_for(s, "from is x", WAIT_MS) == 0,
          "the clause rows follow the text");
    /* invalid text is refused with a reason and changes nothing */
    press(s, "Enter");
    pty_wait_for(s, "The condition as text", WAIT_MS);
    pty_send_key(s, PTY_KEY_END);
    pty_send_str(s, " and colour:red");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Not a valid condition", WAIT_MS) == 0 && pty_screen_contains(s, "field:pattern"),
          "an invalid text is refused with the reason");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "from is x", WAIT_MS) == 0, "and the form still holds the earlier condition");
    /* parentheses that change the meaning: no clause rows, the text is kept */
    press(s, "Enter");
    pty_wait_for(s, "The condition as text", WAIT_MS);
    for (int i = 0; i < 200; i++) pty_send_key(s, PTY_KEY_BACK);
    pty_settle(s, 200);
    pty_send_str(s, "from:a and (subject:b or to:c)");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "has parentheses or 'all': edit it as text", WAIT_MS) == 0
             && pty_screen_contains(s, "from:a and (subject:b or to:c)"),
          "a condition with grouping is kept as text and the clause rows say so");
    press(s, "a");
    CHECK(s, pty_wait_for(s, "edit it as text", WAIT_MS) == 0 && pty_screen_contains(s, "Add a condition")
             , "a on such a condition refuses and says why");
    press(s, "Enter");
    press(s, "s");
    CHECK(s, pty_wait_for(s, "Rule saved: R01", WAIT_MS) == 0 && file_has("when = from:a and (subject:b or to:c)"),
          "the grouped condition is saved as it was typed");
    pty_close(s);
}

/** Editing an existing rule: conditions deleted, labels edited, the discard question, save. */
static void test_rk_form_edit_existing(void) {
    PtySession *s = at_rules(3);
    ASSERT(s != NULL, "edit: opens");
    press(s, "Enter");
    pty_wait_for(s, "Edit rule for testuser", WAIT_MS);
    press(s, "j"); press(s, "j");          /* first condition row */
    press(s, "d");
    CHECK(s, !pty_screen_contains(s, "*@one.example.org") && pty_screen_contains(s, "two"),
          "d deletes the condition row and the text view follows");
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "Throw away the changes of this rule?", WAIT_MS) == 0,
          "Esc after a change asks before throwing it away");
    press(s, "n");
    CHECK(s, pty_screen_contains(s, "Edit rule for testuser"), "No stays in the form");
    press(s, "Esc");
    pty_wait_for(s, "Throw away", WAIT_MS);
    press(s, "y");
    CHECK(s, pty_wait_for(s, "(3 rules)", WAIT_MS) == 0 && file_has("*@one.example.org"), "Yes leaves and the file is untouched");
    press(s, "Enter");
    pty_wait_for(s, "Edit rule for testuser", WAIT_MS);
    while (!cursor_has(s, "add label")) press(s, "j");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Label to add", WAIT_MS) == 0, "Enter on a label row edits the label");
    for (int i = 0; i < 10; i++) pty_send_key(s, PTY_KEY_BACK);
    pty_send_str(s, "Renamed");
    pty_settle(s, 200);
    press(s, "Enter");
    press(s, "s");
    CHECK(s, pty_wait_for(s, "Rule saved: R01", WAIT_MS) == 0 && file_has("= Renamed")
             && !file_has("= L01") && rules_in_file() == 3 && cursor_no(s) == 1,
          "s saves the edited rule in its place");
    pty_close(s);
}

/** The form's help, and the name clash. */
static void test_rk_form_help_and_clash(void) {
    PtySession *s = at_rules(2);
    ASSERT(s != NULL, "form help: opens");
    press(s, "Enter");
    pty_wait_for(s, "Edit rule for testuser", WAIT_MS);
    press(s, "?");
    CHECK(s, pty_wait_for(s, "Rule form shortcuts", WAIT_MS) == 0 && pty_screen_contains(s, "Save the rule"),
          "? opens the help of the form");
    press(s, "Esc");
    CHECK(s, !pty_screen_contains(s, "Rule form shortcuts") && pty_screen_contains(s, "Edit rule for testuser"),
          "Esc closes the help and stays in the form");
    press(s, "Enter");                     /* Name row */
    pty_wait_for(s, "Rule name", WAIT_MS);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_BACK);
    pty_send_str(s, "R02");
    pty_settle(s, 200);
    press(s, "Enter");
    press(s, "s");
    CHECK(s, pty_wait_for(s, "Another rule has this name.", WAIT_MS) == 0 && file_has("[rule \"R01\"]"),
          "saving under a name another rule has is refused");
    pty_close(s);
}

/** A rule with a long condition term is shown and saved whole. */
static void test_rk_form_long_term(void) {
    restart_mock();
    make_home("lg");
    char path[512];
    rules_path(path, sizeof(path));
    FILE *fp = fopen(path, "w");
    ASSERT(fp != NULL, "long: file");
    fprintf(fp, "[rule \"Long\"]\nwhen = subject:*");
    for (int i = 0; i < 700; i++) fputc('a', fp);
    fprintf(fp, "end* or from:x\nthen-add-label = A\n");
    fclose(fp);
    PtySession *s = tui_open();
    ASSERT(s != NULL, "long: tui");
    pty_wait_for(s, "Email Accounts", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "Folders", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "message(s) in", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    pty_wait_for(s, "Rules for", WAIT_MS); pty_settle(s, SETTLE_MS);
    press(s, "Enter");
    pty_wait_for(s, "Edit rule for testuser", WAIT_MS);
    press(s, "Enter");                     /* name: rename so that a save really writes */
    pty_wait_for(s, "Rule name", WAIT_MS);
    pty_send_str(s, "2");
    press(s, "Enter");
    press(s, "s");
    CHECK(s, pty_wait_for(s, "Rule saved: Long2", WAIT_MS) == 0, "the rule saved");
    FILE *rf = fopen(path, "r");
    char line[4096] = "";
    int found = 0;
    while (rf && fgets(line, sizeof(line), rf))
        if (strncmp(line, "when = ", 7) == 0) found = (int)strlen(line);
    if (rf) fclose(rf);
    CHECK(s, found > 700, "the long term was kept whole in the file (%d characters)", found);
    pty_close(s);
}


/** In the message reader R tells which rules match the message and, after an apply, which rule gave a label. */
static void test_rk_reader_rules_and_why(void) {
    restart_mock();
    make_home("rd");
    write_rules_mixed();
    PtySession *s = tui_open();
    ASSERT(s != NULL, "reader: opens");
    pty_wait_for(s, "Email Accounts", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "Folders", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "message(s) in", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);                     /* read the first message */
    pty_wait_for(s, "Message ", WAIT_MS); pty_settle(s, SETTLE_MS);
    press(s, "?");
    CHECK(s, pty_wait_for(s, "which rules match this message", WAIT_MS) == 0, "the reader's help names R");
    press(s, "Esc");
    press(s, "R");
    CHECK(s, pty_wait_for(s, "Rules for this message", WAIT_MS) == 0 && pty_screen_contains(s, "1 rule match")
             && pty_screen_contains(s, "\"R01\"") && pty_screen_contains(s, "+Tagged"),
          "R shows the rule that matches and what it would do");
    CHECK(s, pty_screen_contains(s, "R03") && pty_screen_contains(s, "switched off"),
          "a switched-off rule that would match is named");
    CHECK(s, pty_screen_contains(s, "No apply has changed this message yet."), "before an apply there is no reason to show");
    press(s, "Esc");
    CHECK(s, pty_wait_for(s, "Message ", WAIT_MS) == 0 && !pty_screen_contains(s, "Rules for this message"),
          "Esc goes back to the message");
    press(s, "q");                                      /* to the list */
    pty_wait_for(s, "message(s) in", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_str(s, "l");
    pty_wait_for(s, "Rules for", WAIT_MS); pty_settle(s, SETTLE_MS);
    press(s, "A"); pty_wait_for(s, "Rules preview", WAIT_MS);
    press(s, "y"); pty_wait_for(s, "Change 3 messages now?", WAIT_MS);
    press(s, "y"); pty_wait_for(s, "Rules applied", WAIT_MS);
    press(s, "Esc"); pty_wait_for(s, "Undo this apply now?", WAIT_MS);
    press(s, "n"); pty_wait_for(s, "(3 rules)", WAIT_MS);
    press(s, "Esc");
    pty_wait_for(s, "message(s) in", WAIT_MS); pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "Message ", WAIT_MS); pty_settle(s, SETTLE_MS);
    press(s, "R");
    CHECK(s, pty_wait_for(s, "Changed by an earlier apply:", WAIT_MS) == 0 && pty_screen_contains(s, "rule \"R01\" gave +Tagged"),
          "after an apply the reader says which rule gave the label");
    pty_close(s);
}

/** The editor redraws at once on a resize, the status line on the last row. */
static void test_rk_resize(void) {
    PtySession *s = at_rules(3);
    ASSERT(s != NULL, "resize: opens");
    pty_resize(s, 100, 30);
    pty_settle(s, 600);
    CHECK(s, pty_screen_contains(s, "(3 rules)") && pty_row_contains(s, 29, "? help"),
          "the list is redrawn with the status line on the new last row");
    press(s, "Enter");
    pty_wait_for(s, "Edit rule for testuser", WAIT_MS);
    pty_resize(s, 80, 24);
    pty_settle(s, 600);
    CHECK(s, pty_screen_contains(s, "Edit rule for testuser") && pty_row_contains(s, STATUS_ROW, "s save"),
          "the form is redrawn after a resize");
    pty_close(s);
}

int main(int argc, char *argv[]) {
    printf("--- email-tui rules overview and rule form (80x24) ---\n\n");
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
    RUN_TEST(test_rk_overview_columns_and_counts);
    RUN_TEST(test_rk_list_delete);
    RUN_TEST(test_rk_toggle_reorder_copy);
    RUN_TEST(test_rk_preview_apply_undo);
    RUN_TEST(test_rk_form_opens_and_leaves);
    RUN_TEST(test_rk_form_new_rule);
    RUN_TEST(test_rk_form_text_view);
    RUN_TEST(test_rk_form_edit_existing);
    RUN_TEST(test_rk_form_help_and_clash);
    RUN_TEST(test_rk_form_long_term);
    RUN_TEST(test_rk_reader_rules_and_why);
    RUN_TEST(test_rk_resize);

    if (g_mock_pid > 0) { kill(g_mock_pid, SIGKILL); waitpid(g_mock_pid, NULL, 0); }
    printf("\n--- Rules screens test results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
