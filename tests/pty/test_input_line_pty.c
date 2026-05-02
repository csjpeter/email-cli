/**
 * @file test_input_line_pty.c
 * @brief PTY-based functional tests for InputLine and PathComplete.
 *
 * Usage: test-pty-input-line <path-to-input-line-harness>
 *
 * Each test launches the harness binary inside a PTY, sends keystrokes,
 * and inspects the virtual screen to verify correct behaviour.
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ptytest.h"
#include "pty_assert.h"

/* Minimal RUN_TEST compatible with g_tests_run / g_tests_failed globals */
#ifndef RUN_TEST
#define RUN_TEST(fn) do { \
    printf("  %-50s", #fn); \
    int _before = g_tests_failed; \
    fn(); \
    printf("%s\n", (g_tests_failed == _before) ? "OK" : "FAIL"); \
} while(0)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Globals ─────────────────────────────────────────────────────────── */

int g_tests_run    = 0;
int g_tests_failed = 0;

static char g_harness_bin[512]; /* path to input-line-harness */
static char g_root[256];        /* per-run temp dir for filesystem tests */

#define WAIT_MS   3000
#define SETTLE_MS 150
#define ROWS 24
#define COLS 80

/* ── Temp filesystem helpers ─────────────────────────────────────────── */

static void fs_setup(void)
{
    snprintf(g_root, sizeof(g_root), "/tmp/il_pty_%d", (int)getpid());
    mkdir(g_root, 0755);
}

static void fs_teardown(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '/tmp/il_pty_%d'", (int)getpid());
    int r = system(cmd);
    (void)r;
}

static void make_file(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

static void make_subdir(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    mkdir(path, 0755);
}

/* ── Session helpers ─────────────────────────────────────────────────── */

/** Open a PTY session running the harness with an optional initial path. */
static PtySession *open_harness(const char *initial)
{
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;

    const char *argv[3];
    argv[0] = g_harness_bin;
    argv[1] = initial;  /* may be NULL */
    argv[2] = NULL;

    if (pty_run(s, argv) != 0) {
        pty_close(s);
        return NULL;
    }
    return s;
}

/* ── Tests: basic editing ────────────────────────────────────────────── */

/** Prompt appears on row 0 immediately after launch. */
static void test_prompt_displayed(void)
{
    PtySession *s = open_harness(NULL);
    ASSERT(s != NULL, "harness launched");

    ASSERT_WAIT_FOR(s, "Path:", WAIT_MS);
    ASSERT_ROW_CONTAINS(s, 0, "Path:");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/** Typing characters echoes them in the input row. */
static void test_typing_appears_on_screen(void)
{
    PtySession *s = open_harness(NULL);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, "Path:", WAIT_MS);

    pty_send_str(s, "hello");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "hello");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/** Backspace removes the last character. */
static void test_backspace_deletes_char(void)
{
    PtySession *s = open_harness(NULL);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, "Path:", WAIT_MS);

    pty_send_str(s, "abc");
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_BACK);
    pty_settle(s, SETTLE_MS);

    /* 'c' should be gone; 'ab' remains */
    ASSERT_ROW_CONTAINS(s, 0, "ab");
    ASSERT(pty_row_contains(s, 0, "abc") == 0, "'abc' gone after backspace");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/** Initial text is shown and confirmed verbatim on Enter. */
static void test_initial_text_confirmed(void)
{
    PtySession *s = open_harness("/some/path.txt");
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, "/some/path.txt", WAIT_MS);

    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "RESULT: /some/path.txt", WAIT_MS);
    pty_close(s);
}

/** ESC returns CANCELLED without result. */
static void test_esc_cancels(void)
{
    PtySession *s = open_harness("/cancel/me");
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, "Path:", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    ASSERT(pty_screen_contains(s, "RESULT:") == 0, "no RESULT on ESC");
    pty_close(s);
}

/** Typed text is returned as the result on Enter. */
static void test_typed_text_in_result(void)
{
    PtySession *s = open_harness(NULL);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, "Path:", WAIT_MS);

    pty_send_str(s, "/tmp/myfile.txt");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "RESULT: /tmp/myfile.txt", WAIT_MS);
    pty_close(s);
}

/** Left/right arrow keys move the cursor (no crash). */
static void test_arrow_keys_move_cursor(void)
{
    PtySession *s = open_harness("abc");
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, "abc", WAIT_MS);

    pty_send_key(s, PTY_KEY_LEFT);
    pty_send_key(s, PTY_KEY_LEFT);
    pty_settle(s, SETTLE_MS);

    /* Insert 'X' before last char: should produce 'aXbc' */
    pty_send_str(s, "X");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "aXbc");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/** Home/End jump to start and end of line. */
