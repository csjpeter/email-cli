/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-24 */
/* Unit tests of the terminal input functions, run on a pseudo-terminal. */
#include "test_helpers.h"
#include "pty_stdio.h"
#include "platform/terminal.h"

static char g_out[8192];

/** terminal_read_line() on a pty that is typed @p input.  Returns its result. */
static int line_on_pty(const char *input, int mask, char *buf, size_t size) {
    PtyStdio p;
    if (pty_stdio_begin(&p, 24, 80, input) != 0) return -99;
    int rc = terminal_read_line("Name", mask, buf, size);
    pty_stdio_end(&p, g_out, sizeof(g_out));
    return rc;
}

static void test_term_line_typed_and_erased(void) {
    char buf[64];
    int rc = line_on_pty("ab\x7f" "c\r", 0, buf, sizeof(buf));
    ASSERT(rc == 2 && strcmp(buf, "ac") == 0, "a typed line with a Backspace is returned");
    ASSERT(strstr(g_out, "Name: ") != NULL, "the prompt is written with a colon");
    ASSERT(strstr(g_out, "\b \b") != NULL, "the erased character is wiped from the screen");
    ASSERT(strstr(g_out, "ab") != NULL, "the typed characters are echoed");
}

static void test_term_line_masked(void) {
    char buf[64];
    int rc = line_on_pty("pw\r", 1, buf, sizeof(buf));
    ASSERT(rc == 2 && strcmp(buf, "pw") == 0, "a masked line is returned in clear to the caller");
    ASSERT(strstr(g_out, "**") != NULL, "one star per character is shown");
    ASSERT(strstr(g_out, "pw") == NULL, "the secret is never echoed");
    rc = line_on_pty("ab\x7f\r", 1, buf, sizeof(buf));
    ASSERT(rc == 1 && strcmp(buf, "a") == 0, "Backspace in a masked line removes one character");
}

static void test_term_line_cancel_quit_eof(void) {
    char buf[64];
    int rc = line_on_pty("x\033", 0, buf, sizeof(buf));
    ASSERT(rc == TERM_LINE_CANCEL && buf[0] == '\0', "Esc cancels and the buffer is wiped");
    rc = line_on_pty("x\003", 0, buf, sizeof(buf));
    ASSERT(rc == TERM_LINE_QUIT && buf[0] == '\0', "Ctrl-C asks to quit and the buffer is wiped");
    ASSERT(strstr(g_out, "^C") != NULL, "Ctrl-C is shown");
    rc = line_on_pty("\004", 0, buf, sizeof(buf));
    ASSERT(rc == TERM_LINE_EOF, "Ctrl-D on an empty line is end of input");
    rc = line_on_pty("a\004" "b\r", 0, buf, sizeof(buf));
    ASSERT(rc == 2 && strcmp(buf, "ab") == 0, "Ctrl-D in a non-empty line is ignored");
    rc = terminal_read_line("x", 0, NULL, 0);
    ASSERT(rc == TERM_LINE_EOF, "no buffer: end of input");
}

static void test_term_line_kill_overflow_utf8(void) {
    char buf[64];
    int rc = line_on_pty("abc\025" "d\r", 0, buf, sizeof(buf));
    ASSERT(rc == 1 && strcmp(buf, "d") == 0, "Ctrl-U wipes the line typed so far");
    char small[4];
    rc = line_on_pty("abcdef\r", 0, small, sizeof(small));
    ASSERT(rc == 3 && strcmp(small, "abc") == 0, "input beyond the buffer is not taken");
    ASSERT(strchr(g_out, '\a') != NULL, "the bell rings for the refused character");
    rc = line_on_pty("\xc3\xa9\x7f" "x\r", 0, buf, sizeof(buf));
    ASSERT(rc == 1 && strcmp(buf, "x") == 0, "Backspace removes a whole UTF-8 character");
    rc = line_on_pty("\xe4\xb8\xad\x7f\r", 0, buf, sizeof(buf));
    ASSERT(rc == 0 && buf[0] == '\0', "Backspace removes a wide character");
    const char *e = strstr(g_out, "\b \b");
    ASSERT(e && strstr(e + 3, "\b \b") != NULL, "a wide character is erased from both its columns");
    rc = line_on_pty("\x7f\x1b[Aok\r", 0, buf, sizeof(buf));
    ASSERT(rc == 2 && strcmp(buf, "ok") == 0, "Backspace on an empty line and an arrow key change nothing");
}

static void test_term_password_on_tty(void) {
    char buf[64];
    PtyStdio p;
    ASSERT(pty_stdio_begin(&p, 24, 80, "secret\r") == 0, "pty");
    int n = terminal_read_password("Password", buf, sizeof(buf));
    pty_stdio_end(&p, g_out, sizeof(g_out));
    ASSERT(n == 6 && strcmp(buf, "secret") == 0, "the password typed on a terminal is returned");
    ASSERT(strstr(g_out, "secret") == NULL, "the password is not echoed");
    ASSERT(strstr(g_out, "Password") != NULL, "the prompt is shown");
    ASSERT(pty_stdio_begin(&p, 24, 80, "abc\003") == 0, "pty");
    n = terminal_read_password("Password", buf, sizeof(buf));
    pty_stdio_end(&p, g_out, sizeof(g_out));
    ASSERT(n < 0 && buf[0] == '\0', "Ctrl-C gives an error and the buffer is wiped");
}

