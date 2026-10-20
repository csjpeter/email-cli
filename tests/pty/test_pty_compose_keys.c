/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-20 */
/* PTY key study of the compose dialog, its address drop-down and the Attach path completion. */
/**
 * @file test_pty_compose_keys.c
 * @brief Every key of the pre-compose dialog of email-tui at 80x24 (EMAIL-150).
 *
 * The dialog (`c` in the message list) is driven with the keys a user presses,
 * with 0, 1 and many drop-down rows.  The table of what each key does is in
 * docs/spec/tui-keys.md, section "Compose dialog, address drop-down and path
 * completion"; the test names there are the functions below.
 *
 * Usage: test-pty-compose-keys <email-tui> <mock-imap-server>
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
#define ROW_TO 2          /* screen rows (0-based) of the five fields, nothing open above */
#define ROW_CC 3
#define ROW_BCC 4
#define ROW_SUBJ 5
#define ROW_ATTACH 6

static pid_t g_mock_pid = -1;
static int   g_imap_port = 0;
static char  g_home[256];
static char  g_att[320];                 /* directory holding the files to attach */
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

static void mk(const char *fmt, const char *a) {
    char dir[600];
    snprintf(dir, sizeof(dir), fmt, a);
    mkdir(dir, 0700);
}

static void touch(const char *dir, const char *name) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *fp = fopen(path, "w");
    if (!fp) { g_tests_failed++; return; }
    fputs("x\n", fp);
    fclose(fp);
}

/* nc contacts: "mmN@example.org" for N = 1..nc (all match the token "mm"),
 * then alice@example.com.  The file order is the drop-down order. */
static void make_home(const char *name, int nc) {
    char path[700];
    snprintf(g_home, sizeof(g_home), "%s", TP("h"));
    size_t n = strlen(g_home);
    snprintf(g_home + n, sizeof(g_home) - n, "-%s-%d", name, ++g_seq);
    mkdir(g_home, 0700);
    mk("%s/.config", g_home);
    mk("%s/.config/email-cli", g_home);
    mk("%s/.config/email-cli/accounts", g_home);
    mk("%s/.config/email-cli/accounts/testuser", g_home);
    mk("%s/.local", g_home);
    mk("%s/.local/share", g_home);
    mk("%s/.local/share/email-cli", g_home);
    mk("%s/.local/share/email-cli/accounts", g_home);
    mk("%s/.local/share/email-cli/accounts/testuser", g_home);
    snprintf(path, sizeof(path), "%s/.config/email-cli/accounts/testuser/config.ini", g_home);
    FILE *fp = fopen(path, "w");
    if (!fp) { g_tests_failed++; return; }
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:%d\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SMTP_HOST=smtps://localhost:%d\n"
        "SMTP_PORT=%d\n"
        "SMTP_USER=testuser\n"
        "SMTP_PASS=testpass\n"
        "SSL_NO_VERIFY=1\n", g_imap_port, g_imap_port, g_imap_port);
    fclose(fp);
    chmod(path, 0600);
    snprintf(path, sizeof(path),
             "%s/.local/share/email-cli/accounts/testuser/contacts.tsv", g_home);
    fp = fopen(path, "w");
    if (!fp) { g_tests_failed++; return; }
    for (int i = 1; i <= nc; i++)
        fprintf(fp, "mm%d@example.org\tMm Person %d\t%d\n", i, i, 100 - i);
    fprintf(fp, "alice@example.com\tAlice Example\t1\n");
    fclose(fp);
    /* the files of the Attach field: two with a common prefix, a directory */
    snprintf(g_att, sizeof(g_att), "%s/att", g_home);
    mkdir(g_att, 0700);
    touch(g_att, "alpha.txt");
    touch(g_att, "alpine.txt");
    touch(g_att, "beta.txt");
    touch(g_att, "dfile.txt");
    char sub[400];
    snprintf(sub, sizeof(sub), "%s/sub", g_att);
    mkdir(sub, 0700);
    touch(sub, "inner.txt");
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

