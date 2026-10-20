/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-20 */
/* PTY key study (EMAIL-151) of the body editor and the post-compose review. */
/**
 * @file test_pty_review_keys.c
 * @brief One PTY test per key of the body editor and of the review screen,
 *        at 80x24 against the mock servers (EMAIL-151, study EMAIL-140).
 *        The key tables are in docs/spec/tui-keys.md.
 *
 * Usage: test-pty-review-keys <email-tui> <mock-imap-server> <mock-smtp-server>
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ptytest.h"
#include "pty_assert.h"
#include "test_scratch.h"
#include "pty_editor.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int g_tests_run    = 0;
int g_tests_failed = 0;

#define RUN_TEST(fn) do { if (!test_selected(#fn)) break; printf("  %s...\n", #fn); \
                          fflush(stdout); fn(); } while (0)

#define ROWS 24
#define COLS 80
#define WAIT_MS 6000
#define SETTLE_MS 300
#define TEST_ACCOUNT "testuser@example.com"
#define CTRL_S "\x13"

static char g_tui_bin[512], g_imap_bin[512], g_smtp_bin[512];
static char g_home[512], g_old_home[512];
static char g_attach[600];
static pid_t g_imap_pid = -1, g_smtp_pid = -1;
static int g_imap_port = 0, g_smtp_port = 0;

/* ── Fixture ─────────────────────────────────────────────────────────── */

static void mk(const char *rel) {
    char d[700];
    snprintf(d, sizeof(d), "%s/%s", g_home, rel);
    mkdir(d, 0700);
}

static void write_config(void) {
    mk(".config"); mk(".config/email-cli"); mk(".config/email-cli/accounts");
    mk(".config/email-cli/accounts/testuser");
    mk(".local"); mk(".local/share"); mk(".local/share/email-cli");
    mk(".local/share/email-cli/accounts");
    mk(".local/share/email-cli/accounts/" TEST_ACCOUNT);
    mk(".local/share/email-cli/accounts/" TEST_ACCOUNT "/manifests");
    mk("attachments");
    char path[700];
    snprintf(path, sizeof(path), "%s/.config/email-cli/accounts/testuser/config.ini", g_home);
    FILE *fp = fopen(path, "w");
    if (!fp) { fprintf(stderr, "review-keys: cannot write %s\n", path); g_tests_failed++; return; }
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:%d\nEMAIL_USER=" TEST_ACCOUNT "\nEMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\nSMTP_HOST=smtps://localhost:%d\nSMTP_PORT=%d\n"
        "SMTP_USER=" TEST_ACCOUNT "\nSMTP_PASS=testpass\nSSL_NO_VERIFY=1\n",
        g_imap_port, g_smtp_port, g_smtp_port);
    fclose(fp);
    chmod(path, 0600);
    snprintf(g_attach, sizeof(g_attach), "%s/attachments/notes.txt", g_home);
    fp = fopen(g_attach, "w");
    if (fp) { fputs("notes\n", fp); fclose(fp); }
}

static void start(const char *bin, const char *prefix, pid_t *pid, int *port) {
    int bound = 0;
    *pid = test_spawn_mock(bin, prefix, *port, &bound);
    if (*pid < 0) { fprintf(stderr, "review-keys: %s did not start\n", bin); g_tests_failed++; return; }
    *port = bound;
}

static void stop(pid_t *pid) {
    if (*pid > 0) { kill(*pid, SIGKILL); waitpid(*pid, NULL, 0); *pid = -1; }
}

static void draft_path(char *buf, size_t n) {
    snprintf(buf, n, "%s/.local/share/email-cli/compose-draft", g_home);
}

static char g_draft[16384];
static const char *draft_text(void) {
    char p[700];
    draft_path(p, sizeof(p));
    g_draft[0] = '\0';
    FILE *f = fopen(p, "r");
    if (!f) return g_draft;
    size_t n = fread(g_draft, 1, sizeof(g_draft) - 1, f);
    g_draft[n] = '\0';
    fclose(f);
    return g_draft;
}
static int draft_has(const char *s) { return strstr(draft_text(), s) != NULL; }

static void fresh(void) {
    pty_editor_forget_draft(g_home);
    stop(&g_imap_pid);
    start(g_imap_bin, "MOCK_IMAP", &g_imap_pid, &g_imap_port);
}