static TermKey key_on_pty(int rows, int cols, const char *input) {
    PtyStdio p;
    if (pty_stdio_begin(&p, rows, cols, input) != 0) return TERM_KEY_QUIT;
    TermKey k = terminal_read_key();
    pty_stdio_end(&p, g_out, sizeof(g_out));
    return k;
}

static void test_term_key_on_tty(void) {
    ASSERT(key_on_pty(24, 80, "\r") == TERM_KEY_ENTER, "Enter on a terminal");
    ASSERT(key_on_pty(24, 80, "\003") == TERM_KEY_QUIT, "Ctrl-C is quit");
    ASSERT(key_on_pty(24, 80, "\001") == TERM_KEY_IGNORE, "another Ctrl key is ignored");
    ASSERT(key_on_pty(24, 80, "\033x") == TERM_KEY_ESC, "Alt+character is Esc");
    ASSERT(key_on_pty(24, 80, "\t") == TERM_KEY_TAB, "Tab");
    ASSERT(key_on_pty(24, 80, "\x7f") == TERM_KEY_BACK, "Backspace");
    ASSERT(key_on_pty(24, 80, "\033[1;5C") == TERM_KEY_IGNORE, "a modified arrow is dropped");
    ASSERT(key_on_pty(24, 80, "\033[2~") == TERM_KEY_IGNORE, "Insert is not a key of the program");
    ASSERT(key_on_pty(24, 80, "\xc3\xa9") == TERM_KEY_IGNORE &&
           strcmp(terminal_last_utf8(), "\xc3\xa9") == 0 && terminal_last_printable() == 1,
           "a non-ASCII character is reported as text, not as a key");
    ASSERT(key_on_pty(24, 80, "q") == TERM_KEY_IGNORE && terminal_last_printable() == 'q',
           "a printable ASCII character is reported as typed");
}

static void test_term_key_too_small(void) {
    TermKey k = key_on_pty(10, 40, "a\033");
    ASSERT(k == TERM_KEY_ESC, "on a too small terminal Esc still gets through");
    ASSERT(strstr(g_out, "Terminal too small (40x10)") != NULL, "the notice names the size");
    ASSERT(strstr(g_out, "Needs 80x24") != NULL, "the notice names the needed size");
    k = key_on_pty(10, 40, "b\003");
    ASSERT(k == TERM_KEY_QUIT, "on a too small terminal Ctrl-C still quits");
}

static void test_term_size_and_quit_flag(void) {
    PtyStdio p;
    ASSERT(pty_stdio_begin(&p, 31, 101, NULL) == 0, "pty");
    int c = terminal_cols(), r = terminal_rows();
    int tty = terminal_is_tty(STDIN_FILENO);
    pty_stdio_end(&p, g_out, sizeof(g_out));
    ASSERT(c == 101 && r == 31, "the size of the terminal is read from it");
    ASSERT(tty == 1, "a pty is a tty");

    terminal_clear_quit_request();
    ASSERT(terminal_quit_requested() == 0, "no quit requested at first");
    terminal_request_quit();
    ASSERT(terminal_quit_requested() == 1, "the request is remembered");
    ASSERT(key_on_pty(24, 80, NULL) == TERM_KEY_QUIT, "a pending quit request is returned as quit");
    terminal_clear_quit_request();
    ASSERT(terminal_quit_requested() == 0, "the request can be withdrawn");
}

static void test_term_raw_exit_shows_cursor(void) {
    PtyStdio p;
    ASSERT(pty_stdio_begin(&p, 24, 80, NULL) == 0, "pty");
    TermRawState *st = terminal_raw_enter();
    int entered = st != NULL;
    terminal_raw_exit(&st);
    pty_stdio_end(&p, g_out, sizeof(g_out));
    ASSERT(entered, "raw mode is entered on a terminal");
    ASSERT(strstr(g_out, "\033[?25h") != NULL, "leaving raw mode gives the cursor back");
}

static void test_term_resize_returns_ignored(void) {
    PtyStdio p;
    ASSERT(pty_stdio_begin(&p, 24, 80, NULL) == 0, "pty");
    pid_t parent = getpid();
    pid_t kid = fork();
    if (kid == 0) {
        struct timespec ts = { 0, 300 * 1000000L };
        nanosleep(&ts, NULL);
        kill(parent, SIGWINCH);
        _exit(0);
    }
    TermKey k = terminal_read_key();
    int st;
    waitpid(kid, &st, 0);
    pty_stdio_end(&p, g_out, sizeof(g_out));
    ASSERT(k == TERM_KEY_IGNORE, "a resize wakes the wait and asks the caller to redraw");
}

void test_terminal_io(void) {
    RUN_TEST(test_term_line_typed_and_erased);
    RUN_TEST(test_term_line_masked);
    RUN_TEST(test_term_line_cancel_quit_eof);
    RUN_TEST(test_term_line_kill_overflow_utf8);
    RUN_TEST(test_term_password_on_tty);
    RUN_TEST(test_term_key_on_tty);
    RUN_TEST(test_term_key_too_small);
    RUN_TEST(test_term_size_and_quit_flag);
    RUN_TEST(test_term_raw_exit_shows_cursor);
    RUN_TEST(test_term_resize_returns_ignored);
}