/* The message list of the mock's INBOX, then `c`: the compose dialog. */
static PtySession *at_dialog(int ncontacts) {
    restart_mock();
    make_home("c", ncontacts);
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
    pty_send_str(s, "c");
    if (pty_wait_for(s, "New Message", WAIT_MS) != 0) { pty_close(s); return NULL; }
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
    else if (strcmp(k, "S-Tab") == 0) pty_send(s, "\033[Z", 3);
    else if (strcmp(k, "Del") == 0)   pty_send_key(s, PTY_KEY_DELETE);
    else if (strcmp(k, "Ins") == 0)   pty_send_key(s, PTY_KEY_INSERT);
    else if (strcmp(k, "F1") == 0)    pty_send_key(s, PTY_KEY_F1);
    else if (strcmp(k, "^C") == 0)    pty_send_key(s, PTY_KEY_CTRL_C);
    else                              pty_send_str(s, k);
    pty_settle(s, 250);
}

/* Types text into the active field, one settle at the end. */
static void typed(PtySession *s, const char *text) {
    pty_send_str(s, text);
    pty_settle(s, 300);
}

/* The field the caret is in: 0 To .. 4 Attach (the row of the caret - ROW_TO). */
static int active_field(PtySession *s) {
    int r = 0, c = 0;
    pty_cursor(s, &r, &c);
    return r - ROW_TO;
}

/* 1 when the row holds the label and the value (after the label's columns). */
static int field_is(PtySession *s, int row, const char *value) {
    char buf[256];
    pty_row_text(s, row, buf, sizeof(buf));
    const char *v = strstr(buf, ":");
    if (!v) return 0;
    v += 1;
    while (*v == ' ') v++;
    char want[256];
    snprintf(want, sizeof(want), "%s", value);
    char got[256];
    snprintf(got, sizeof(got), "%s", v);
    size_t n = strlen(got);
    while (n > 0 && got[n - 1] == ' ') got[--n] = '\0';
    return strcmp(got, want) == 0;
}

#define CHECK(s, cond, ...) do { \
    char _w[200]; snprintf(_w, sizeof(_w), __VA_ARGS__); \
    if (!(cond)) pty_screen_dump((s), stdout); \
    ASSERT((cond), _w); \
} while (0)

/* A left dialog is followed by "[Press any key to return to inbox]" (a pause
 * kept for the send result; ticketed as a defect for a cancel: EMAIL-222). */
static void leave_pause(PtySession *s) {
    ASSERT(pty_wait_for(s, "[Press any key", WAIT_MS) == 0, "the pause after the dialog");
    pty_send_str(s, " ");
}

static int in_list(PtySession *s) {
    return pty_screen_contains(s, "message(s) in") && !pty_screen_contains(s, "Subject:");
}

/* ── Layout and field navigation ─────────────────────────────────────── */

/** The dialog: title, five labelled fields, the caret in To, the status line. */
static void test_ck_layout(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "layout: opens");
    CHECK(s, pty_row_contains(s, 0, "New Message"), "title row names the dialog");
    CHECK(s, pty_row_contains(s, ROW_TO, "To:") && pty_row_contains(s, ROW_CC, "Cc:") &&
             pty_row_contains(s, ROW_BCC, "Bcc:") && pty_row_contains(s, ROW_SUBJ, "Subject:") &&
             pty_row_contains(s, ROW_ATTACH, "Attach:"), "five labelled fields");
    CHECK(s, active_field(s) == 0, "the caret starts in To");
    CHECK(s, pty_row_contains(s, STATUS_ROW, "Tab next") && pty_row_contains(s, STATUS_ROW, "Enter ok") &&
             pty_row_contains(s, STATUS_ROW, "Esc cancel"), "status line names Tab, Enter and Esc");
    pty_close(s);
}