static PtySession *inbox(void) {
    write_config();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    if (pty_wait_for(s, "testuser", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) { pty_send_key(s, PTY_KEY_DOWN); pty_settle(s, 50); }
    if (pty_wait_for(s, "INBOX", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "message(s) in", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* Inbox, `c`, a filled dialog, Enter: the body editor is on screen. */
static PtySession *editor(const char *subject) {
    PtySession *s = inbox();
    if (!s) return NULL;
    pty_send_str(s, "c");
    if (pty_wait_for(s, "New Message", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB); pty_send_key(s, PTY_KEY_TAB); pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, subject);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, PTY_EDITOR_MARK, WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, 150);
    return s;
}

/* The same, the editor left with Esc: the review screen is on screen. */
static PtySession *review(const char *subject, const char *text) {
    PtySession *s = editor(subject);
    if (!s) return NULL;
    if (text && text[0]) { pty_send_str(s, text); pty_settle(s, 150); }
    pty_send_key(s, PTY_KEY_ESC);
    if (pty_wait_for(s, "actions (send)", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, 150);
    return s;
}

/* Leave the review: q, y. */
static void discard(PtySession *s) {
    pty_send_str(s, "q");
    pty_wait_for(s, "Discard draft?", WAIT_MS);
    pty_send_str(s, "y");
    pty_settle(s, 200);
}

/* ══ Body editor ═════════════════════════════════════════════════════ */

static void test_ed_layout(void) {
    fresh();
    PtySession *s = editor("Layout");
    ASSERT(s != NULL, "ed layout: opens");
    ASSERT_ROW_CONTAINS(s, 0, "Message body");
    ASSERT_ROW_CONTAINS(s, ROWS - 1, "Esc save and continue");
    ASSERT_ROW_CONTAINS(s, ROWS - 1, "Ctrl-S save");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    discard(s);
    pty_close(s);
}

static void test_ed_empty_body_esc(void) {
    fresh();
    PtySession *s = editor("Empty");           /* 0 lines */
    ASSERT(s != NULL, "ed empty: opens");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Body:    0 lines", WAIT_MS);
    discard(s);
    pty_close(s);
}

static void test_ed_cursor_and_delete_keys(void) {
    fresh();
    PtySession *s = editor("Cursor");
    ASSERT(s != NULL, "ed cursor: opens");
    pty_send_str(s, "hello");
    pty_send_key(s, PTY_KEY_HOME);
    pty_send_str(s, "A");
    pty_send_key(s, PTY_KEY_END);
    pty_send_str(s, "Z");
    pty_send_key(s, PTY_KEY_LEFT);
    pty_send_key(s, PTY_KEY_LEFT);
    pty_send_key(s, PTY_KEY_DELETE);            /* removes the o */
    pty_send_key(s, PTY_KEY_BACK);              /* removes the second l */
    pty_send_key(s, PTY_KEY_RIGHT);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_send_str(s, "two");
    pty_send_key(s, PTY_KEY_UP);
    pty_send_key(s, PTY_KEY_END);
    pty_send_str(s, "!");
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_HOME);
    pty_send_str(s, ">");
    pty_send_str(s, CTRL_S);
    ASSERT_WAIT_FOR(s, "saved", WAIT_MS);
    ASSERT(draft_has("AhelZ!\n>two"), "ed cursor: Home End Left Right Up Down Delete Backspace Enter");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    discard(s);
    pty_close(s);
}

static void test_ed_many_lines_paging(void) {
    fresh();
    PtySession *s = editor("Many");
    ASSERT(s != NULL, "ed many: opens");
    for (int i = 1; i <= 60; i++) {
        char l[32];
        snprintf(l, sizeof(l), "line%02d\r", i);
        pty_send_str(s, l);
    }
    ASSERT_WAIT_FOR(s, "line60", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "line01");
    for (int i = 0; i < 4; i++) pty_send_key(s, PTY_KEY_PGUP);   /* one page each */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "line01");
    for (int i = 0; i < 4; i++) pty_send_key(s, PTY_KEY_PGDN);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "line60");
    ASSERT_SCREEN_NOT_CONTAINS(s, "line01");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Body:    60 lines", WAIT_MS);
    discard(s);
    pty_close(s);
}

static void test_ed_ctrl_s_note(void) {
    fresh();
    PtySession *s = editor("CtrlS");
    ASSERT(s != NULL, "ed ctrl-s: opens");
    pty_send_str(s, "one");
    pty_send_str(s, CTRL_S);
    ASSERT_WAIT_FOR(s, "saved", WAIT_MS);
    ASSERT_ROW_CONTAINS(s, ROWS - 1, "saved");
    ASSERT(draft_has("one"), "ed ctrl-s: written");
    pty_send_str(s, "x");                       /* the note goes with the next redraw */
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, ROWS - 1, "Esc save and continue");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    discard(s);
    pty_close(s);
}

