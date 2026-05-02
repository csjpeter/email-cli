#include "test_helpers.h"
#include "input_line.h"
#include "path_complete.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Test fixture ─────────────────────────────────────────���──────────── */

static char g_root[256]; /* per-run temp root, removed at end */

static void fixture_setup(void) {
    snprintf(g_root, sizeof(g_root), "/tmp/pc_test_%d", getpid());
    mkdir(g_root, 0755);
}

static void fixture_teardown(void) {
    /* Recursively remove the temp tree via shell to keep test code short. */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '/tmp/pc_test_%d'", (int)getpid());
    if (system(cmd)) { /* cleanup failure ignored in tests */ }
}

/* Create a test sub-directory under g_root; return its path in out[outsz]. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void td_make(const char *name, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s", g_root, name);
    mkdir(out, 0755);
}
#pragma GCC diagnostic pop

/* Create an empty regular file at dir/name. */
static void make_file(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
}

/* Create a sub-directory at dir/name. */
static void make_subdir(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    mkdir(path, 0755);
}

/* Initialise an InputLine with path_complete_attach and buf = text. */
static void il_setup(InputLine *il, char *buf, size_t bufsz, const char *text) {
    input_line_init(il, buf, bufsz, text);
    path_complete_attach(il);
}

/* ── Tests ───────────────────────────────────────────────────────────── */

static void test_attach_sets_callbacks(void) {
    char buf[256] = "";
    InputLine il;
    input_line_init(&il, buf, sizeof(buf), "");

    ASSERT(il.tab_fn       == NULL, "tab_fn NULL before attach");
    ASSERT(il.shift_tab_fn == NULL, "shift_tab_fn NULL before attach");
    ASSERT(il.render_below == NULL, "render_below NULL before attach");

    path_complete_attach(&il);

    ASSERT(il.tab_fn       != NULL, "tab_fn set after attach");
    ASSERT(il.shift_tab_fn != NULL, "shift_tab_fn set after attach");
    ASSERT(il.render_below != NULL, "render_below set after attach");
}

static void test_reset_idempotent(void) {
    path_complete_reset(); /* no crash on empty state */
    path_complete_reset(); /* double reset safe */
}

static void test_no_match_leaves_buf_unchanged(void) {
    char td[256]; td_make("no_match", td, sizeof(td));
    /* dir is empty; type a prefix that matches nothing */
    char path[512]; snprintf(path, sizeof(path), "%s/zzz", td);

    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), path);
    il.tab_fn(&il);

    ASSERT(strcmp(il.buf, path) == 0, "buf unchanged when no match");
    ASSERT(il.cur == strlen(path),    "cur unchanged when no match");
    path_complete_reset();
}

static void test_single_match_completes_fully(void) {
    char td[256]; td_make("single", td, sizeof(td));
    make_file(td, "report.pdf");

    char prefix[512]; snprintf(prefix, sizeof(prefix), "%s/rep", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), prefix);
    il.tab_fn(&il);

    char expected[512]; snprintf(expected, sizeof(expected), "%s/report.pdf", td);
    ASSERT(strcmp(il.buf, expected) == 0, "single match: full completion");
    ASSERT(il.cur == strlen(expected),    "cursor at end after single match");
    path_complete_reset();
}

static void test_cycles_forward_through_matches(void) {
    char td[256]; td_make("cycle_fwd", td, sizeof(td));
    make_file(td, "aaa.txt");
    make_file(td, "aab.txt");
    make_file(td, "aac.txt");

    char prefix[512]; snprintf(prefix, sizeof(prefix), "%s/aa", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), prefix);

    char e1[512], e2[512], e3[512];
    snprintf(e1, sizeof(e1), "%s/aaa.txt", td);
    snprintf(e2, sizeof(e2), "%s/aab.txt", td);
    snprintf(e3, sizeof(e3), "%s/aac.txt", td);

    il.tab_fn(&il);
    ASSERT(strcmp(il.buf, e1) == 0, "1st Tab → aaa.txt");

    il.tab_fn(&il);
    ASSERT(strcmp(il.buf, e2) == 0, "2nd Tab → aab.txt");

    il.tab_fn(&il);
    ASSERT(strcmp(il.buf, e3) == 0, "3rd Tab → aac.txt");

    il.tab_fn(&il); /* wrap */
    ASSERT(strcmp(il.buf, e1) == 0, "4th Tab wraps to aaa.txt");
    path_complete_reset();
}

static void test_cycles_backward_with_shift_tab(void) {
    char td[256]; td_make("cycle_bwd", td, sizeof(td));
    make_file(td, "baa.txt");
    make_file(td, "bab.txt");
    make_file(td, "bac.txt");

    char prefix[512]; snprintf(prefix, sizeof(prefix), "%s/ba", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), prefix);

    char e1[512], e2[512], e3[512];
    snprintf(e1, sizeof(e1), "%s/baa.txt", td);
    snprintf(e2, sizeof(e2), "%s/bab.txt", td);
    snprintf(e3, sizeof(e3), "%s/bac.txt", td);

    il.tab_fn(&il);              /* → baa.txt (idx 0) */
    il.shift_tab_fn(&il);        /* backward wrap → bac.txt (idx 2) */
    ASSERT(strcmp(il.buf, e3) == 0, "Shift+Tab wraps backward to bac.txt");

    il.shift_tab_fn(&il);        /* → bab.txt */
    ASSERT(strcmp(il.buf, e2) == 0, "Shift+Tab → bab.txt");

    il.shift_tab_fn(&il);        /* → baa.txt */
    ASSERT(strcmp(il.buf, e1) == 0, "Shift+Tab → baa.txt");
    path_complete_reset();
}