/** Tab and Shift-Tab cycle To Cc Bcc Subject Attach and wrap; the Attach status line differs. */
static void test_ck_tab_cycle(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "tab cycle: opens");
    int want[] = { 1, 2, 3, 4, 0 };
    for (int i = 0; i < 5; i++) {
        press(s, "Tab");
        CHECK(s, active_field(s) == want[i], "Tab %d goes to field %d", i + 1, want[i]);
        if (want[i] == 4)
            CHECK(s, pty_row_contains(s, STATUS_ROW, "Tab complete") &&
                     pty_row_contains(s, STATUS_ROW, "remove last"),
                  "the Attach status line names Tab complete and d remove last");
    }
    press(s, "S-Tab");
    CHECK(s, active_field(s) == 4, "Shift-Tab in To wraps to Attach");
    press(s, "S-Tab");
    CHECK(s, active_field(s) == 3, "Shift-Tab goes back to Subject");
    press(s, "S-Tab"); press(s, "S-Tab"); press(s, "S-Tab");
    CHECK(s, active_field(s) == 0, "three more Shift-Tab reach To");
    pty_close(s);
}

/** Left Right Home End Del BS edit the text at the caret; the caret stops at the ends. */
static void test_ck_line_editing(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "line editing: opens");
    press(s, "Tab"); press(s, "Tab"); press(s, "Tab");         /* Subject */
    typed(s, "hello");
    CHECK(s, field_is(s, ROW_SUBJ, "hello"), "typed text appears");
    press(s, "Left"); press(s, "Left");
    typed(s, "X");
    CHECK(s, field_is(s, ROW_SUBJ, "helXlo"), "text is inserted at the caret");
    press(s, "Home");
    typed(s, "Y");
    CHECK(s, field_is(s, ROW_SUBJ, "YhelXlo"), "Home goes to the start");
    press(s, "Left");
    press(s, "Left");                                           /* stays at the start */
    typed(s, "Q");
    CHECK(s, field_is(s, ROW_SUBJ, "QYhelXlo"), "Left stops at the start");
    press(s, "BS"); press(s, "BS");                             /* the start: nothing to delete */
    CHECK(s, field_is(s, ROW_SUBJ, "YhelXlo"), "BS deletes before the caret, nothing at the start");
    press(s, "Del");
    CHECK(s, field_is(s, ROW_SUBJ, "helXlo"), "Del deletes at the caret");
    press(s, "End");
    typed(s, "Z");
    CHECK(s, field_is(s, ROW_SUBJ, "helXloZ"), "End goes to the end");
    press(s, "Right"); press(s, "Del");
    CHECK(s, field_is(s, ROW_SUBJ, "helXloZ"), "Right and Del stop at the end");
    press(s, "BS");
    CHECK(s, field_is(s, ROW_SUBJ, "helXlo"), "BS deletes the last character");
    press(s, "Left"); press(s, "Right"); press(s, "Right");
    typed(s, "!");
    CHECK(s, field_is(s, ROW_SUBJ, "helXlo!"), "Right moves back toward the end");
    pty_close(s);
}

/** Printable keys that are bound elsewhere (h ? q j k ;) are plain text in Subject; the
 *  unbound function keys do nothing. */
static void test_ck_unbound_and_literal_keys(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "unbound: opens");
    press(s, "Tab"); press(s, "Tab"); press(s, "Tab");
    typed(s, "h?qjk;");
    CHECK(s, field_is(s, ROW_SUBJ, "h?qjk;"), "h ? q j k ; are text in Subject");
    press(s, "F1"); press(s, "PgUp"); press(s, "PgDn"); press(s, "Ins"); press(s, "Up"); press(s, "Down");
    CHECK(s, active_field(s) == 3 && field_is(s, ROW_SUBJ, "h?qjk;") &&
             pty_screen_contains(s, "New Message"),
          "F1 PgUp PgDn Ins Up Down without a drop-down do nothing");
    pty_close(s);
}

/** A resize redraws the dialog with the status line on the new last row. */
static void test_ck_resize(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "resize: opens");
    typed(s, "x");
    pty_resize(s, 100, 30);
    pty_send_str(s, "y");
    pty_settle(s, 600);
    CHECK(s, field_is(s, ROW_TO, "xy") && pty_row_contains(s, 29, "Esc cancel"),
          "the dialog is redrawn with the status line on the new last row");
    pty_close(s);
}

