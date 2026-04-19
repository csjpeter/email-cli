/**
 * @file test_ptytest.c
 * @brief Self-tests for libptytest: screen buffer, VT100 parser, PTY session.
 *
 * Build:  cc -std=c11 -o test_ptytest test_ptytest.c pty_session.c pty_screen.c pty_sync.c -lutil
 * Run:    ./test_ptytest
 */

#include "ptytest.h"
#include "pty_internal.h"
#include "pty_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_tests_run = 0;
int g_tests_failed = 0;

/* ── Screen buffer unit tests ────────────────────────────────────────── */

static void test_screen_basic_write(void) {
    PtyScreen *scr = pty_screen_new(20, 5);
    pty_screen_feed(scr, "Hello", 5);

    ASSERT(scr->cells[0].ch[0] == 'H', "cell(0,0) = H");
    ASSERT(scr->cells[4].ch[0] == 'o', "cell(0,4) = o");
    ASSERT(scr->cur_col == 5, "cursor at col 5 after 5 chars");
    ASSERT(scr->cur_row == 0, "cursor at row 0");

    pty_screen_free(scr);
}

static void test_screen_newline(void) {
    PtyScreen *scr = pty_screen_new(20, 5);
    pty_screen_feed(scr, "AB\nCD", 5);

    ASSERT(scr->cells[0].ch[0] == 'A', "row 0: A");
    ASSERT(scr->cells[1].ch[0] == 'B', "row 0: B");
    /* \n moves to next row, cursor col stays (no \r) */
    ASSERT(scr->cells[1 * 20 + 2].ch[0] == 'C', "row 1 col 2: C");

    pty_screen_free(scr);
}

static void test_screen_cr_lf(void) {
    PtyScreen *scr = pty_screen_new(20, 5);
    pty_screen_feed(scr, "AB\r\nCD", 6);

    ASSERT(scr->cells[1 * 20 + 0].ch[0] == 'C', "row 1 col 0: C after CR+LF");
    ASSERT(scr->cells[1 * 20 + 1].ch[0] == 'D', "row 1 col 1: D");

    pty_screen_free(scr);
}

static void test_screen_cursor_move(void) {
    PtyScreen *scr = pty_screen_new(20, 5);
    /* Move cursor to row 3, col 5 (1-based: 4,6) */
    pty_screen_feed(scr, "\033[4;6H*", 7);

    ASSERT(scr->cells[3 * 20 + 5].ch[0] == '*', "CSI H: * at row 3 col 5");

    pty_screen_free(scr);
}

static void test_screen_erase_display(void) {
    PtyScreen *scr = pty_screen_new(10, 3);
    pty_screen_feed(scr, "ABCDE", 5);
    pty_screen_feed(scr, "\033[2J", 4);

    ASSERT(scr->cells[0].ch[0] == ' ', "erase display: cell 0 is space");
    ASSERT(scr->cells[4].ch[0] == ' ', "erase display: cell 4 is space");

    pty_screen_free(scr);
}

static void test_screen_sgr_reverse(void) {
    PtyScreen *scr = pty_screen_new(20, 5);
    pty_screen_feed(scr, "\033[7mX\033[0mY", 10);

    ASSERT(scr->cells[0].attr & PTY_ATTR_REVERSE, "X has REVERSE attr");
    ASSERT((scr->cells[1].attr & PTY_ATTR_REVERSE) == 0, "Y has no REVERSE attr");

    pty_screen_free(scr);
}

static void test_screen_sgr_bold_dim(void) {
    PtyScreen *scr = pty_screen_new(20, 5);
    pty_screen_feed(scr, "\033[1mB\033[2mD\033[0mN", 15);

    ASSERT(scr->cells[0].attr & PTY_ATTR_BOLD, "B has BOLD");
    ASSERT(scr->cells[1].attr & PTY_ATTR_DIM, "D has DIM");
    ASSERT(scr->cells[1].attr & PTY_ATTR_BOLD, "D also has BOLD (cumulative)");
    ASSERT(scr->cells[2].attr == PTY_ATTR_NONE, "N has NONE after reset");

    pty_screen_free(scr);
}

static void test_screen_sgr_strikethrough(void) {
    PtyScreen *scr = pty_screen_new(20, 5);
    pty_screen_feed(scr, "\033[9mX\033[0mY", 10);

    ASSERT(scr->cells[0].attr & PTY_ATTR_STRIKE, "X has STRIKE attr");
    ASSERT((scr->cells[1].attr & PTY_ATTR_STRIKE) == 0, "Y has no STRIKE after reset");

    pty_screen_free(scr);
}