static void test_home_end_keys(void)
{
    PtySession *s = open_harness("foo");
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, "foo", WAIT_MS);

    /* Home → insert at start */
    pty_send_key(s, PTY_KEY_HOME);
    pty_send_str(s, "Z");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "Zfoo");

    /* End → append at end */
    pty_send_key(s, PTY_KEY_END);
    pty_send_str(s, "Y");
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "ZfooY");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/* ── Tests: Tab completion ───────────────────────────────────────────── */

/** Tab with a single matching file completes it fully. */
static void test_tab_single_match(void)
{
    make_file(g_root, "report.pdf");

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/rep", g_root);

    PtySession *s = open_harness(prefix);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, prefix, WAIT_MS);

    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);

    /* Completion row (row 0) should contain the full filename */
    ASSERT_ROW_CONTAINS(s, 0, "report.pdf");

    pty_send_key(s, PTY_KEY_ENTER);
    char expected[512];
    snprintf(expected, sizeof(expected), "RESULT: %s/report.pdf", g_root);
    ASSERT_WAIT_FOR(s, expected, WAIT_MS);
    pty_close(s);
}

/** Completion bar appears at row 1 when there are multiple matches. */
static void test_tab_shows_completion_bar(void)
{
    make_file(g_root, "aaa.txt");
    make_file(g_root, "aab.txt");
    make_file(g_root, "aac.txt");

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/aa", g_root);

    PtySession *s = open_harness(prefix);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, prefix, WAIT_MS);

    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);

    /* Completion bar on row 1 must list all three names */
    ASSERT_ROW_CONTAINS(s, 1, "aaa.txt");
    ASSERT_ROW_CONTAINS(s, 1, "aab.txt");
    ASSERT_ROW_CONTAINS(s, 1, "aac.txt");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/** Repeated Tab presses cycle forward through alphabetically sorted matches. */
static void test_tab_cycles_forward(void)
{
    make_file(g_root, "zz_alpha.txt");
    make_file(g_root, "zz_beta.txt");
    make_file(g_root, "zz_gamma.txt");

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/zz_", g_root);

    PtySession *s = open_harness(prefix);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, prefix, WAIT_MS);

    /* 1st Tab → zz_alpha */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "zz_alpha.txt");

    /* 2nd Tab → zz_beta */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "zz_beta.txt");

    /* 3rd Tab → zz_gamma */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "zz_gamma.txt");

    /* 4th Tab wraps back to alpha */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "zz_alpha.txt");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/** Shift+Tab cycles backward through completions. */
static void test_shift_tab_cycles_backward(void)
{
    make_file(g_root, "xx_one.txt");
    make_file(g_root, "xx_three.txt");
    make_file(g_root, "xx_two.txt");

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/xx_", g_root);

    PtySession *s = open_harness(prefix);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, prefix, WAIT_MS);

    /* First Tab → xx_one (idx 0) */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "xx_one.txt");

    /* Shift+Tab backward-wraps to xx_two (last, idx 2) */
    pty_send(s, "\033[Z", 3);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "xx_two.txt");

    /* Another Shift+Tab → xx_three */
    pty_send(s, "\033[Z", 3);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "xx_three.txt");

    /* Another Shift+Tab → xx_one */
    pty_send(s, "\033[Z", 3);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "xx_one.txt");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/** Typing '/' after a directory completion triggers a fresh scan inside it. */
static void test_slash_enters_subdirectory(void)
{
    make_subdir(g_root, "docs");
    make_file(g_root, "docs/guide.pdf");

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/doc", g_root);

    PtySession *s = open_harness(prefix);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, prefix, WAIT_MS);

    /* Tab completes to "docs" */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "docs");

    /* Append '/' */
    pty_send_str(s, "/");
    pty_settle(s, SETTLE_MS);

    /* Next Tab scans inside docs/ → guide.pdf */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    ASSERT_ROW_CONTAINS(s, 0, "guide.pdf");

    char expected[512];
    snprintf(expected, sizeof(expected), "%s/docs/guide.pdf", g_root);
    pty_send_key(s, PTY_KEY_ENTER);
    char want[600];
    snprintf(want, sizeof(want), "RESULT: %s", expected);
    ASSERT_WAIT_FOR(s, want, WAIT_MS);
    pty_close(s);
}