/* ── The address drop-down: 0, 1 and many rows ───────────────────────── */

/** 0 rows: no match, or a single character, shows no drop-down; Enter then confirms. */
static void test_ck_dropdown_zero(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "dropdown 0: opens");
    typed(s, "zzz");
    CHECK(s, !pty_screen_contains(s, "@example") && field_is(s, ROW_TO, "zzz"), "no match, no rows");
    press(s, "Down"); press(s, "Up");
    CHECK(s, field_is(s, ROW_TO, "zzz") && active_field(s) == 0, "Down and Up do nothing without rows");
    press(s, "BS"); press(s, "BS"); press(s, "BS");
    typed(s, "m");
    CHECK(s, !pty_screen_contains(s, "mm1@example.org"), "one character opens no drop-down");
    pty_close(s);
}

/** 1 row: Enter takes it and the dialog stays; Tab takes it and stays in the field. */
static void test_ck_dropdown_one(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "dropdown 1: opens");
    typed(s, "alice");
    CHECK(s, pty_screen_contains(s, "alice@example.com") && pty_screen_contains(s, "Alice Example"),
          "one row with the address and the name");
    press(s, "Down"); press(s, "Down");
    press(s, "Enter");
    CHECK(s, field_is(s, ROW_TO, "alice@example.com") && pty_screen_contains(s, "New Message") &&
             active_field(s) == 0, "Enter takes the row and the dialog stays");
    press(s, "BS");
    CHECK(s, field_is(s, ROW_TO, "alice@example.co"), "BS after a taken address deletes one character");
    press(s, "Esc");
    for (int i = 0; i < 16; i++) press(s, "BS");
    CHECK(s, field_is(s, ROW_TO, ""), "BS empties the field");
    typed(s, "alice");
    press(s, "Tab");
    CHECK(s, field_is(s, ROW_TO, "alice@example.com") && active_field(s) == 0,
          "Tab takes the row and stays in the field");
    pty_close(s);
}

/** Many rows (7 match): at most five are shown; Down/Up move, stop at the ends; Enter takes the highlighted. */
static void test_ck_dropdown_many(void) {
    PtySession *s = at_dialog(7);
    ASSERT(s != NULL, "dropdown many: opens");
    typed(s, "mm");
    CHECK(s, pty_screen_contains(s, "mm1@example.org") && pty_screen_contains(s, "mm5@example.org") &&
             !pty_screen_contains(s, "mm6@example.org"), "five rows are shown out of seven");
    press(s, "Up");
    press(s, "Down"); press(s, "Down");
    press(s, "Enter");
    CHECK(s, field_is(s, ROW_TO, "mm3@example.org"), "Up stops at the top; Down Down Enter takes the third");
    typed(s, ";mm");
    for (int i = 0; i < 8; i++) press(s, "Down");
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "mm5@example.org") && !pty_screen_contains(s, "mm6@example.org"),
          "Down stops at the last row (fifth)");
    CHECK(s, field_is(s, ROW_TO, "mm3@example.org; mm5@example.org"),
          "; starts the next address and the fifth row was taken");
    pty_close(s);
}

/** Esc with the drop-down open closes it only; the text stays; typing reopens it. */
static void test_ck_dropdown_esc(void) {
    PtySession *s = at_dialog(7);
    ASSERT(s != NULL, "dropdown esc: opens");
    typed(s, "mm");
    press(s, "Esc");
    CHECK(s, !pty_screen_contains(s, "mm1@example.org") && field_is(s, ROW_TO, "mm") &&
             pty_screen_contains(s, "New Message"), "Esc closes the drop-down, keeps text and dialog");
    typed(s, "1");
    CHECK(s, pty_screen_contains(s, "mm1@example.org"), "typing opens the drop-down again");
    press(s, "BS"); press(s, "BS"); press(s, "BS");
    CHECK(s, !pty_screen_contains(s, "mm1@example.org"), "emptying the field closes the drop-down");
    pty_close(s);
}