static void test_shift_tab_without_active_list_is_noop(void) {
    char td[256]; td_make("stab_noop", td, sizeof(td));
    make_file(td, "x.txt");

    char path[512]; snprintf(path, sizeof(path), "%s/x.txt", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), path);

    /* No Tab pressed yet; shift_tab_fn should be a no-op */
    il.shift_tab_fn(&il);
    ASSERT(strcmp(il.buf, path) == 0, "Shift+Tab no-op without active list");
    path_complete_reset();
}

static void test_suffix_preserved_when_cursor_in_middle(void) {
    char td[256]; td_make("suffix", td, sizeof(td));
    make_file(td, "report.pdf");
    make_file(td, "readme.txt");

    /* buf = "<td>/filename.pdf", cursor right after "<td>/" */
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/filename.pdf", td);
    InputLine il;
    input_line_init(&il, buf, sizeof(buf), buf);
    il.cur = strlen(td) + 1; /* cursor points just after the '/' */
    path_complete_attach(&il);

    il.tab_fn(&il); /* prefix="" → first alphabetical match selected */

    /* The suffix "filename.pdf" must appear at the end of the result */
    size_t blen = strlen(il.buf);
    const char *suffix = "filename.pdf";
    size_t slen = strlen(suffix);
    ASSERT(blen > slen, "result longer than suffix");
    ASSERT(strcmp(il.buf + blen - slen, suffix) == 0,
           "suffix 'filename.pdf' preserved after completion");

    /* Cursor is at the end of the completed prefix, not at the end of buf */
    ASSERT(il.cur < il.len, "cursor before end when suffix present");
    path_complete_reset();
}

static void test_edit_after_cycle_triggers_fresh_scan(void) {
    char td[256]; td_make("rescan", td, sizeof(td));
    make_file(td, "cat.txt");
    make_file(td, "car.txt");

    char prefix[512]; snprintf(prefix, sizeof(prefix), "%s/ca", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), prefix);

    il.tab_fn(&il); /* first match: car.txt (alphabetical) */

    /* Simulate user editing the buffer back to the original prefix */
    snprintf(il.buf, il.bufsz, "%s/ca", td);
    il.len = strlen(il.buf);
    il.cur = il.len;

    /* Next Tab must do a fresh scan, not rely on stale expected */
    il.tab_fn(&il);
    char expected[512]; snprintf(expected, sizeof(expected), "%s/car.txt", td);
    ASSERT(strcmp(il.buf, expected) == 0, "fresh scan after edit → car.txt");
    path_complete_reset();
}

static void test_typing_slash_enters_subdirectory(void) {
    char td[256]; td_make("enter_dir", td, sizeof(td));
    make_subdir(td, "docs");
    make_file(td, "docs/guide.pdf"); /* nested file */

    char prefix[512]; snprintf(prefix, sizeof(prefix), "%s/doc", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), prefix);

    il.tab_fn(&il); /* completes to "<td>/docs" */
    char exp_docs[512]; snprintf(exp_docs, sizeof(exp_docs), "%s/docs", td);
    ASSERT(strcmp(il.buf, exp_docs) == 0, "completes to 'docs'");

    /* User types '/' → buf becomes "<td>/docs/" */
    size_t len = il.len;
    il.buf[len] = '/'; il.buf[len+1] = '\0';
    il.len = len + 1; il.cur = il.len;

    /* Next Tab must scan inside docs/, not cycle old list */
    il.tab_fn(&il);
    char expected[512]; snprintf(expected, sizeof(expected), "%s/docs/guide.pdf", td);
    ASSERT(strcmp(il.buf, expected) == 0, "Tab after '/' enters subdirectory");
    path_complete_reset();
}

static void test_results_are_sorted_alphabetically(void) {
    char td[256]; td_make("sorted", td, sizeof(td));
    make_file(td, "zebra.txt");
    make_file(td, "alpha.txt");
    make_file(td, "mango.txt");

    char prefix[512]; snprintf(prefix, sizeof(prefix), "%s/", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), prefix);

    char prev[512] = "";
    for (int i = 0; i < 3; i++) {
        il.tab_fn(&il);
        /* Extract just the filename part */
        const char *name = il.buf + strlen(td) + 1;
        if (i > 0)
            ASSERT(strcmp(prev, name) < 0, "each match strictly after previous (sorted)");
        strncpy(prev, name, sizeof(prev) - 1);
    }
    path_complete_reset();
}