/** Suffix after cursor is preserved throughout Tab cycling. */
static void test_suffix_preserved(void)
{
    make_file(g_root, "report.pdf");
    make_file(g_root, "readme.txt");

    /* Initial path: "<root>/filename.pdf", cursor just after '<root>/' */
    char initial[512];
    snprintf(initial, sizeof(initial), "%s/filename.pdf", g_root);

    PtySession *s = open_harness(initial);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, initial, WAIT_MS);

    /* Move cursor to just after the '/' (Home, then right to skip "<root>/") */
    pty_send_key(s, PTY_KEY_HOME);
    /* advance past every character in g_root + '/' */
    size_t steps = strlen(g_root) + 1;
    for (size_t i = 0; i < steps; i++)
        pty_send_key(s, PTY_KEY_RIGHT);
    pty_settle(s, SETTLE_MS);

    /* Tab with empty prefix → first alphabetical match; suffix stays */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);

    /* Suffix "filename.pdf" must still be visible on the input row */
    ASSERT_ROW_CONTAINS(s, 0, "filename.pdf");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/** Hidden files (dot-files) are excluded when prefix is empty. */
static void test_hidden_files_excluded(void)
{
    char td[512];
    snprintf(td, sizeof(td), "%s/hidden_test", g_root);
    mkdir(td, 0755);
    make_file(td, ".hidden_secret");
    make_file(td, "visible.cfg");

    char prefix[768];
    snprintf(prefix, sizeof(prefix), "%s/", td);

    PtySession *s = open_harness(prefix);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, prefix, WAIT_MS);

    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);

    /* Input row must show the visible file (first Tab selection) */
    ASSERT_ROW_CONTAINS(s, 0, "visible.cfg");
    /* Hidden file must not appear anywhere on screen */
    ASSERT(pty_screen_contains(s, ".hidden_secret") == 0,
           "hidden file not on screen");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/** Dot prefix reveals hidden files. */
static void test_dot_prefix_shows_hidden(void)
{
    make_file(g_root, ".bashrc");
    make_file(g_root, ".profile");

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/.", g_root);

    PtySession *s = open_harness(prefix);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, prefix, WAIT_MS);

    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);

    /* First alphabetical dot-file (.bashrc) should appear */
    ASSERT_ROW_CONTAINS(s, 0, ".bashrc");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/** No match: buffer is unchanged after Tab. */
static void test_no_match_leaves_buf_unchanged(void)
{
    /* Use empty dir (g_root has only previously created files from other
     * tests — use a fresh subdir to guarantee it is empty). */
    char emptydir[512];
    snprintf(emptydir, sizeof(emptydir), "%s/empty_notest", g_root);
    mkdir(emptydir, 0755);

    char prefix[768];
    snprintf(prefix, sizeof(prefix), "%s/zzz", emptydir);

    PtySession *s = open_harness(prefix);
    ASSERT(s != NULL, "harness launched");
    ASSERT_WAIT_FOR(s, prefix, WAIT_MS);

    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);

    /* Input row must still show the unchanged prefix */
    ASSERT_ROW_CONTAINS(s, 0, prefix);
    /* Completion bar (row 1) must not appear / must be empty */
    ASSERT(pty_row_contains(s, 1, "zzz") == 0, "no completion bar on no match");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "CANCELLED", WAIT_MS);
    pty_close(s);
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input-line-harness>\n", argv[0]);
        return 1;
    }
    snprintf(g_harness_bin, sizeof(g_harness_bin), "%s", argv[1]);

    printf("--- InputLine / PathComplete PTY Test Suite ---\n\n");

    fs_setup();

    /* Basic editing */
    printf("[Basic editing]\n");
    RUN_TEST(test_prompt_displayed);
    RUN_TEST(test_typing_appears_on_screen);
    RUN_TEST(test_backspace_deletes_char);
    RUN_TEST(test_initial_text_confirmed);
    RUN_TEST(test_esc_cancels);
    RUN_TEST(test_typed_text_in_result);
    RUN_TEST(test_arrow_keys_move_cursor);
    RUN_TEST(test_home_end_keys);

    /* Tab completion */
    printf("\n[Tab completion]\n");
    RUN_TEST(test_tab_single_match);
    RUN_TEST(test_tab_shows_completion_bar);
    RUN_TEST(test_tab_cycles_forward);
    RUN_TEST(test_shift_tab_cycles_backward);
    RUN_TEST(test_slash_enters_subdirectory);
    RUN_TEST(test_suffix_preserved);
    RUN_TEST(test_hidden_files_excluded);
    RUN_TEST(test_dot_prefix_shows_hidden);
    RUN_TEST(test_no_match_leaves_buf_unchanged);

    fs_teardown();

    printf("\n--- Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return (g_tests_failed > 0) ? 1 : 0;
}