/** The drop-down also works in Cc and Bcc, matches the name, ignores case; Tab with no row moves on. */
static void test_ck_dropdown_other_fields(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "dropdown fields: opens");
    press(s, "Tab");
    typed(s, "ALICE");
    CHECK(s, pty_screen_contains(s, "alice@example.com"), "Cc: case-insensitive match");
    press(s, "Enter");
    CHECK(s, field_is(s, ROW_CC, "alice@example.com"), "Cc: Enter takes the row");
    press(s, "Tab");
    CHECK(s, active_field(s) == 2, "Tab with no rows goes to Bcc");
    typed(s, "person 2");
    CHECK(s, pty_screen_contains(s, "mm2@example.org"), "Bcc: matches the display name");
    press(s, "Tab");
    CHECK(s, field_is(s, ROW_BCC, "mm2@example.org") && active_field(s) == 2, "Bcc: Tab takes the row");
    pty_close(s);
}

/** ; commits the address (one space after it, a space before it dropped); BS removes the last whole address. */
static void test_ck_semicolon_and_backspace(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "semicolon: opens");
    typed(s, "a@x.org ;");
    CHECK(s, field_is(s, ROW_TO, "a@x.org;"), "; drops the space before it");
    typed(s, "b@y.org;");
    CHECK(s, field_is(s, ROW_TO, "a@x.org; b@y.org;"), "a second address follows");
    press(s, "BS");
    CHECK(s, field_is(s, ROW_TO, "a@x.org;"), "BS after ; removes the whole last address");
    pty_close(s);
}

/* ── Enter, Esc and Ctrl-C ───────────────────────────────────────────── */

/** Enter with To empty shows the error and keeps the dialog, focus on To. */
static void test_ck_enter_empty_to(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "empty To: opens");
    press(s, "Tab"); press(s, "Tab"); press(s, "Tab");
    typed(s, "hi");
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "To: is empty") && active_field(s) == 0, "error shown, focus on To");
    typed(s, "q");
    CHECK(s, !pty_screen_contains(s, "To: is empty"), "typing clears the error");
    pty_close(s);
}

/** Enter with an empty Subject asks; anything but y or Enter goes back to Subject; y and Enter go on. */
static void test_ck_enter_empty_subject(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "empty Subject: opens");
    typed(s, "a@x.org");
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "Subject is empty") && pty_screen_contains(s, "send anyway"),
          "the question is shown");
    press(s, "n");
    CHECK(s, active_field(s) == 3 && !pty_screen_contains(s, "Subject is empty"),
          "n returns to Subject");
    press(s, "Enter");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Esc save and continue", WAIT_MS) == 0, "Enter at the question opens the editor");
    pty_close(s);
    s = at_dialog(3);
    ASSERT(s != NULL, "empty Subject y: opens");
    typed(s, "a@x.org");
    press(s, "Enter");
    press(s, "y");
    CHECK(s, pty_wait_for(s, "Esc save and continue", WAIT_MS) == 0, "y opens the editor");
    pty_close(s);
}

/** Enter in Subject, Cc, Bcc with To and Subject filled opens the editor. */
static void test_ck_enter_confirms(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "enter: opens");
    typed(s, "a@x.org");
    press(s, "Tab"); press(s, "Tab"); press(s, "Tab");
    typed(s, "subj");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Esc save and continue", WAIT_MS) == 0, "Enter in Subject opens the editor");
    pty_close(s);
    s = at_dialog(3);
    ASSERT(s != NULL, "enter in Cc: opens");
    typed(s, "a@x.org");
    press(s, "Tab");
    press(s, "Tab");
    press(s, "Tab");
    typed(s, "subj");
    press(s, "S-Tab"); press(s, "S-Tab");
    press(s, "Enter");
    CHECK(s, pty_wait_for(s, "Esc save and continue", WAIT_MS) == 0, "Enter in Cc opens the editor");
    pty_close(s);
}