static void test_hidden_files_excluded_with_empty_prefix(void) {
    char td[256]; td_make("hidden", td, sizeof(td));
    make_file(td, ".hidden");
    make_file(td, "visible.txt");

    char prefix[512]; snprintf(prefix, sizeof(prefix), "%s/", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), prefix);

    il.tab_fn(&il);
    ASSERT(strstr(il.buf, ".hidden") == NULL, "hidden file excluded when prefix empty");
    ASSERT(strstr(il.buf, "visible.txt") != NULL, "visible file included");
    path_complete_reset();
}

static void test_hidden_files_included_with_dot_prefix(void) {
    char td[256]; td_make("hidden_dot", td, sizeof(td));
    make_file(td, ".bashrc");
    make_file(td, ".profile");

    char prefix[512]; snprintf(prefix, sizeof(prefix), "%s/.", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), prefix);

    il.tab_fn(&il);
    /* First alphabetical match starting with '.' should appear */
    const char *name = il.buf + strlen(td) + 1;
    ASSERT(name[0] == '.', "hidden file included when prefix starts with '.'");
    path_complete_reset();
}

static void test_render_below_no_completions_is_noop(void) {
    /* render_below is a no-op when count == 0 (no completions) */
    char buf[256] = "";
    InputLine il;
    input_line_init(&il, buf, sizeof(buf), "");
    path_complete_attach(&il);
    path_complete_reset();  /* ensure empty state */
    /* Should not crash even though count == 0 */
    il.render_below(&il);
    ASSERT(1, "render_below with no completions does not crash");
}

static void test_render_below_after_match(void) {
    /* Trigger completion so count > 0, then call render_below directly.
     * The render writes ANSI escapes to stdout — that's fine in tests. */
    char td[256]; td_make("render_after_match", td, sizeof(td));
    make_file(td, "alpha.txt");
    make_file(td, "beta.txt");

    char prefix[512]; snprintf(prefix, sizeof(prefix), "%s/", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), prefix);
    il.trow = 1;  /* give a non-zero row so ANSI escape is well-formed */

    il.tab_fn(&il);  /* populates g_comp with 2 matches */
    il.render_below(&il);  /* exercises the rendering path with count > 0 */
    ASSERT(1, "render_below with matches does not crash");
    path_complete_reset();
}

static void test_render_below_many_matches_scrolls(void) {
    /* Create enough matches that view_start is advanced (scrolling path). */
    char td[256]; td_make("render_scroll", td, sizeof(td));
    /* Create 30 files to overflow a narrow terminal */
    for (int i = 0; i < 30; i++) {
        char name[64]; snprintf(name, sizeof(name), "file%02d.txt", i);
        make_file(td, name);
    }

    char prefix[512]; snprintf(prefix, sizeof(prefix), "%s/", td);
    char buf[512]; InputLine il;
    il_setup(&il, buf, sizeof(buf), prefix);
    il.trow = 1;

    il.tab_fn(&il);   /* populates g_comp with 30 matches, idx=0 */
    /* Advance idx towards the end to force view scrolling */
    for (int i = 0; i < 25; i++) {
        il.tab_fn(&il);
    }
    il.render_below(&il);  /* exercises view_start advancement */
    ASSERT(1, "render_below with many matches (scroll) does not crash");
    path_complete_reset();
}

static void test_no_slash_in_prefix_uses_cwd(void) {
    /* When the prefix has no '/', g_comp.dir becomes "./" and opendir runs
     * on the current directory.  Any single character that matches nothing is fine. */
    char buf[256]; InputLine il;
    /* Use a prefix that is very unlikely to match any real file */
    il_setup(&il, buf, sizeof(buf), "ZZZQQQNONEXISTENT");
    il.tab_fn(&il);
    /* buf unchanged since no match */
    ASSERT(strcmp(il.buf, "ZZZQQQNONEXISTENT") == 0,
           "no-slash prefix with no match leaves buf unchanged");
    path_complete_reset();
}

/* ── Entry point ─────────────────────────────────────────────────────── */

void test_path_complete(void) {
    fixture_setup();

    RUN_TEST(test_attach_sets_callbacks);
    RUN_TEST(test_reset_idempotent);
    RUN_TEST(test_no_match_leaves_buf_unchanged);
    RUN_TEST(test_single_match_completes_fully);
    RUN_TEST(test_cycles_forward_through_matches);
    RUN_TEST(test_cycles_backward_with_shift_tab);
    RUN_TEST(test_shift_tab_without_active_list_is_noop);
    RUN_TEST(test_suffix_preserved_when_cursor_in_middle);
    RUN_TEST(test_edit_after_cycle_triggers_fresh_scan);
    RUN_TEST(test_typing_slash_enters_subdirectory);
    RUN_TEST(test_results_are_sorted_alphabetically);
    RUN_TEST(test_hidden_files_excluded_with_empty_prefix);
    RUN_TEST(test_hidden_files_included_with_dot_prefix);
    RUN_TEST(test_render_below_no_completions_is_noop);
    RUN_TEST(test_render_below_after_match);
    RUN_TEST(test_render_below_many_matches_scrolls);
    RUN_TEST(test_no_slash_in_prefix_uses_cwd);

    fixture_teardown();
}