static void test_screen_erase_line(void) {
    PtyScreen *scr = pty_screen_new(10, 3);
    pty_screen_feed(scr, "0123456789", 10);
    /* Move to col 5, erase to end of line */
    const char *el_seq = "\033[1;6H\033[K";
    pty_screen_feed(scr, el_seq, strlen(el_seq));

    ASSERT(scr->cells[4].ch[0] == '4', "col 4 preserved");
    ASSERT(scr->cells[5].ch[0] == ' ', "col 5 erased");
    ASSERT(scr->cells[9].ch[0] == ' ', "col 9 erased");

    pty_screen_free(scr);
}

static void test_screen_scroll(void) {
    PtyScreen *scr = pty_screen_new(10, 3);
    pty_screen_feed(scr, "AAA\r\nBBB\r\nCCC\r\nDDD", 19);

    /* After 4 lines in a 3-row screen, first line should have scrolled off */
    char buf[64];
    /* Row 0 should now be BBB (AAA scrolled off) */
    ASSERT(scr->cells[0].ch[0] == 'B', "scroll: row 0 = B (was A)");
    /* Row 2 should be DDD */
    ASSERT(scr->cells[2 * 10].ch[0] == 'D', "scroll: row 2 = D");

    pty_screen_free(scr);
    (void)buf;
}

static void test_screen_24bit_color_skip(void) {
    PtyScreen *scr = pty_screen_new(20, 5);
    /* 24-bit fg + bg colour then text */
    pty_screen_feed(scr, "\033[38;2;255;0;0;48;2;0;0;255mR\033[0m", 33);

    ASSERT(scr->cells[0].ch[0] == 'R', "24-bit color: R printed correctly");
    /* Colour is parsed but not stored — just verify no crash */

    pty_screen_free(scr);
}

/* ── PTY session integration test ────────────────────────────────────── */

static void test_pty_echo(void) {
    PtySession *s = pty_open(40, 10);
    ASSERT(s != NULL, "pty_open succeeds");

    /* Run echo — a simple command that outputs and exits */
    const char *argv[] = { "echo", "Hello PTY", NULL };
    int rc = pty_run(s, argv);
    ASSERT(rc == 0, "pty_run succeeds");

    rc = pty_wait_for(s, "Hello PTY", 2000);
    ASSERT(rc == 0, "wait_for: 'Hello PTY' found");

    ASSERT(pty_screen_contains(s, "Hello PTY"), "screen contains 'Hello PTY'");

    pty_close(s);
}

static void test_pty_cat_interactive(void) {
    PtySession *s = pty_open(40, 10);
    ASSERT(s != NULL, "pty_open succeeds");

    const char *argv[] = { "cat", NULL };
    int rc = pty_run(s, argv);
    ASSERT(rc == 0, "pty_run cat succeeds");

    /* Send text, cat echoes it back (TTY echo) */
    pty_send_str(s, "test123\n");
    rc = pty_wait_for(s, "test123", 2000);
    ASSERT(rc == 0, "wait_for: 'test123' echoed by cat");

    pty_close(s);
}

/* ── Row text extraction ─────────────────────────────────────────────── */

static void test_row_text(void) {
    PtySession *s = pty_open(20, 5);
    ASSERT(s != NULL, "pty_open succeeds");

    /* Feed directly into the screen buffer */
    pty_screen_feed(s->screen, "Hello World", 11);

    char buf[64];
    pty_row_text(s, 0, buf, sizeof(buf));
    ASSERT(strcmp(buf, "Hello World") == 0, "row_text: 'Hello World'");

    pty_close(s);
}

/* ── Main ────────────────────────────────────────────────────────────── */

#define RUN_TEST(fn) do { printf("  %s...\n", #fn); fn(); } while(0)

int main(void) {
    printf("--- libptytest self-tests ---\n\n");

    RUN_TEST(test_screen_basic_write);
    RUN_TEST(test_screen_newline);
    RUN_TEST(test_screen_cr_lf);
    RUN_TEST(test_screen_cursor_move);
    RUN_TEST(test_screen_erase_display);
    RUN_TEST(test_screen_sgr_reverse);
    RUN_TEST(test_screen_sgr_bold_dim);
    RUN_TEST(test_screen_sgr_strikethrough);
    RUN_TEST(test_screen_erase_line);
    RUN_TEST(test_screen_scroll);
    RUN_TEST(test_screen_24bit_color_skip);
    RUN_TEST(test_pty_echo);
    RUN_TEST(test_pty_cat_interactive);
    RUN_TEST(test_row_text);

    printf("\n--- Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