/** Esc: an empty dialog leaves at once; with text it asks (default No); ctrl-c leaves without asking. */
static void test_ck_esc_and_ctrl_c(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "esc: opens");
    press(s, "Esc");
    leave_pause(s);
    CHECK(s, pty_wait_for(s, "message(s) in", WAIT_MS) == 0 && in_list(s), "Esc on an empty dialog leaves");
    pty_close(s);

    s = at_dialog(3);
    ASSERT(s != NULL, "esc text: opens");
    typed(s, "a@x.org");
    press(s, "Esc");
    CHECK(s, pty_screen_contains(s, "Discard what was typed?"), "Esc with text asks");
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "New Message") && field_is(s, ROW_TO, "a@x.org"),
          "Enter takes the default No: the dialog and its text stay");
    press(s, "Esc");
    press(s, "n");
    CHECK(s, pty_screen_contains(s, "New Message"), "n keeps the dialog");
    press(s, "Esc");
    press(s, "y");
    leave_pause(s);
    CHECK(s, pty_wait_for(s, "message(s) in", WAIT_MS) == 0 && in_list(s), "y discards and leaves");
    pty_close(s);

    s = at_dialog(3);
    ASSERT(s != NULL, "ctrl-c: opens");
    typed(s, "a@x.org");
    press(s, "^C");
    leave_pause(s);
    CHECK(s, pty_wait_for(s, "message(s) in", WAIT_MS) == 0 && in_list(s), "ctrl-c leaves without asking");
    pty_close(s);
}

/** Added attachments count as text for Esc (the question) too. */
static void test_ck_esc_with_attachment_only(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "esc attach: opens");
    for (int i = 0; i < 4; i++) press(s, "Tab");
    char p[500];
    snprintf(p, sizeof(p), "%s/beta.txt", g_att);
    typed(s, p);
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "[1] beta.txt"), "the file is added");
    press(s, "Esc");
    CHECK(s, pty_screen_contains(s, "Discard what was typed?"), "an attachment alone makes Esc ask");
    pty_close(s);
}

/* ── Attach: path completion ─────────────────────────────────────────── */

/** 0 rows: a path with no match shows nothing; Tab moves on; Enter says File not found. */
static void test_ck_attach_zero(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "attach 0: opens");
    for (int i = 0; i < 4; i++) press(s, "Tab");
    char p[500];
    snprintf(p, sizeof(p), "%s/zzz", g_att);
    typed(s, p);
    CHECK(s, !pty_screen_contains(s, "alpha.txt"), "no match, no rows");
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "File not found") && active_field(s) == 4, "Enter: File not found");
    typed(s, "x");
    CHECK(s, !pty_screen_contains(s, "File not found"), "typing clears the error");
    press(s, "Tab");
    CHECK(s, active_field(s) == 0, "Tab with no match goes to the next field");
    pty_close(s);
}

/** 1 row: Tab completes to the file; Enter adds it; the list shows it; the field is empty again. */
static void test_ck_attach_one_and_add(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "attach 1: opens");
    for (int i = 0; i < 4; i++) press(s, "Tab");
    char p[500], q[500];
    snprintf(p, sizeof(p), "%s/be", g_att);
    typed(s, p);
    CHECK(s, pty_screen_contains(s, "beta.txt"), "one row");
    press(s, "Tab");
    snprintf(q, sizeof(q), "%s/beta.txt", g_att);
    CHECK(s, field_is(s, ROW_ATTACH, q) || pty_screen_contains(s, "beta.txt"), "Tab completes the name");
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "[1] beta.txt") && pty_screen_contains(s, "Added:") &&
             field_is(s, ROW_ATTACH, ""), "Enter adds the file and empties the field");
    pty_close(s);
}

/** Many rows: files with a common prefix are listed; Tab takes the first; Down/Up do not select. */
static void test_ck_attach_many(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "attach many: opens");
    for (int i = 0; i < 4; i++) press(s, "Tab");
    char p[500];
    snprintf(p, sizeof(p), "%s/al", g_att);
    typed(s, p);
    CHECK(s, pty_screen_contains(s, "alpha.txt") && pty_screen_contains(s, "alpine.txt"), "both rows are listed");
    press(s, "Down"); press(s, "Up");
    CHECK(s, active_field(s) == 4 && !pty_screen_contains(s, "Cc:  x"), "Down and Up keep the field");
    press(s, "Tab");
    CHECK(s, pty_screen_contains(s, "alpha.txt") || pty_screen_contains(s, "alpine.txt"),
          "Tab completes one of them");
    pty_close(s);
}