static void test_ed_esc_saves_and_leaves(void) {
    fresh();
    PtySession *s = editor("EscKey");
    ASSERT(s != NULL, "ed esc: opens");
    pty_send_str(s, "text");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Body:    1 line", WAIT_MS);
    ASSERT(draft_has("text"), "ed esc: saved");
    discard(s);
    pty_close(s);
}

static void test_ed_ctrl_c(void) {
    fresh();
    PtySession *s = editor("CtrlC");
    ASSERT(s != NULL, "ed ctrl-c: opens");
    pty_send_str(s, "kept");
    pty_settle(s, 150);
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, WAIT_MS) >= 0, "ed ctrl-c: the program quits at once");
    ASSERT(draft_has("kept"), "ed ctrl-c: the text was saved");
    pty_close(s);
}

static void test_ed_unbound_keys_are_text(void) {
    fresh();
    PtySession *s = editor("Plain");
    ASSERT(s != NULL, "ed plain: opens");
    pty_send_str(s, "q?:sebad");
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "q?:sebad");      /* no help, no menu: a letter is a letter */
    ASSERT_ROW_CONTAINS(s, ROWS - 1, "Esc save and continue");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    discard(s);
    pty_close(s);
}

static void test_ed_resize(void) {
    fresh();
    PtySession *s = editor("Resize");
    ASSERT(s != NULL, "ed resize: opens");
    pty_send_str(s, "kept text");
    pty_resize(s, 60, 20);
    ASSERT_WAIT_FOR(s, "Terminal too small", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);               /* Esc still saves and leaves */
    pty_settle(s, SETTLE_MS);
    pty_resize(s, 80, 24);
    ASSERT_WAIT_FOR(s, "Body:    1 line", WAIT_MS);
    discard(s);
    pty_close(s);
}

/* ══ Review screen ═══════════════════════════════════════════════════ */

static void test_rv_layout(void) {
    fresh();
    PtySession *s = review("Layout review", "a\rb");
    ASSERT(s != NULL, "rv layout: reached");
    ASSERT_ROW_CONTAINS(s, 0, "Review: Message");
    ASSERT_SCREEN_CONTAINS(s, "From:");
    ASSERT_SCREEN_CONTAINS(s, "alice@example.com");
    ASSERT_SCREEN_CONTAINS(s, "Layout review");
    ASSERT_SCREEN_CONTAINS(s, "Body:    2 lines");
    ASSERT_SCREEN_CONTAINS(s, "(none)");
    ASSERT_ROW_CONTAINS(s, ROWS - 1, "actions (send)");
    ASSERT_ROW_CONTAINS(s, ROWS - 1, "q discard");
    discard(s);
    pty_close(s);
}

static void test_rv_e_edit_headers(void) {
    fresh();
    PtySession *s = review("Edit me", "x");
    ASSERT(s != NULL, "rv e: reached");
    pty_send_str(s, "e");
    ASSERT_WAIT_FOR(s, "Edit Message", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Edit me");
    pty_send_key(s, PTY_KEY_ESC);               /* asks before it drops the edit */
    ASSERT_WAIT_FOR(s, "Discard what was typed?", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    discard(s);
    pty_close(s);
}

static void test_rv_b_edit_body(void) {
    fresh();
    PtySession *s = review("Body again", "one");
    ASSERT(s != NULL, "rv b: reached");
    pty_send_str(s, "b");
    ASSERT_WAIT_FOR(s, PTY_EDITOR_MARK, WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "one");           /* the text is kept */
    pty_send_str(s, "\rtwo");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Body:    2 lines", WAIT_MS);
    discard(s);
    pty_close(s);
}

static void test_rv_a_attach_prompt(void) {
    fresh();
    PtySession *s = review("Attach", "x");
    ASSERT(s != NULL, "rv a: reached");
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Add Attachment", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Enter add", WAIT_MS);       /* the status line is drawn after the title */
    ASSERT_ROW_CONTAINS(s, ROWS - 1, "Enter add");
    ASSERT_ROW_CONTAINS(s, ROWS - 1, "Esc cancel");
    pty_send_key(s, PTY_KEY_ENTER);             /* empty path: cancels */
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Add Attachment", WAIT_MS);
    pty_send_str(s, "/no/such/file");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "File not found", WAIT_MS);
    pty_send_key(s, PTY_KEY_BACK);              /* editing clears the error */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "File not found");
    pty_send_key(s, PTY_KEY_ESC);               /* Esc cancels */
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "(none)");
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Add Attachment", WAIT_MS);
    pty_send_str(s, g_attach);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "notes.txt", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "(none)");
    discard(s);
    pty_close(s);
}

static void test_rv_a_tab_completes(void) {
    fresh();
    PtySession *s = review("Complete", "x");
    ASSERT(s != NULL, "rv a tab: reached");
    pty_send_str(s, "a");
    ASSERT_WAIT_FOR(s, "Add Attachment", WAIT_MS);
    char dir[700];
    snprintf(dir, sizeof(dir), "%s/attachments/not", g_home);
    pty_send_str(s, dir);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "notes.txt");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Attach:  notes.txt", WAIT_MS);
    discard(s);
    pty_close(s);
}

static void test_rv_colon_menu(void) {
    fresh();
    PtySession *s = review("Menu", "x");
    ASSERT(s != NULL, "rv menu: reached");
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Send");
    ASSERT_SCREEN_CONTAINS(s, "Remove an attachment");
    pty_send_key(s, PTY_KEY_ESC);               /* closes the menu only */
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "Review: Message");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Discard draft?");
    pty_send_str(s, ":");                       /* Remove with nothing attached: nothing */
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_DOWN); pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "(none)");
    ASSERT_ROW_CONTAINS(s, 0, "Review: Message");
    discard(s);
    pty_close(s);
}

static void test_rv_send_asks_and_declines(void) {
    fresh();
    PtySession *s = review("Send ask", "x");
    ASSERT(s != NULL, "rv send: reached");
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "alice@example.com", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Yes", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);               /* no sending without the yes */
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "Review: Message");
    ASSERT(draft_has("x"), "rv send: declined keeps the draft");
    discard(s);
    pty_close(s);
}

static void test_rv_send_yes(void) {
    fresh();
    PtySession *s = review("Send yes", "x");
    ASSERT(s != NULL, "rv send yes: reached");
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Yes", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Message sent", WAIT_MS);
    ASSERT(!draft_has("Subject:"), "rv send yes: the draft is removed");
    pty_close(s);
}

static void test_rv_help(void) {
    fresh();
    PtySession *s = review("Help", "x");
    ASSERT(s != NULL, "rv help: reached");
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Review shortcuts", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Edit headers");
    ASSERT_SCREEN_CONTAINS(s, "Edit body");
    ASSERT_SCREEN_CONTAINS(s, "Add attachment");
    ASSERT_SCREEN_CONTAINS(s, "Discard");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Review shortcuts");
    ASSERT_ROW_CONTAINS(s, 0, "Review: Message");
    discard(s);
    pty_close(s);
}

static void test_rv_discard_keys(void) {
    fresh();
    PtySession *s = review("Discard keys", "x");
    ASSERT(s != NULL, "rv discard: reached");
    const char *stay[] = { "n", "N" };
    for (int i = 0; i < 2; i++) {
        pty_send_str(s, "q");
        ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
        pty_send_str(s, stay[i]);
        ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    }
    pty_send_key(s, PTY_KEY_ESC);               /* Esc is q */
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);               /* Esc at the question: stay */
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Discard keys");
    ASSERT_SCREEN_CONTAINS(s, "alice@example.com");
    pty_send_str(s, "Y");                       /* capital Y discards */
    ASSERT_WAIT_FOR(s, "Cancelled. Draft discarded.", WAIT_MS);
    ASSERT(!draft_has("Subject:"), "rv discard: the draft is removed");
    pty_close(s);
}

static void test_rv_ctrl_c(void) {
    fresh();
    PtySession *s = review("Ctrl-C", "x");
    ASSERT(s != NULL, "rv ctrl-c: reached");
    pty_send_key(s, PTY_KEY_CTRL_C);
    ASSERT(pty_wait_exit(s, WAIT_MS) >= 0, "rv ctrl-c: the program quits at once");
    ASSERT(draft_has("x"), "rv ctrl-c: the draft stays");
    pty_close(s);
}