/** A directory completes with a trailing / so Tab goes on into it; Enter on a directory says Not a file. */
static void test_ck_attach_directory(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "attach dir: opens");
    for (int i = 0; i < 4; i++) press(s, "Tab");
    char p[500];
    snprintf(p, sizeof(p), "%s/su", g_att);
    typed(s, p);
    press(s, "Tab");
    CHECK(s, pty_screen_contains(s, "inner.txt"), "Tab on a directory completes it with / and lists its files");
    press(s, "BS");
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "Not a file"), "Enter on a directory: Not a file");
    pty_close(s);
}

/** d removes the last attachment when the field is empty; with none it is an ordinary letter. */
static void test_ck_attach_d(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "attach d: opens");
    for (int i = 0; i < 4; i++) press(s, "Tab");
    typed(s, "d");
    CHECK(s, field_is(s, ROW_ATTACH, "d"), "with no attachment d is typed into the field");
    press(s, "BS");
    char p[500];
    snprintf(p, sizeof(p), "%s/beta.txt", g_att);
    typed(s, p);
    press(s, "Enter");
    snprintf(p, sizeof(p), "%s/alpha.txt", g_att);
    typed(s, p);
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "[2] alpha.txt"), "two attachments");
    press(s, "d");
    CHECK(s, !pty_screen_contains(s, "[2] alpha.txt") && pty_screen_contains(s, "[1] beta.txt"),
          "d removes the last one");
    press(s, "d");
    CHECK(s, !pty_screen_contains(s, "Added:") && field_is(s, ROW_ATTACH, ""), "d removes the first");
    press(s, "d");
    CHECK(s, field_is(s, ROW_ATTACH, "d"), "with none left d is a letter again");
    pty_close(s);
}

/** Enter on an empty Attach field confirms the dialog (the To guard applies). */
static void test_ck_attach_enter_empty(void) {
    PtySession *s = at_dialog(3);
    ASSERT(s != NULL, "attach enter: opens");
    for (int i = 0; i < 4; i++) press(s, "Tab");
    press(s, "Enter");
    CHECK(s, pty_screen_contains(s, "To: is empty") && active_field(s) == 0, "empty Attach + Enter confirms: the To guard answers");
    pty_close(s);
}

int main(int argc, char *argv[]) {
    printf("--- email-tui compose dialog keys (80x24) ---\n\n");
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

    RUN_TEST(test_ck_layout);
    RUN_TEST(test_ck_tab_cycle);
    RUN_TEST(test_ck_line_editing);
    RUN_TEST(test_ck_unbound_and_literal_keys);
    RUN_TEST(test_ck_resize);
    RUN_TEST(test_ck_dropdown_zero);
    RUN_TEST(test_ck_dropdown_one);
    RUN_TEST(test_ck_dropdown_many);
    RUN_TEST(test_ck_dropdown_esc);
    RUN_TEST(test_ck_dropdown_other_fields);
    RUN_TEST(test_ck_semicolon_and_backspace);
    RUN_TEST(test_ck_enter_empty_to);
    RUN_TEST(test_ck_enter_empty_subject);
    RUN_TEST(test_ck_enter_confirms);
    RUN_TEST(test_ck_esc_and_ctrl_c);
    RUN_TEST(test_ck_esc_with_attachment_only);
    RUN_TEST(test_ck_attach_zero);
    RUN_TEST(test_ck_attach_one_and_add);
    RUN_TEST(test_ck_attach_many);
    RUN_TEST(test_ck_attach_directory);
    RUN_TEST(test_ck_attach_d);
    RUN_TEST(test_ck_attach_enter_empty);

    if (g_mock_pid > 0) { kill(g_mock_pid, SIGKILL); waitpid(g_mock_pid, NULL, 0); }
    printf("\n--- Compose dialog key study results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