static void test_rv_unbound_keys(void) {
    fresh();
    PtySession *s = review("Unbound", "x");
    ASSERT(s != NULL, "rv unbound: reached");
    const char *keys[] = { "s", "d", "x", "S", "j", " " };
    for (int i = 0; i < 6; i++) {
        pty_send_str(s, keys[i]);
        pty_settle(s, 150);
    }
    pty_send_key(s, PTY_KEY_UP); pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_ENTER); pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "Review: Message");
    ASSERT_ROW_CONTAINS(s, ROWS - 1, "actions (send)");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Discard draft?");
    discard(s);
    pty_close(s);
}

static void test_rv_resize(void) {
    fresh();
    PtySession *s = review("Resize review", "x");
    ASSERT(s != NULL, "rv resize: reached");
    pty_resize(s, 100, 30);
    pty_settle(s, SETTLE_MS * 2);
    ASSERT_ROW_CONTAINS(s, 0, "Review: Message");
    pty_resize(s, 80, 24);
    pty_settle(s, SETTLE_MS * 2);
    ASSERT_SCREEN_CONTAINS(s, "Resize review");
    discard(s);
    pty_close(s);
}

int main(int argc, char *argv[]) {
    printf("--- Body editor and review screen key tests (EMAIL-151) ---\n\n");
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <email-tui> <mock-imap-server> <mock-smtp-server>\n", argv[0]);
        return 1;
    }
    snprintf(g_tui_bin, sizeof(g_tui_bin), "%s", argv[1]);
    snprintf(g_imap_bin, sizeof(g_imap_bin), "%s", argv[2]);
    snprintf(g_smtp_bin, sizeof(g_smtp_bin), "%s", argv[3]);
    snprintf(g_home, sizeof(g_home), "%s", TP("home"));
    if (mkdir(g_home, 0700) != 0) { perror("mkdir home"); return 1; }
    if (getenv("HOME")) snprintf(g_old_home, sizeof(g_old_home), "%s", getenv("HOME"));
    setenv("HOME", g_home, 1);
    unsetenv("XDG_CONFIG_HOME"); unsetenv("XDG_DATA_HOME"); unsetenv("XDG_CACHE_HOME");
    start(g_imap_bin, "MOCK_IMAP", &g_imap_pid, &g_imap_port);
    start(g_smtp_bin, "MOCK_SMTP", &g_smtp_pid, &g_smtp_port);
    if (g_imap_pid < 0 || g_smtp_pid < 0) return 1;

    printf("--- Body editor ---\n");
    RUN_TEST(test_ed_layout);
    RUN_TEST(test_ed_empty_body_esc);
    RUN_TEST(test_ed_cursor_and_delete_keys);
    RUN_TEST(test_ed_many_lines_paging);
    RUN_TEST(test_ed_ctrl_s_note);
    RUN_TEST(test_ed_esc_saves_and_leaves);
    RUN_TEST(test_ed_ctrl_c);
    RUN_TEST(test_ed_unbound_keys_are_text);
    RUN_TEST(test_ed_resize);

    printf("\n--- Review screen ---\n");
    RUN_TEST(test_rv_layout);
    RUN_TEST(test_rv_e_edit_headers);
    RUN_TEST(test_rv_b_edit_body);
    RUN_TEST(test_rv_a_attach_prompt);
    RUN_TEST(test_rv_a_tab_completes);
    RUN_TEST(test_rv_colon_menu);
    RUN_TEST(test_rv_send_asks_and_declines);
    RUN_TEST(test_rv_send_yes);
    RUN_TEST(test_rv_help);
    RUN_TEST(test_rv_discard_keys);
    RUN_TEST(test_rv_ctrl_c);
    RUN_TEST(test_rv_unbound_keys);
    RUN_TEST(test_rv_resize);

    stop(&g_imap_pid);
    stop(&g_smtp_pid);
    if (g_old_home[0]) setenv("HOME", g_old_home, 1);
    printf("\n--- Results ---\nTests Run:    %d\nTests Passed: %d\nTests Failed: %d\n",
           g_tests_run, g_tests_run - g_tests_failed, g_tests_failed);
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
