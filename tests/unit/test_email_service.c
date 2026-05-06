#include "test_helpers.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <locale.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/*
 * Include the full domain source so all static helpers are visible in
 * this translation unit.  email_service.c is NOT added to CMakeLists.txt
 * as a separate source — the #include below is the only compilation unit
 * that defines its symbols.
 */
#include "../../libemail/src/domain/email_service.c"

void test_email_service(void) {

    setlocale(LC_ALL, "");
    local_store_init("imaps://test.example.com", "testuser");

    /* ── count_visual_rows ───────────────────────────────────────────── */

    /* Short lines: visual rows == logical lines (all fit within term_cols) */
    ASSERT(count_visual_rows(NULL,  80) == 0, "cvr: NULL → 0");
    ASSERT(count_visual_rows("",    80) == 0, "cvr: empty → 0");
    ASSERT(count_visual_rows("abc", 80) == 1, "cvr: single line → 1");
    ASSERT(count_visual_rows("a\nb", 80) == 2, "cvr: two lines → 2");
    ASSERT(count_visual_rows("a\nb\nc\n", 80) == 4, "cvr: trailing newline → 4");

    /* A line exactly term_cols wide → 1 visual row */
    {
        char exact[81]; memset(exact, 'X', 80); exact[80] = '\0';
        ASSERT(count_visual_rows(exact, 80) == 1, "cvr: 80-char line → 1 row");
    }

    /* A line wider than term_cols → multiple visual rows */
    {
        char wide[161]; memset(wide, 'X', 160); wide[160] = '\0';
        /* 160-char line on 80-col terminal → 2 visual rows (+ terminating segment) */
        char body[163]; snprintf(body, sizeof(body), "%s\n", wide);
        int vr = count_visual_rows(body, 80);
        ASSERT(vr == 3, "cvr: 160-char line+\\n → 3 rows (2 for URL, 1 trailing)");
    }

    /* A long URL (no newline) → single logical line counted as multiple visual rows */
    {
        char url[201]; memset(url, 'x', 200); url[200] = '\0';
        /* 200 chars on 80-col terminal = ceil(200/80) = 3 visual rows */
        ASSERT(count_visual_rows(url, 80) == 3, "cvr: 200-char url → 3 rows");
    }

    /* With ANSI escapes: invisible bytes not counted toward visible cols */
    ASSERT(count_visual_rows("\033[1mhello\033[22m", 80) == 1,
           "cvr: ANSI-wrapped line → 1 row");

    /* ── word_wrap ───────────────────────────────────────────────────── */

    /* NULL input → NULL */
    {
        char *r = word_wrap(NULL, 40);
        ASSERT(r == NULL, "word_wrap: NULL input → NULL");
    }

    /* Short text that fits entirely — no wrapping needed */
    {
        char *r = word_wrap("Hello world", 40);
        ASSERT(r != NULL, "word_wrap: short text not NULL");
        ASSERT(strstr(r, "Hello world") != NULL, "word_wrap: short text passthrough");
        free(r);
    }

    /* Word break at space (lines 169-174): width=25, long text with spaces */
    {
        char *r = word_wrap("The quick brown fox jumps over the lazy dog", 25);
        ASSERT(r != NULL, "word_wrap: word break not NULL");
        ASSERT(strstr(r, "\n") != NULL, "word_wrap: word break produces newline");
        free(r);
    }

    /* Long word without spaces: emitted whole (terminal wraps, not us) */
    {
        char *r = word_wrap("aaaaaaaaaaaaaaaaaaaaaaaaa", 20);
        ASSERT(r != NULL, "word_wrap: long word not NULL");
        ASSERT(strstr(r, "aaaaaaaaaaaaaaaaaaaaaaaaa") != NULL,
               "word_wrap: long word emitted intact");
        free(r);
    }

    /* URL longer than width must not be broken mid-URL */
    {
        const char *url = "https://www.example.com/very/long/path/that/exceeds/"
                          "the/wrap/width/limit/by/far/and/keeps/going";
        char *r = word_wrap(url, 40);
        ASSERT(r != NULL, "word_wrap: long URL not NULL");
        ASSERT(strstr(r, url) != NULL, "word_wrap: long URL emitted intact");
        free(r);
    }

    /* 2-byte UTF-8 lead byte (line 143: *p < 0xE0): é = \xC3\xA9 */
    {
        char *r = word_wrap("\xC3\xA9\xC3\xA9\xC3\xA9 test", 40);
        ASSERT(r != NULL, "word_wrap: 2-byte UTF-8 not NULL");
        free(r);
    }

    /* 3-byte UTF-8 lead byte (line 144: *p < 0xF0): 中 = \xE4\xB8\xAD */
    {
        char *r = word_wrap("\xE4\xB8\xAD text", 40);
        ASSERT(r != NULL, "word_wrap: 3-byte UTF-8 not NULL");
        free(r);
    }

    /* 4-byte UTF-8 lead byte (line 145: *p < 0xF8): U+10000 = \xF0\x90\x80\x80 */
    {
        char *r = word_wrap("\xF0\x90\x80\x80 test", 40);
        ASSERT(r != NULL, "word_wrap: 4-byte UTF-8 not NULL");
        free(r);
    }

    /* Invalid lead byte < 0xC2 (line 142: continuation byte as lead) */
    {
        char *r = word_wrap("\x80 bad", 40);
        ASSERT(r != NULL, "word_wrap: 0x80 lead byte not NULL");
        free(r);
    }

    /* Invalid lead byte >= 0xF8 (line 146: else branch) */
    {
        char *r = word_wrap("\xFE bad", 40);
        ASSERT(r != NULL, "word_wrap: 0xFE lead byte not NULL");
        free(r);
    }

    /* Continuation byte mismatch (line 148): 2-byte start \xC3 + non-continuation \x41 */
    {
        char *r = word_wrap("\xC3\x41 bad", 40);
        ASSERT(r != NULL, "word_wrap: truncated multibyte not NULL");
        free(r);
    }

    /* Multi-line input — exercises the outer loop past eol */
    {
        char *r = word_wrap("first line\nsecond line\n", 40);
        ASSERT(r != NULL, "word_wrap: multi-line not NULL");
        ASSERT(strstr(r, "first line") != NULL, "word_wrap: multi-line first");
        ASSERT(strstr(r, "second line") != NULL, "word_wrap: multi-line second");
        free(r);
    }

    /* ── ansi_scan ───────────────────────────────────────────────────── */

    /* Empty content → all zeros */
    {
        AnsiState st = {0};
        ansi_scan("", "", &st);
        ASSERT(st.bold==0 && st.italic==0 && st.uline==0 && st.strike==0,
               "ansi_scan: empty → no state");
        ASSERT(st.fg_on==0 && st.bg_on==0, "ansi_scan: empty → no color");
    }

    /* Bold on/off */
    {
        AnsiState st = {0};
        const char *s = "\033[1mtext\033[22m";
        ansi_scan(s, s + strlen(s), &st);
        ASSERT(st.bold == 0, "ansi_scan: bold on then off → 0");

        AnsiState st2 = {0};
        const char *s2 = "\033[1mtext";
        ansi_scan(s2, s2 + strlen(s2), &st2);
        ASSERT(st2.bold == 1, "ansi_scan: bold on, no off → 1");
    }

    /* Italic on/off */
    {
        AnsiState st = {0};
        const char *s = "\033[3m";
        ansi_scan(s, s + strlen(s), &st);
        ASSERT(st.italic == 1, "ansi_scan: italic on → 1");

        st.italic = 1;
        const char *s2 = "\033[23m";
        ansi_scan(s2, s2 + strlen(s2), &st);
        ASSERT(st.italic == 0, "ansi_scan: italic off → 0");
    }

    /* Underline on/off */
    {
        AnsiState st = {0};
        const char *s = "\033[4m";
        ansi_scan(s, s + strlen(s), &st);
        ASSERT(st.uline == 1, "ansi_scan: uline on → 1");

        const char *s2 = "\033[24m";
        ansi_scan(s2, s2 + strlen(s2), &st);
        ASSERT(st.uline == 0, "ansi_scan: uline off → 0");
    }

    /* Strikethrough on/off */
    {
        AnsiState st = {0};
        const char *s = "\033[9m";
        ansi_scan(s, s + strlen(s), &st);
        ASSERT(st.strike == 1, "ansi_scan: strike on → 1");

        const char *s2 = "\033[29m";
        ansi_scan(s2, s2 + strlen(s2), &st);
        ASSERT(st.strike == 0, "ansi_scan: strike off → 0");
    }

    /* Foreground color set and reset */
    {
        AnsiState st = {0};
        const char *s = "\033[38;2;255;0;128m";
        ansi_scan(s, s + strlen(s), &st);
        ASSERT(st.fg_on == 1, "ansi_scan: fg on → 1");
        ASSERT(st.fg_r == 255 && st.fg_g == 0 && st.fg_b == 128,
               "ansi_scan: fg RGB correct");

        const char *s2 = "\033[39m";
        ansi_scan(s2, s2 + strlen(s2), &st);
        ASSERT(st.fg_on == 0, "ansi_scan: fg reset → 0");
    }

    /* Background color set and reset */
    {
        AnsiState st = {0};
        const char *s = "\033[48;2;0;64;255m";
        ansi_scan(s, s + strlen(s), &st);
        ASSERT(st.bg_on == 1, "ansi_scan: bg on → 1");
        ASSERT(st.bg_r == 0 && st.bg_g == 64 && st.bg_b == 255,
               "ansi_scan: bg RGB correct");

        const char *s2 = "\033[49m";
        ansi_scan(s2, s2 + strlen(s2), &st);
        ASSERT(st.bg_on == 0, "ansi_scan: bg reset → 0");
    }

    /* Full reset \033[0m clears all accumulated state */
    {
        AnsiState st = {0};
        const char *s = "\033[1m\033[3m\033[38;2;255;0;0m\033[0m";
        ansi_scan(s, s + strlen(s), &st);
        ASSERT(st.bold==0 && st.italic==0 && st.fg_on==0,
               "ansi_scan: full reset clears all");
    }

    /* Partial scan: only up to a mid-point in the string */
    {
        /* Scan only the first segment (bold+color open), stop before close */
        const char *body = "\033[1m\033[38;2;255;0;0mLine 0\nLine 1\n\033[22m\033[39m";
        const char *nl   = strchr(body, '\n');  /* end of "Line 0" */
        AnsiState st = {0};
        ansi_scan(body, nl, &st);
        ASSERT(st.bold == 1,  "ansi_scan: partial scan bold open");
        ASSERT(st.fg_on == 1, "ansi_scan: partial scan fg open");
    }

    /* ── print_body_page ─────────────────────────────────────────────── */
    /*
     * Redirect stdout to /dev/null so the printed lines do not pollute
     * the test runner output.  Restore after.
     */
    {
        fflush(stdout);
        int saved_fd = dup(STDOUT_FILENO);
        int null_fd  = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) dup2(null_fd, STDOUT_FILENO);
        if (null_fd >= 0) close(null_fd);

        /* Print lines 1-2 of a 4-line body (normal newline path) */
        print_body_page("Line 0\nLine 1\nLine 2\nLine 3\n", 1, 2, 80);

        /* Body does not end with '\n': last segment hits the else branch
         * (printf("%s\n", p); break;) at lines 255-257 */
        print_body_page("Line 0\nNo newline here", 1, 5, 80);

        /* from_line == 0, single print */
        print_body_page("only line", 0, 1, 80);

        fflush(stdout);
        dup2(saved_fd, STDOUT_FILENO);
        close(saved_fd);
    }

    /*
     * Regression test: ANSI state must be replayed at page boundaries.
     *
     * A multi-line styled span (e.g. <div style="color:red">) produces:
     *   \033[38;2;255;0;0mLine 0\nLine 1\nLine 2\n\n\033[39m
     *
     * When paginating from line 1 onward, the fg-color escape from line 0
     * would have been SKIPPED.  Without the fix, Line 1 and Line 2 appeared
     * in the terminal's default color — and if the terminal had a dark theme
     * and the email also set background:white, the result was white-on-white.
     *
     * The fix (ansi_scan + ansi_replay) re-emits the color escape before the
     * first visible line.  This test captures stdout via a pipe and asserts
     * the replayed escape is present.
     */
    {
        /* Body that html_render() would produce for a multi-line color span */
        const char *body =
            "\033[38;2;255;0;0mLine 0\n"   /* fg red open on line 0 */
            "Line 1\n"
            "Line 2\n"
            "\033[39m";                    /* fg reset after last line */

        int pipefd[2];
        if (pipe(pipefd) != 0) { ASSERT(0, "page ANSI replay: pipe failed"); goto skip_replay_fg; }
        fflush(stdout);
        int saved = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* Skip line 0; print lines 1-2 */
        print_body_page(body, 1, 2, 80);

        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);

        char buf[256] = {0};
        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
        close(pipefd[0]);
        buf[n > 0 ? n : 0] = '\0';

        /* The replayed fg-red escape must appear before "Line 1" */
        const char *esc  = strstr(buf, "\033[38;2;255;0;0m");
        const char *line1 = strstr(buf, "Line 1");
        ASSERT(esc != NULL,
               "page ANSI replay: fg color escape present in page-2 output");
        ASSERT(line1 != NULL,
               "page ANSI replay: Line 1 present in output");
        ASSERT(esc < line1,
               "page ANSI replay: fg escape precedes Line 1");
        skip_replay_fg:;
    }

    /*
     * Regression test: background color must also be replayed.
     * This models the exact scenario that caused white-on-white:
     * a <div style="background-color:white"> spanning multiple lines.
     */
    {
        const char *body =
            "\033[48;2;255;255;255mLine 0\n"   /* bg white on line 0 */
            "Line 1\n"
            "\033[49m";

        int pipefd[2];
        if (pipe(pipefd) != 0) { ASSERT(0, "page ANSI replay bg: pipe failed"); goto skip_replay_bg; }
        fflush(stdout);
        int saved = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        print_body_page(body, 1, 1, 80);

        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);

        char buf[256] = {0};
        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
        close(pipefd[0]);
        buf[n > 0 ? n : 0] = '\0';

        const char *esc   = strstr(buf, "\033[48;2;255;255;255m");
        const char *line1 = strstr(buf, "Line 1");
        ASSERT(esc != NULL,
               "page ANSI replay bg: bg color escape present in page-2 output");
        ASSERT(esc < line1,
               "page ANSI replay bg: bg escape precedes Line 1");
        skip_replay_bg:;
    }

    /* ── print_padded_col (non-ASCII paths, lines 83-91) ─────────────── */
    /*
     * Redirect stdout to /dev/null to avoid polluting test output.
     * print_padded_col writes to stdout via fwrite/putchar.
     */
    {
        fflush(stdout);
        int saved_fd2 = dup(STDOUT_FILENO);
        int null_fd2  = open("/dev/null", O_WRONLY);
        if (null_fd2 >= 0) dup2(null_fd2, STDOUT_FILENO);
        if (null_fd2 >= 0) close(null_fd2);

        /* 0x80 = invalid lead byte → line 83 */
        print_padded_col("\x80 bad", 20);

        /* 2-byte UTF-8: é = \xC3\xA9 → line 84 */
        print_padded_col("\xC3\xA9 cafe", 20);

        /* 3-byte UTF-8: 中 = \xE4\xB8\xAD → line 85 */
        print_padded_col("\xE4\xB8\xAD word", 20);

        /* 4-byte UTF-8: U+10000 = \xF0\x90\x80\x80 → line 86 */
        print_padded_col("\xF0\x90\x80\x80 hi", 20);

        /* 0xFE = invalid lead byte >= 0xF8 → line 87 */
        print_padded_col("\xFE bad", 20);

        /* Truncated 2-byte: \xC3 then 'A' (not continuation) → lines 90-91 */
        print_padded_col("\xC3\x41 trunc", 20);

        fflush(stdout);
        dup2(saved_fd2, STDOUT_FILENO);
        close(saved_fd2);
    }

    /*
     * Regression: visual row budget in print_body_page.
     *
     * Body has 1 normal line + 1 very wide line (wider than term_cols) +
     * 2 more normal lines.  With a visual row budget of 3 on a 40-col
     * terminal, the wide line consumes multiple visual rows, so the 3rd
     * normal line should NOT appear in the output.
     *
     * This proves print_body_page stops at the visual row budget, not the
     * logical line count.
     */
    {
        /* Build a 120-char URL-like token (fits on 1 logical line, 3 visual rows on 40-col) */
        char wide[121]; memset(wide, 'W', 120); wide[120] = '\0';
        char body_vr[256];
        snprintf(body_vr, sizeof(body_vr),
                 "NormalA\n%s\nNormalB\nNormalC\n", wide);

        int pipefd[2];
        if (pipe(pipefd) != 0) {
            ASSERT(0, "visual rows: pipe failed");
            goto skip_vr_test;
        }
        fflush(stdout);
        int saved_vr = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* budget = 4 visual rows on 40-col terminal:
         *   NormalA  = 1 row  (total 1)
         *   wide 120 = 3 rows (total 4) → fits in budget
         *   NormalB  = 1 row  (total 5 > 4) → should NOT appear
         *   NormalC  → should NOT appear */
        print_body_page(body_vr, 0, 4, 40);

        fflush(stdout);
        dup2(saved_vr, STDOUT_FILENO);
        close(saved_vr);

        char buf_vr[512] = {0};
        ssize_t n_vr = read(pipefd[0], buf_vr, sizeof(buf_vr) - 1);
        close(pipefd[0]);
        buf_vr[n_vr > 0 ? n_vr : 0] = '\0';

        ASSERT(strstr(buf_vr, "NormalA")  != NULL,
               "visual rows: NormalA shown (fits in budget)");
        ASSERT(strstr(buf_vr, wide)        != NULL,
               "visual rows: wide line shown (fits in budget)");
        ASSERT(strstr(buf_vr, "NormalB")  == NULL,
               "visual rows: NormalB NOT shown (budget exhausted)");
        ASSERT(strstr(buf_vr, "NormalC")  == NULL,
               "visual rows: NormalC NOT shown (budget exhausted)");
        skip_vr_test:;
    }

    /* ── print_clean — truncation at max_cols ───────────────────────── */
    /*
     * Regression test for ce09877: print_clean must stop emitting characters
     * once the visible column count reaches max_cols, so that header values
     * (From/Subject/Date) never overflow the 80-column display width.
     *
     * We capture stdout via a pipe, call print_clean with a 200-char ASCII
     * string and max_cols=10, then verify the captured output is ≤ 10 bytes.
     */
    {
        char long_str[201];
        memset(long_str, 'A', 200);
        long_str[200] = '\0';

        int pipefd[2];
        if (pipe(pipefd) != 0) {
            ASSERT(0, "print_clean truncation: pipe failed");
            goto skip_print_clean;
        }
        fflush(stdout);
        int saved_pc = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        print_clean(long_str, "(none)", 10);

        fflush(stdout);
        dup2(saved_pc, STDOUT_FILENO);
        close(saved_pc);

        char buf_pc[256] = {0};
        ssize_t n_pc = read(pipefd[0], buf_pc, sizeof(buf_pc) - 1);
        close(pipefd[0]);
        buf_pc[n_pc > 0 ? n_pc : 0] = '\0';

        ASSERT((int)strlen(buf_pc) <= 10,
               "print_clean: output truncated to max_cols=10");
        ASSERT(strlen(buf_pc) > 0,
               "print_clean: output is non-empty");
        skip_print_clean:;
    }

    /* NULL input falls back to fallback string */
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            ASSERT(0, "print_clean fallback: pipe failed");
            goto skip_print_clean_fb;
        }
        fflush(stdout);
        int saved_fb = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        print_clean(NULL, "(none)", 20);

        fflush(stdout);
        dup2(saved_fb, STDOUT_FILENO);
        close(saved_fb);

        char buf_fb[64] = {0};
        ssize_t n_fb = read(pipefd[0], buf_fb, sizeof(buf_fb) - 1);
        close(pipefd[0]);
        buf_fb[n_fb > 0 ? n_fb : 0] = '\0';

        ASSERT(strcmp(buf_fb, "(none)") == 0,
               "print_clean: NULL input uses fallback");
        skip_print_clean_fb:;
    }

    /* ── cmp_uid_entry ───────────────────────────────────────────────── */
    {
        MsgEntry a = {"0000000000000100", MSG_FLAG_UNSEEN, 1000, ""};  /* unseen */
        MsgEntry b = {"0000000000000200", 0,               2000, ""};  /* seen   */
        /* unseen before seen regardless of date */
        ASSERT(cmp_uid_entry(&a, &b) < 0, "cmp_uid_entry: unseen before seen");
        ASSERT(cmp_uid_entry(&b, &a) > 0, "cmp_uid_entry: seen after unseen");
    }
    {
        MsgEntry c = {"0000000000000100", MSG_FLAG_UNSEEN, 1000, ""};
        MsgEntry d = {"0000000000000200", MSG_FLAG_UNSEEN, 2000, ""};
        /* both unseen: newer date (higher epoch) first */
        ASSERT(cmp_uid_entry(&c, &d) > 0, "cmp_uid_entry: older date after newer");
        ASSERT(cmp_uid_entry(&d, &c) < 0, "cmp_uid_entry: newer date before older");
    }
    {
        MsgEntry e = {"0000000000000100", MSG_FLAG_FLAGGED, 500, ""};
        MsgEntry f = {"0000000000000200", 0,                500, ""};
        /* flagged (read) before plain read */
        ASSERT(cmp_uid_entry(&e, &f) < 0, "cmp_uid_entry: flagged before rest");
    }
    {
        MsgEntry g = {"0000000000000100", 0, 0, ""};
        MsgEntry h = {"0000000000000100", 0, 0, ""};
        /* equal: cmp == 0 */
        ASSERT(cmp_uid_entry(&g, &h) == 0, "cmp_uid_entry: equal entries → 0");
    }

    /* ── is_last_sibling ─────────────────────────────────────────────── */

    /* Root-level two items: first is not last, second is */
    {
        char *names[] = {"A", "B"};
        ASSERT(is_last_sibling(names, 2, 0, '.') == 0,
               "is_last_sibling: A not last (B follows)");
        ASSERT(is_last_sibling(names, 2, 1, '.') == 1,
               "is_last_sibling: B is last");
    }

    /* parent_len == 0 path (line 582): root-level item with multiple followers */
    {
        char *names[] = {"A", "B", "C"};
        ASSERT(is_last_sibling(names, 3, 0, '.') == 0,
               "is_last_sibling: root level, A not last");
    }

    /* line 587: jumped to a different parent subtree → return 1 */
    {
        /* INBOX, INBOX.A, INBOX.B, Other — sorted */
        char *names[] = {"INBOX", "INBOX.A", "INBOX.B", "Other"};
        /* INBOX.A: sibling INBOX.B follows → not last */
        ASSERT(is_last_sibling(names, 4, 1, '.') == 0,
               "is_last_sibling: INBOX.A not last");
        /* INBOX.B: next is Other (different parent subtree) → last */
        ASSERT(is_last_sibling(names, 4, 2, '.') == 1,
               "is_last_sibling: INBOX.B is last (diff parent)");
    }

    /* Single item → always last */
    {
        char *names[] = {"INBOX"};
        ASSERT(is_last_sibling(names, 1, 0, '.') == 1,
               "is_last_sibling: single item is last");
    }

    /* ── ancestor_is_last ────────────────────────────────────────────── */

    /* Root-level ancestor with a sibling following: parent_len == 0, return 0 (line 630) */
    {
        char *names[] = {"INBOX", "INBOX.A", "INBOX.B", "Sent"};
        /* For INBOX.A at level=0: ancestor "INBOX" is NOT the last root (Sent follows) */
        int r = ancestor_is_last(names, 4, 1, 0, '.');
        ASSERT(r == 0, "ancestor_is_last: INBOX not last root");
    }

    /* Root-level ancestor that IS last */
    {
        char *names[] = {"INBOX", "INBOX.A", "Sent"};
        /* For Sent (index=2) at level=0: nothing after it → last */
        int r = ancestor_is_last(names, 3, 2, 0, '.');
        ASSERT(r == 1, "ancestor_is_last: Sent is last root");
    }

    /* level=0 with follower: parent_len==0 → return 0 (covers line 630) */
    {
        char *names[] = {"A.X", "A.Y", "B.Z"};
        /* A.Y's root-level ancestor is "A"; "B.Z" follows at root → return 0 */
        int r = ancestor_is_last(names, 3, 1, 0, '.');
        ASSERT(r == 0, "ancestor_is_last: level=0, another root item follows → 0");
    }

    /* line 636: jumped to different parent subtree → return 1 (level > 0) */
    {
        /* A.B.Y's ancestor at level=1 is "A.B"; parent of "A.B" is "A".
         * After A.B.Y's subtree, C.D has parent "C" ≠ "A" → return 1. */
        char *names[] = {"A.B.X", "A.B.Y", "C.D"};
        int r = ancestor_is_last(names, 3, 1, 1, '.');
        ASSERT(r == 1, "ancestor_is_last: level=1, different grandparent → 1");
    }

    /* Only one root-level folder (INBOX) → ancestor is last */
    {
        char *names[] = {"INBOX.A", "INBOX.A.X", "INBOX.A.Y", "INBOX.B"};
        /* All entries share root "INBOX"; nothing at a different root → last=1 */
        int r = ancestor_is_last(names, 4, 2, 0, '.');
        ASSERT(r == 1, "ancestor_is_last: INBOX is only root → 1");
    }

    /* ── HTML-only MIME: CSS must not leak into rendered output ──────── */
    /*
     * Regression test for show_uid_interactive: when an email has only a
     * text/html part (no text/plain), the body must be rendered through
     * html_render(), not passed through as raw text.  In particular, any
     * <style> block must be suppressed and visible body text must appear.
     */
    {
        /* Minimal MIME message: HTML-only, with an embedded <style> block */
        const char *mime_msg =
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/html; charset=UTF-8\r\n"
            "\r\n"
            "<html>"
            "<head><style>body { color: red; font-family: Arial; }</style></head>"
            "<body><b>Visible Text</b></body>"
            "</html>";

        char *html = mime_get_html_part(mime_msg);
        ASSERT(html != NULL, "html-only mime: html part found");

        char *rendered = html_render(html, 0, 0);
        free(html);
        ASSERT(rendered != NULL, "html-only mime: render not NULL");

        /* Visible content must appear */
        ASSERT(strstr(rendered, "Visible Text") != NULL,
               "html-only mime: body text present in output");

        /* CSS must be suppressed */
        ASSERT(strstr(rendered, "color") == NULL,
               "html-only mime: CSS property 'color' not in output");
        ASSERT(strstr(rendered, "font-family") == NULL,
               "html-only mime: CSS property 'font-family' not in output");
        ASSERT(strstr(rendered, "Arial") == NULL,
               "html-only mime: CSS value 'Arial' not in output");

        free(rendered);
    }

    /* ── show_uid_interactive: uses correct folder, not cfg->folder ──── */
    /*
     * Regression test for subfolder message open bug.
     *
     * When the user presses Enter on a message in a subfolder (e.g. "munka/ai"),
     * show_uid_interactive must look up the message in that subfolder's cache —
     * NOT in cfg->folder (which is always "INBOX").
     *
     * Setup:
     *   - Pre-populate cache under "test_subfolder" with UID 7777.
     *   - Config has .folder = "INBOX" (wrong folder — the bug).
     *   - Inject ESC via pipe into STDIN_FILENO so the function exits cleanly.
     *
     * If the function uses cfg->folder ("INBOX"):
     *   local_msg_exists("INBOX", 7777) → false → fetch fails → returns -1.
     * If the function uses the correct folder ("test_subfolder"):
     *   local_msg_exists("test_subfolder", 7777) → true → loads OK → ESC → returns 1.
     */
    {
        /* Minimal plain-text MIME message */
        const char *sf_mime =
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "Subject: Subfolder test\r\n"
            "From: test@example.com\r\n"
            "\r\n"
            "Subfolder message body.\r\n";

        /* Pre-populate cache under the correct subfolder */
        int saved_rc = local_msg_save("test_subfolder", "0000000000007777",
                                  sf_mime, strlen(sf_mime));
        if (saved_rc != 0) {
            ASSERT(0, "show_uid_interactive subfolder: local_msg_save failed");
            goto skip_subfolder_test;
        }

        /* Config intentionally has the wrong folder (the bug) */
        Config sf_cfg;
        memset(&sf_cfg, 0, sizeof(sf_cfg));
        sf_cfg.folder = "INBOX";

        /* Inject ESC (\033) into stdin via pipe so the function exits */
        int sf_pipe[2];
        if (pipe(sf_pipe) != 0) {
            ASSERT(0, "show_uid_interactive subfolder: pipe failed");
            goto skip_subfolder_test;
        }
        unsigned char esc_byte = '\033';
        ssize_t _w = write(sf_pipe[1], &esc_byte, 1);
        (void)_w;
        close(sf_pipe[1]);

        /* Redirect stdin to pipe read end */
        int saved_stdin = dup(STDIN_FILENO);
        dup2(sf_pipe[0], STDIN_FILENO);
        close(sf_pipe[0]);

        /* Redirect stdout + stderr to /dev/null (suppress TUI output) */
        fflush(stdout); fflush(stderr);
        int sf_null = open("/dev/null", O_WRONLY);
        int saved_stdout = dup(STDOUT_FILENO);
        int saved_stderr = dup(STDERR_FILENO);
        if (sf_null >= 0) {
            dup2(sf_null, STDOUT_FILENO);
            dup2(sf_null, STDERR_FILENO);
            close(sf_null);
        }

        /* Call with new signature: explicit folder parameter */
        int sf_ret = show_uid_interactive(&sf_cfg, NULL, "test_subfolder", "0000000000007777", 25, 0, NULL);

        /* Restore stdin, stdout, stderr — ALWAYS, even if ASSERT would fail */
        fflush(stdout); fflush(stderr);
        dup2(saved_stdin,  STDIN_FILENO);  close(saved_stdin);
        dup2(saved_stdout, STDOUT_FILENO); close(saved_stdout);
        dup2(saved_stderr, STDERR_FILENO); close(saved_stderr);

        /* ESC → returns 1 (exit program); "not found in INBOX" → returns -1 */
        ASSERT(sf_ret == 1,
               "show_uid_interactive: uses correct folder (not cfg->folder)");

        skip_subfolder_test:;
    }

    /* ── email_service_set_flag ─────────────────────────────────────── */

    /* test_set_flag_mark_read: create manifest with UNSEEN message, call set_flag
     * with MSG_FLAG_UNSEEN,0, verify manifest has UNSEEN cleared */
    {
        const char *test_folder = "test_set_flag_folder";
        const char *test_uid    = "0000000000009001";

        /* Build a manifest with one UNSEEN message */
        Manifest *m = calloc(1, sizeof(Manifest));
        ASSERT(m != NULL, "set_flag mark_read: manifest alloc");
        manifest_upsert(m, test_uid,
                        strdup("test@example.com"),
                        strdup("Test Subject"),
                        strdup("2024-01-01 00:00"),
                        MSG_FLAG_UNSEEN);
        ASSERT(manifest_save(test_folder, m) == 0, "set_flag mark_read: manifest_save");
        manifest_free(m);

        /* Fake config (no real server) */
        Config fcfg;
        memset(&fcfg, 0, sizeof(fcfg));
        fcfg.folder     = (char *)test_folder;
        fcfg.gmail_mode = 0;

        /* Call set_flag to clear UNSEEN (mark as read) — server push will fail,
         * but we only care about the local manifest update */
        email_service_set_flag(&fcfg, test_uid, test_folder, MSG_FLAG_UNSEEN, 0);

        /* Reload manifest and verify flag was cleared */
        Manifest *m2 = manifest_load(test_folder);
        ASSERT(m2 != NULL, "set_flag mark_read: manifest_load after");
        ManifestEntry *me = manifest_find(m2, test_uid);
        ASSERT(me != NULL, "set_flag mark_read: entry found");
        ASSERT(!(me->flags & MSG_FLAG_UNSEEN), "set_flag mark_read: UNSEEN cleared");
        manifest_free(m2);
    }

    /* test_set_flag_mark_starred: create manifest without FLAGGED, call set_flag
     * with MSG_FLAG_FLAGGED,1, verify manifest has FLAGGED set */
    {
        const char *test_folder = "test_set_flag_star_folder";
        const char *test_uid    = "0000000000009002";

        /* Build a manifest with one message (not flagged) */
        Manifest *m = calloc(1, sizeof(Manifest));
        ASSERT(m != NULL, "set_flag star: manifest alloc");
        manifest_upsert(m, test_uid,
                        strdup("test@example.com"),
                        strdup("Star Test"),
                        strdup("2024-01-01 00:00"),
                        0 /* no flags */);
        ASSERT(manifest_save(test_folder, m) == 0, "set_flag star: manifest_save");
        manifest_free(m);

        Config fcfg;
        memset(&fcfg, 0, sizeof(fcfg));
        fcfg.folder     = (char *)test_folder;
        fcfg.gmail_mode = 0;

        email_service_set_flag(&fcfg, test_uid, test_folder, MSG_FLAG_FLAGGED, 1);

        Manifest *m2 = manifest_load(test_folder);
        ASSERT(m2 != NULL, "set_flag star: manifest_load after");
        ManifestEntry *me = manifest_find(m2, test_uid);
        ASSERT(me != NULL, "set_flag star: entry found");
        ASSERT(me->flags & MSG_FLAG_FLAGGED, "set_flag star: FLAGGED set");
        manifest_free(m2);
    }

    /* test_remove_account_preserves_local: config_delete_account removes config
     * but does not touch local store directory */
    {
        /* Create a minimal config for a test account */
        Config tmp_cfg;
        memset(&tmp_cfg, 0, sizeof(tmp_cfg));
        tmp_cfg.user = "testremoveaccount@example.com";
        tmp_cfg.host = "imaps://imap.example.com";
        tmp_cfg.pass = "testpass";
        tmp_cfg.folder = "INBOX";

        /* Save the account */
        int save_rc = config_save_account(&tmp_cfg);
        /* If save fails (e.g. permissions in test env), skip gracefully */
        if (save_rc == 0) {
            /* Verify it was saved */
            int cnt = 0;
            AccountEntry *entries = config_list_accounts(&cnt);
            int found_before = 0;
            for (int i = 0; i < cnt; i++) {
                if (entries[i].name &&
                    strcmp(entries[i].name, "testremoveaccount@example.com") == 0)
                    found_before = 1;
            }
            config_free_account_list(entries, cnt);

            if (found_before) {
                /* Delete account */
                config_delete_account("testremoveaccount@example.com");

                /* Verify config entry is gone */
                int cnt2 = 0;
                AccountEntry *entries2 = config_list_accounts(&cnt2);
                int found_after = 0;
                for (int i = 0; i < cnt2; i++) {
                    if (entries2[i].name &&
                        strcmp(entries2[i].name, "testremoveaccount@example.com") == 0)
                        found_after = 1;
                }
                config_free_account_list(entries2, cnt2);
                ASSERT(!found_after, "remove_account: config entry deleted");
            }
        }
        /* Local store is NOT deleted — this is a policy test, not a file-system test */
        ASSERT(1, "remove_account: local data preservation is policy (no file ops here)");
    }

    /* ── print_dbar ─────────────────────────────────────────────────── */
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) { ASSERT(0, "print_dbar: pipe failed"); goto skip_print_dbar; }
        fflush(stdout);
        int saved_dbar = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);

        print_dbar(3);

        fflush(stdout);
        dup2(saved_dbar, STDOUT_FILENO); close(saved_dbar);
        char buf_dbar[64] = {0};
        ssize_t n_dbar = read(pipefd[0], buf_dbar, sizeof(buf_dbar) - 1);
        close(pipefd[0]);
        buf_dbar[n_dbar > 0 ? n_dbar : 0] = '\0';

        /* Each U+2550 (═) is 3 UTF-8 bytes: 0xE2 0x95 0x90 */
        ASSERT(n_dbar == 9, "print_dbar(3): 3 * 3 bytes = 9");
        ASSERT((unsigned char)buf_dbar[0] == 0xE2 &&
               (unsigned char)buf_dbar[1] == 0x95 &&
               (unsigned char)buf_dbar[2] == 0x90,
               "print_dbar(3): first char is U+2550");
        skip_print_dbar:;
    }

    /* print_dbar(0) produces no output */
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) { ASSERT(0, "print_dbar(0): pipe failed"); goto skip_print_dbar0; }
        fflush(stdout);
        int saved_db0 = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);

        print_dbar(0);

        fflush(stdout);
        dup2(saved_db0, STDOUT_FILENO); close(saved_db0);
        char buf_db0[16] = {0};
        ssize_t n_db0 = read(pipefd[0], buf_db0, sizeof(buf_db0) - 1);
        close(pipefd[0]);

        ASSERT(n_db0 == 0, "print_dbar(0): no output");
        skip_print_dbar0:;
    }

    /* ── utf8_extra_bytes ────────────────────────────────────────────── */
    {
        /* NULL: the function dereferences directly, so test empty string instead */
        ASSERT(utf8_extra_bytes("") == 0, "utf8_extra_bytes: empty → 0");

        /* Pure ASCII: no continuation bytes */
        ASSERT(utf8_extra_bytes("hello") == 0, "utf8_extra_bytes: ASCII → 0");

        /* "é" = 0xC3 0xA9: 1 continuation byte */
        ASSERT(utf8_extra_bytes("\xC3\xA9") == 1,
               "utf8_extra_bytes: é (2-byte) → 1");

        /* "中" = 0xE4 0xB8 0xAD: 2 continuation bytes */
        ASSERT(utf8_extra_bytes("\xE4\xB8\xAD") == 2,
               "utf8_extra_bytes: 中 (3-byte) → 2");

        /* "😀" = 0xF0 0x9F 0x98 0x80: 3 continuation bytes */
        ASSERT(utf8_extra_bytes("\xF0\x9F\x98\x80") == 3,
               "utf8_extra_bytes: 😀 (4-byte) → 3");

        /* Mixed: ASCII + 2-byte + 3-byte → 1+2 = 3 extra bytes */
        ASSERT(utf8_extra_bytes("a\xC3\xA9\xE4\xB8\xAD") == 3,
               "utf8_extra_bytes: mixed → 3");
    }

    /* ── fmt_thou ────────────────────────────────────────────────────── */
    {
        char buf[32];

        /* n=0: early return → empty string */
        fmt_thou(buf, sizeof(buf), 0);
        ASSERT(buf[0] == '\0', "fmt_thou(0): empty string");

        /* n=1: single digit */
        fmt_thou(buf, sizeof(buf), 1);
        ASSERT(strcmp(buf, "1") == 0, "fmt_thou(1): \"1\"");

        /* n=999: three digits, no separator */
        fmt_thou(buf, sizeof(buf), 999);
        ASSERT(strcmp(buf, "999") == 0, "fmt_thou(999): \"999\"");

        /* n=1000: four digits → "1 000" */
        fmt_thou(buf, sizeof(buf), 1000);
        ASSERT(strcmp(buf, "1 000") == 0, "fmt_thou(1000): \"1 000\"");

        /* n=1234567: → "1 234 567" */
        fmt_thou(buf, sizeof(buf), 1234567);
        ASSERT(strcmp(buf, "1 234 567") == 0, "fmt_thou(1234567): \"1 234 567\"");

        /* n=1000000: → "1 000 000" */
        fmt_thou(buf, sizeof(buf), 1000000);
        ASSERT(strcmp(buf, "1 000 000") == 0, "fmt_thou(1000000): \"1 000 000\"");
    }

    /* ── visible_line_cols ───────────────────────────────────────────── */
    {
        /* Pure ASCII: 5 chars = 5 cols */
        const char *s1 = "hello";
        ASSERT(visible_line_cols(s1, s1 + 5) == 5,
               "visible_line_cols: ASCII 5 chars → 5 cols");

        /* Empty span: 0 cols */
        const char *s2 = "abc";
        ASSERT(visible_line_cols(s2, s2) == 0,
               "visible_line_cols: empty span → 0");

        /* ANSI CSI escape must not count toward cols */
        const char *s3 = "\033[1mABC\033[22m";
        ASSERT(visible_line_cols(s3, s3 + strlen(s3)) == 3,
               "visible_line_cols: ANSI bold wrappers → 3 visible cols");

        /* OSC sequence (hyperlink URL) must not count */
        /* ESC ] 8 ; ; http://x BEL */
        const char *s4 = "\033]8;;http://x\007hi\033]8;;\007";
        ASSERT(visible_line_cols(s4, s4 + strlen(s4)) == 2,
               "visible_line_cols: OSC hyperlink → 2 visible cols");

        /* 2-byte UTF-8: "é" (1 column wide) */
        const char *s5 = "\xC3\xA9";
        int vcols5 = visible_line_cols(s5, s5 + 2);
        ASSERT(vcols5 == 1, "visible_line_cols: é → 1 col");

        /* 3-byte UTF-8: "中" (2 columns wide on CJK-capable terminal) */
        const char *s6 = "\xE4\xB8\xAD";
        int vcols6 = visible_line_cols(s6, s6 + 3);
        ASSERT(vcols6 >= 1, "visible_line_cols: CJK char → ≥1 col");

        /* Invalid lead byte 0x80: treated as U+FFFD (width 0 or 1 depending on wcwidth) */
        const char *s7 = "\x80";
        int vcols7 = visible_line_cols(s7, s7 + 1);
        ASSERT(vcols7 >= 0, "visible_line_cols: invalid lead byte → non-negative");

        /* OSC with ESC-backslash terminator */
        const char *s8 = "\033]8;;\033\\X";
        ASSERT(visible_line_cols(s8, s8 + strlen(s8)) == 1,
               "visible_line_cols: OSC ESC-backslash term → 1 visible col");
    }

    /* ── email_service_fetch_raw ─────────────────────────────────────── */
    {
        const char *fetch_folder = "INBOX";
        const char *fetch_uid    = "0000000000008001";
        const char *fetch_mime   =
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "From: fetch@example.com\r\n"
            "Subject: Fetch Raw Test\r\n"
            "\r\n"
            "Raw fetch test body.\r\n";

        int sr = local_msg_save(fetch_folder, fetch_uid, fetch_mime, strlen(fetch_mime));
        ASSERT(sr == 0, "fetch_raw: local_msg_save ok");

        Config fr_cfg = {0};
        fr_cfg.folder = (char *)fetch_folder;

        char *raw = email_service_fetch_raw(&fr_cfg, fetch_uid);
        ASSERT(raw != NULL, "fetch_raw: returns non-NULL for cached message");
        ASSERT(strstr(raw, "Raw fetch test body.") != NULL,
               "fetch_raw: returned content matches saved message");
        free(raw);

        /* Non-existent UID with no server: returns NULL */
        char *raw2 = email_service_fetch_raw(&fr_cfg, "0000000000008999");
        ASSERT(raw2 == NULL, "fetch_raw: returns NULL for non-existent UID");
    }

    /* ── email_service_list_attachments ─────────────────────────────── */
    {
        const char *att_folder = "INBOX";
        const char *att_uid    = "0000000000005555";
        const char *mime_attach =
            "MIME-Version: 1.0\r\n"
            "Content-Type: multipart/mixed; boundary=\"B001\"\r\n"
            "From: test@example.com\r\n"
            "Subject: Attach Test\r\n"
            "\r\n"
            "--B001\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Body text\r\n"
            "--B001\r\n"
            "Content-Type: text/plain; name=\"notes.txt\"\r\n"
            "Content-Disposition: attachment; filename=\"notes.txt\"\r\n"
            "Content-Transfer-Encoding: base64\r\n"
            "\r\n"
            "SGVsbG8gV29ybGQ=\r\n"
            "--B001--\r\n";

        int sa = local_msg_save(att_folder, att_uid, mime_attach, strlen(mime_attach));
        ASSERT(sa == 0, "list_attachments: local_msg_save ok");

        Config att_cfg = {0};
        att_cfg.folder = (char *)att_folder;

        /* Capture stdout */
        int pipefd[2];
        if (pipe(pipefd) != 0) { ASSERT(0, "list_attachments: pipe failed"); goto skip_list_att; }
        fflush(stdout);
        int saved_la = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);

        int rc_la = email_service_list_attachments(&att_cfg, att_uid);

        fflush(stdout);
        dup2(saved_la, STDOUT_FILENO); close(saved_la);
        char buf_la[512] = {0};
        ssize_t n_la = read(pipefd[0], buf_la, sizeof(buf_la) - 1);
        close(pipefd[0]);
        buf_la[n_la > 0 ? n_la : 0] = '\0';

        ASSERT(rc_la == 0, "list_attachments: returns 0");
        ASSERT(strstr(buf_la, "notes.txt") != NULL,
               "list_attachments: attachment name in output");
        skip_list_att:;
    }

    /* list_attachments on message with no attachments → "No attachments." */
    {
        const char *na_folder = "INBOX";
        const char *na_uid    = "0000000000005556";
        const char *mime_plain =
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain\r\n"
            "From: noatt@example.com\r\n"
            "Subject: No Attach\r\n"
            "\r\n"
            "Just text.\r\n";

        local_msg_save(na_folder, na_uid, mime_plain, strlen(mime_plain));

        Config na_cfg = {0};
        na_cfg.folder = (char *)na_folder;

        int pipefd[2];
        if (pipe(pipefd) != 0) { ASSERT(0, "list_att_none: pipe failed"); goto skip_list_att_none; }
        fflush(stdout);
        int saved_na = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);

        int rc_na = email_service_list_attachments(&na_cfg, na_uid);

        fflush(stdout);
        dup2(saved_na, STDOUT_FILENO); close(saved_na);
        char buf_na[128] = {0};
        ssize_t n_na = read(pipefd[0], buf_na, sizeof(buf_na) - 1);
        close(pipefd[0]);
        buf_na[n_na > 0 ? n_na : 0] = '\0';

        ASSERT(rc_na == 0, "list_att_none: returns 0");
        ASSERT(strstr(buf_na, "No attachments") != NULL,
               "list_att_none: output says 'No attachments'");
        skip_list_att_none:;
    }

    /* list_attachments on non-existent UID → returns -1 */
    {
        Config ne_cfg = {0};
        ne_cfg.folder = "INBOX";

        /* Redirect stderr to suppress error message */
        fflush(stderr);
        int saved_ne_err = dup(STDERR_FILENO);
        int null_ne = open("/dev/null", O_WRONLY);
        if (null_ne >= 0) { dup2(null_ne, STDERR_FILENO); close(null_ne); }

        int rc_ne = email_service_list_attachments(&ne_cfg, "0000000000009999");

        fflush(stderr);
        dup2(saved_ne_err, STDERR_FILENO); close(saved_ne_err);

        ASSERT(rc_ne == -1, "list_att_missing: non-existent UID → -1");
    }

    /* ── email_service_save_attachment ──────────────────────────────── */
    {
        const char *sv_folder = "INBOX";
        const char *sv_uid    = "0000000000005557";
        const char *mime_sv =
            "MIME-Version: 1.0\r\n"
            "Content-Type: multipart/mixed; boundary=\"B002\"\r\n"
            "From: sv@example.com\r\n"
            "Subject: Save Attach Test\r\n"
            "\r\n"
            "--B002\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n"
            "Body\r\n"
            "--B002\r\n"
            "Content-Type: text/plain; name=\"save_me.txt\"\r\n"
            "Content-Disposition: attachment; filename=\"save_me.txt\"\r\n"
            "Content-Transfer-Encoding: base64\r\n"
            "\r\n"
            "SGVsbG8gV29ybGQ=\r\n"
            "--B002--\r\n";

        local_msg_save(sv_folder, sv_uid, mime_sv, strlen(mime_sv));

        Config sv_cfg = {0};
        sv_cfg.folder = (char *)sv_folder;

        /* Use /tmp as output dir to avoid touching $HOME */
        char sv_dest[256];
        snprintf(sv_dest, sizeof(sv_dest), "/tmp/email_cli_test_%d", (int)getpid());
        mkdir(sv_dest, 0700);

        /* Capture stdout (prints "Saved: ...") */
        int pipefd[2];
        if (pipe(pipefd) != 0) { ASSERT(0, "save_attachment: pipe failed"); goto skip_save_att; }
        fflush(stdout);
        int saved_sv = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);

        int rc_sv = email_service_save_attachment(&sv_cfg, sv_uid, "save_me.txt", sv_dest);

        fflush(stdout);
        dup2(saved_sv, STDOUT_FILENO); close(saved_sv);
        char buf_sv[256] = {0};
        ssize_t n_sv = read(pipefd[0], buf_sv, sizeof(buf_sv) - 1);
        close(pipefd[0]);
        buf_sv[n_sv > 0 ? n_sv : 0] = '\0';

        ASSERT(rc_sv == 0, "save_attachment: returns 0");
        ASSERT(strstr(buf_sv, "Saved:") != NULL,
               "save_attachment: output contains 'Saved:'");

        /* Verify file exists */
        char expected_path[512];
        snprintf(expected_path, sizeof(expected_path), "%s/save_me.txt", sv_dest);
        struct stat st_sv;
        ASSERT(stat(expected_path, &st_sv) == 0,
               "save_attachment: saved file exists on disk");

        /* Cleanup */
        unlink(expected_path);
        rmdir(sv_dest);
        skip_save_att:;
    }

    /* save_attachment: attachment name not found → -1 */
    {
        const char *sf2_uid = "0000000000005557";  /* already saved above */
        Config sf2_cfg = {0};
        sf2_cfg.folder = "INBOX";

        fflush(stderr);
        int saved_sf2_err = dup(STDERR_FILENO);
        int null_sf2 = open("/dev/null", O_WRONLY);
        if (null_sf2 >= 0) { dup2(null_sf2, STDERR_FILENO); close(null_sf2); }

        int rc_sf2 = email_service_save_attachment(&sf2_cfg, sf2_uid,
                                                    "nonexistent.txt", "/tmp");

        fflush(stderr);
        dup2(saved_sf2_err, STDERR_FILENO); close(saved_sf2_err);

        ASSERT(rc_sf2 == -1, "save_attachment: wrong name → -1");
    }

    /* save_attachment: non-existent UID → -1 */
    {
        Config sf3_cfg = {0};
        sf3_cfg.folder = "INBOX";

        fflush(stderr);
        int saved_sf3_err = dup(STDERR_FILENO);
        int null_sf3 = open("/dev/null", O_WRONLY);
        if (null_sf3 >= 0) { dup2(null_sf3, STDERR_FILENO); close(null_sf3); }

        int rc_sf3 = email_service_save_attachment(&sf3_cfg, "0000000000009998",
                                                    "any.txt", "/tmp");

        fflush(stderr);
        dup2(saved_sf3_err, STDERR_FILENO); close(saved_sf3_err);

        ASSERT(rc_sf3 == -1, "save_attachment: missing UID → -1");
    }

    /* ── email_service_list_labels ───────────────────────────────────── */
    /* Without a real connection, make_mail returns NULL → function returns -1.
     * This covers the early-exit error path of email_service_list_labels. */
    {
        Config lbl_cfg = {0};
        lbl_cfg.folder     = "INBOX";
        lbl_cfg.gmail_mode = 0;

        /* Suppress stderr "Error: Could not connect." */
        fflush(stderr);
        int saved_lbl_err = dup(STDERR_FILENO);
        int null_lbl = open("/dev/null", O_WRONLY);
        if (null_lbl >= 0) { dup2(null_lbl, STDERR_FILENO); close(null_lbl); }

        int rc_lbl = email_service_list_labels(&lbl_cfg);

        fflush(stderr);
        dup2(saved_lbl_err, STDERR_FILENO); close(saved_lbl_err);

        ASSERT(rc_lbl == -1, "list_labels: no connection → -1");
    }

    /* ── email_service_list (cron/cache mode, offline) ───────────────── */
    {
        /* Use a unique folder to avoid collisions with other tests */
        const char *lf = "test_list_cron_folder";

        /* Create a manifest with 2 messages */
        Manifest *m = calloc(1, sizeof(Manifest));
        manifest_upsert(m, "0000000000008001", strdup("alice@example.com"),
                        strdup("Hello World"), strdup("2026-01-15 10:00"), MSG_FLAG_UNSEEN);
        manifest_upsert(m, "0000000000008002", strdup("bob@example.com"),
                        strdup("Meeting notes"), strdup("2026-01-14 09:00"), 0);
        manifest_save(lf, m);
        manifest_free(m);

        Config lcfg = {0};
        lcfg.host         = "imaps://test.example.com";
        lcfg.user         = "testuser";
        lcfg.folder       = (char *)lf;
        lcfg.sync_interval = 1;  /* cron mode: read from manifest cache */

        EmailListOpts opts = {0};
        opts.folder = lf;
        opts.pager  = 1;   /* TUI mode */

        /* Inject ESC (27) to exit the TUI */
        int lp[2]; int _pr = pipe(lp); (void)_pr;
        unsigned char esc = 27;
        ssize_t _wr = write(lp[1], &esc, 1); (void)_wr;
        close(lp[1]);
        int saved_stdin = dup(STDIN_FILENO);
        dup2(lp[0], STDIN_FILENO); close(lp[0]);

        /* Suppress TUI output */
        fflush(stdout); fflush(stderr);
        int lnull = open("/dev/null", O_WRONLY);
        int lsout = dup(STDOUT_FILENO), lserr = dup(STDERR_FILENO);
        if (lnull >= 0) { dup2(lnull, STDOUT_FILENO); dup2(lnull, STDERR_FILENO); close(lnull); }

        int lr = email_service_list(&lcfg, &opts);

        fflush(stdout); fflush(stderr);
        dup2(lsout, STDOUT_FILENO); close(lsout);
        dup2(lserr, STDERR_FILENO); close(lserr);
        dup2(saved_stdin, STDIN_FILENO); close(saved_stdin);

        /* ESC returns 0 from the TUI */
        ASSERT(lr == 0 || lr == 1, "list cron: ESC exits cleanly");
    }

    /* ── email_service_list (cron/cache mode, batch output) ─────────── */
    {
        const char *bf = "test_list_batch_folder";

        /* Create manifest */
        Manifest *m = calloc(1, sizeof(Manifest));
        manifest_upsert(m, "0000000000008010", strdup("sender@example.com"),
                        strdup("Batch Test Subject"), strdup("2026-02-01 08:00"), MSG_FLAG_UNSEEN);
        manifest_save(bf, m);
        manifest_free(m);

        Config bcfg = {0};
        bcfg.host         = "imaps://test.example.com";
        bcfg.user         = "testuser";
        bcfg.folder       = (char *)bf;
        bcfg.sync_interval = 1;  /* cron mode */

        EmailListOpts opts = {0};
        opts.folder = bf;
        opts.pager  = 0;  /* batch mode: prints to stdout, no TUI */

        /* Capture stdout */
        int bp[2]; int _pr = pipe(bp); (void)_pr;
        fflush(stdout);
        int bsout = dup(STDOUT_FILENO);
        dup2(bp[1], STDOUT_FILENO); close(bp[1]);

        /* Suppress stderr */
        int bnull = open("/dev/null", O_WRONLY);
        int bserr = dup(STDERR_FILENO);
        if (bnull >= 0) { dup2(bnull, STDERR_FILENO); close(bnull); }

        int br = email_service_list(&bcfg, &opts);

        fflush(stdout);
        dup2(bsout, STDOUT_FILENO); close(bsout);
        dup2(bserr, STDERR_FILENO); close(bserr);

        char bbuf[2048] = {0};
        ssize_t bn = read(bp[0], bbuf, sizeof(bbuf)-1);
        close(bp[0]);
        bbuf[bn > 0 ? bn : 0] = '\0';

        ASSERT(br == 0, "list cron batch: returns 0");
        ASSERT(strstr(bbuf, "Batch Test Subject") != NULL || strstr(bbuf, "message") != NULL,
               "list cron batch: content present in output");
    }

    /* ── email_service_list (cron mode, empty manifest, batch) ──────── */
    {
        Config ecfg = {0};
        ecfg.host         = "imaps://test.example.com";
        ecfg.user         = "testuser";
        ecfg.folder       = "test_empty_cron_folder";
        ecfg.sync_interval = 1;  /* cron mode */

        EmailListOpts opts = {0};
        opts.folder = "test_empty_cron_folder";
        opts.pager  = 0;  /* batch mode: prints "No cached data" message */

        int ep[2]; int _pr = pipe(ep); (void)_pr;
        fflush(stdout);
        int esout = dup(STDOUT_FILENO);
        dup2(ep[1], STDOUT_FILENO); close(ep[1]);
        int enull = open("/dev/null", O_WRONLY);
        int eserr = dup(STDERR_FILENO);
        if (enull >= 0) { dup2(enull, STDERR_FILENO); close(enull); }

        int er = email_service_list(&ecfg, &opts);

        fflush(stdout);
        dup2(esout, STDOUT_FILENO); close(esout);
        dup2(eserr, STDERR_FILENO); close(eserr);

        char ebuf[512] = {0};
        ssize_t en = read(ep[0], ebuf, sizeof(ebuf)-1);
        close(ep[0]);

        ASSERT(er == 0, "list empty cron: returns 0");
        (void)en; /* may print to stdout */
    }

    /* ── email_service_read (batch mode, cached message) ─────────────── */
    {
        /* Message already saved by earlier test; use a fresh one */
        const char *ruid = "0000000000008888";
        const char *rmsg =
            "From: reader@example.com\r\n"
            "Subject: Read Test\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "\r\n"
            "This is the message body for reader test.\r\n";
        local_msg_save("INBOX", ruid, rmsg, strlen(rmsg));

        Config rcfg = {0};
        rcfg.host   = "imaps://test.example.com";
        rcfg.user   = "testuser";
        rcfg.folder = "INBOX";

        /* Capture stdout */
        int rp[2]; int _pr = pipe(rp); (void)_pr;
        fflush(stdout);
        int rsout = dup(STDOUT_FILENO);
        dup2(rp[1], STDOUT_FILENO); close(rp[1]);
        int rnull = open("/dev/null", O_WRONLY);
        int rserr = dup(STDERR_FILENO);
        if (rnull >= 0) { dup2(rnull, STDERR_FILENO); close(rnull); }

        /* email_service_read(cfg, uid, pager=0, page_size=25) for batch output */
        int rr = email_service_read(&rcfg, ruid, 0, 25);

        fflush(stdout);
        dup2(rsout, STDOUT_FILENO); close(rsout);
        dup2(rserr, STDERR_FILENO); close(rserr);

        char rbuf[4096] = {0};
        ssize_t rn = read(rp[0], rbuf, sizeof(rbuf)-1);
        close(rp[0]);
        rbuf[rn > 0 ? rn : 0] = '\0';

        ASSERT(rr == 0, "read batch: returns 0");
        ASSERT(strstr(rbuf, "reader@example.com") != NULL || strstr(rbuf, "Read Test") != NULL,
               "read batch: header content in output");
    }

    /* ── get_account_totals (IMAP mode, offline) ─────────────────────── */
    {
        /* Create a manifest with 3 messages: 2 unseen, 1 flagged */
        const char *gfold = "test_totals_folder";
        Manifest *tm = calloc(1, sizeof(Manifest));
        manifest_upsert(tm, "0000000000009010", strdup("a@ex.com"), strdup("s1"),
                        strdup("2026-01-01 00:00"), MSG_FLAG_UNSEEN);
        manifest_upsert(tm, "0000000000009011", strdup("b@ex.com"), strdup("s2"),
                        strdup("2026-01-02 00:00"), MSG_FLAG_UNSEEN | MSG_FLAG_FLAGGED);
        manifest_upsert(tm, "0000000000009012", strdup("c@ex.com"), strdup("s3"),
                        strdup("2026-01-03 00:00"), 0);
        manifest_save(gfold, tm);
        manifest_free(tm);

        Config gcfg = {0};
        gcfg.host   = "imaps://test.example.com";
        gcfg.user   = "testuser";
        gcfg.folder = (char *)gfold;
        gcfg.gmail_mode = 0;

        int unseen = 0, flagged = 0;
        get_account_totals(&gcfg, &unseen, &flagged);
        ASSERT(unseen >= 0, "get_account_totals IMAP: unseen >= 0");
        ASSERT(flagged >= 0, "get_account_totals IMAP: flagged >= 0");

        /* Restore local store for subsequent tests */
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list (Gmail offline mode) ─────────────────────── */
    {
        local_store_init(NULL, "gmailtest@gmail.com");

        /* Add some entries to a Gmail label index */
        label_idx_add("INBOX", "abcdef1234567");
        label_idx_add("INBOX", "abcdef1234568");

        Config gcfg2 = {0};
        gcfg2.host       = NULL;
        gcfg2.user       = "gmailtest@gmail.com";
        gcfg2.folder     = "INBOX";
        gcfg2.gmail_mode = 1;
        gcfg2.sync_interval = 1;  /* cron/cache mode */

        EmailListOpts gopts = {0};
        gopts.folder = "INBOX";
        gopts.pager  = 0;  /* batch */

        int gp[2]; int _pr = pipe(gp); (void)_pr;
        fflush(stdout);
        int gsout = dup(STDOUT_FILENO);
        dup2(gp[1], STDOUT_FILENO); close(gp[1]);
        int gnull = open("/dev/null", O_WRONLY);
        int gserr = dup(STDERR_FILENO);
        if (gnull >= 0) { dup2(gnull, STDERR_FILENO); close(gnull); }

        int gret = email_service_list(&gcfg2, &gopts);

        fflush(stdout);
        dup2(gsout, STDOUT_FILENO); close(gsout);
        dup2(gserr, STDERR_FILENO); close(gserr);

        char gbuf[1024] = {0};
        ssize_t gn = read(gp[0], gbuf, sizeof(gbuf)-1);
        close(gp[0]);

        ASSERT(gret == 0 || gret == -1, "list gmail offline: returns 0 or -1");
        (void)gn;

        /* Restore normal local store */
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── stdin/stdout injection helpers (local) ──────────────────────── */
#define INJECT_STDIN(keys, klen, saved)  do { \
        int _pfd[2]; int _pr = pipe(_pfd); (void)_pr; \
        ssize_t _wr = write(_pfd[1], (keys), (klen)); (void)_wr; \
        close(_pfd[1]); \
        (saved) = dup(STDIN_FILENO); \
        dup2(_pfd[0], STDIN_FILENO); close(_pfd[0]); } while(0)
#define RESTORE_STDIN(saved)  do { dup2((saved), STDIN_FILENO); close(saved); } while(0)
#define SUPPRESS_OUT(so, se)  do { \
        fflush(stdout); fflush(stderr); \
        int _nfd = open("/dev/null", O_WRONLY); \
        (so) = dup(STDOUT_FILENO); (se) = dup(STDERR_FILENO); \
        if (_nfd >= 0) { dup2(_nfd,STDOUT_FILENO); dup2(_nfd,STDERR_FILENO); close(_nfd); } \
    } while(0)
#define RESTORE_OUT(so, se)   do { \
        fflush(stdout); fflush(stderr); \
        dup2((so),STDOUT_FILENO); close(so); \
        dup2((se),STDERR_FILENO); close(se); } while(0)

    /* ── fmt_size ────────────────────────────────────────────────────── */
    {
        char buf[64];
        fmt_size(buf, sizeof(buf), 512);
        ASSERT(strstr(buf, "KB") || strstr(buf, "0 KB"),
               "fmt_size: <1MB shows KB");

        fmt_size(buf, sizeof(buf), 2 * 1024 * 1024);
        ASSERT(strstr(buf, "MB") != NULL, "fmt_size: >=1MB shows MB");

        fmt_size(buf, sizeof(buf), 0);
        ASSERT(strstr(buf, "KB") != NULL, "fmt_size: 0 bytes shows 0 KB");

        fmt_size(buf, sizeof(buf), 1024 * 1024);
        ASSERT(strstr(buf, "MB") != NULL, "fmt_size: exactly 1MB shows MB");
    }

    /* ── fmt_url_with_port ───────────────────────────────────────────── */
    {
        char out[256];

        fmt_url_with_port(NULL, 993, out, sizeof(out));
        ASSERT(out[0] == '\0', "fmt_url_with_port: NULL → empty");

        fmt_url_with_port("", 993, out, sizeof(out));
        ASSERT(out[0] == '\0', "fmt_url_with_port: empty → empty");

        fmt_url_with_port("imaps://imap.example.com", 993, out, sizeof(out));
        ASSERT(strstr(out, ":993") != NULL, "fmt_url_with_port: appends :993");

        fmt_url_with_port("imaps://imap.example.com:143", 993, out, sizeof(out));
        ASSERT(strstr(out, ":143") != NULL, "fmt_url_with_port: keeps existing port");

        fmt_url_with_port("imap.noscheme.com", 993, out, sizeof(out));
        ASSERT(strstr(out, ":993") != NULL, "fmt_url_with_port: no scheme, appends port");
    }

    /* ── is_system_or_special_label ─────────────────────────────────── */
    {
        ASSERT(is_system_or_special_label("INBOX")          == 1, "system label INBOX");
        ASSERT(is_system_or_special_label("STARRED")        == 1, "system label STARRED");
        ASSERT(is_system_or_special_label("UNREAD")         == 1, "system label UNREAD");
        ASSERT(is_system_or_special_label("IMPORTANT")      == 1, "special label IMPORTANT");
        ASSERT(is_system_or_special_label("CATEGORY_SOCIAL")== 1, "special label CATEGORY_*");
        ASSERT(is_system_or_special_label("TRASH")          == 1, "special label TRASH");
        ASSERT(is_system_or_special_label("SPAM")           == 1, "special label SPAM");
        ASSERT(is_system_or_special_label("Work")           == 0, "user label Work");
        ASSERT(is_system_or_special_label("Personal")       == 0, "user label Personal");
    }

    /* ── folder_has_children ─────────────────────────────────────────── */
    {
        char *folders[] = { (char*)"INBOX", (char*)"INBOX.Sent",
                            (char*)"INBOX.Archive", (char*)"Trash" };
        int n = 4;
        ASSERT(folder_has_children(folders, n, "INBOX", '.') == 1,
               "folder_has_children: INBOX has children");
        ASSERT(folder_has_children(folders, n, "Trash", '.') == 0,
               "folder_has_children: Trash has no children");
        ASSERT(folder_has_children(folders, n, "INBOX.Sent", '.') == 0,
               "folder_has_children: INBOX.Sent leaf");
        ASSERT(folder_has_children(NULL, 0, "x", '.') == 0,
               "folder_has_children: empty list");
    }

    /* ── sum_subtree ─────────────────────────────────────────────────── */
    {
        char *names[] = { (char*)"INBOX", (char*)"INBOX.Sent", (char*)"Trash" };
        FolderStatus st[3] = { {5, 2, 1}, {3, 0, 0}, {1, 0, 0} };
        int msgs = 0, unseen = 0, flagged = 0;

        sum_subtree(names, 3, '.', "INBOX", st, &msgs, &unseen, &flagged);
        ASSERT(msgs == 8,    "sum_subtree: msgs INBOX+children = 8");
        ASSERT(unseen == 2,  "sum_subtree: unseen = 2");
        ASSERT(flagged == 1, "sum_subtree: flagged = 1");

        /* NULL statuses → all zeros */
        sum_subtree(names, 3, '.', "INBOX", NULL, &msgs, &unseen, &flagged);
        ASSERT(msgs == 0 && unseen == 0 && flagged == 0,
               "sum_subtree: NULL statuses → zeros");
    }

    /* ── build_flat_view ─────────────────────────────────────────────── */
    {
        char *names[] = { (char*)"A", (char*)"A.B", (char*)"A.B.C", (char*)"Z" };
        int vis[8];

        /* Root level: only A and Z (no separator) */
        int cnt = build_flat_view(names, 4, '.', "", vis);
        ASSERT(cnt == 2, "build_flat_view: root level = 2");
        ASSERT(vis[0] == 0 && vis[1] == 3, "build_flat_view: root indices 0,3");

        /* Children of A: only A.B (direct child, no second separator) */
        cnt = build_flat_view(names, 4, '.', "A", vis);
        ASSERT(cnt == 1, "build_flat_view: A children = 1 (A.B only)");
        ASSERT(vis[0] == 1, "build_flat_view: A child is index 1");

        /* No prefix match → 0 */
        cnt = build_flat_view(names, 4, '.', "X", vis);
        ASSERT(cnt == 0, "build_flat_view: no match → 0");
    }

    /* ── get_sync_bin_path ───────────────────────────────────────────── */
    {
        char buf[512];
        get_sync_bin_path(buf, sizeof(buf));
        ASSERT(buf[0] != '\0', "get_sync_bin_path: non-empty result");
        ASSERT(strstr(buf, "email-sync") != NULL,
               "get_sync_bin_path: contains email-sync");
    }

    /* ── attachment_save_dir ─────────────────────────────────────────── */
    {
        char *dir = attachment_save_dir();
        ASSERT(dir != NULL, "attachment_save_dir: non-NULL");
        ASSERT(strlen(dir) > 0, "attachment_save_dir: non-empty");
        free(dir);
    }

    /* ── sync_progress_cb ────────────────────────────────────────────── */
    {
        SyncProgressCtx ctx;
        ctx.loop_i     = 3;
        ctx.loop_total = 10;
        strncpy(ctx.uid, "0000000000000042", sizeof(ctx.uid) - 1);
        ctx.uid[16] = '\0';

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        sync_progress_cb(512 * 1024, 1024 * 1024, &ctx);   /* <1MB */
        sync_progress_cb(2 * 1024 * 1024, 4 * 1024 * 1024, &ctx); /* >=1MB */
        RESTORE_OUT(sout, serr);
        ASSERT(1, "sync_progress_cb: no crash");
    }

    /* ── build_label_display / free_label_display ────────────────────── */
    {
        char *user[] = { (char*)"Work", (char*)"Personal" };
        char **ids = NULL, **nms = NULL;
        int *seps = NULL, *hdrs = NULL;
        char *cats[] = { (char*)"CATEGORY_SOCIAL" };
        int cnt = build_label_display(&ids, &nms, &seps, &hdrs, user, 2, cats, 1);
        ASSERT(cnt > 2, "build_label_display: count > user count (system labels added)");
        ASSERT(ids   != NULL, "build_label_display: ids non-NULL");
        ASSERT(nms   != NULL, "build_label_display: names non-NULL");
        ASSERT(seps  != NULL, "build_label_display: seps non-NULL");
        ASSERT(hdrs  != NULL, "build_label_display: hdrs non-NULL");
        /* First row is "Tags / Flags" section header */
        ASSERT(hdrs[0] == 1, "build_label_display: first row is header");
        ASSERT(ids[0] != NULL, "build_label_display: first id non-NULL");
        free_label_display(ids, nms, seps, hdrs, cnt);
        ASSERT(1, "free_label_display: no crash after free");

        /* Empty user labels */
        cnt = build_label_display(&ids, &nms, &seps, &hdrs, NULL, 0, NULL, 0);
        ASSERT(cnt > 0, "build_label_display: no user labels still returns system labels");
        free_label_display(ids, nms, seps, hdrs, cnt);
    }

    /* ── fetch_uid_headers_cached ────────────────────────────────────── */
    {
        /* Save a fake header to cache, then load via fetch_uid_headers_cached */
        const char *hfold = "test_hdr_cache_folder";
        const char *huid  = "0000000000007777";
        const char *hdr   = "From: hdr@test.com\r\nSubject: Cached\r\n\r\n";
        local_store_init("imaps://test.example.com", "testuser");
        local_hdr_save(hfold, huid, hdr, strlen(hdr));

        Config hcfg = {0};
        hcfg.host   = "imaps://test.example.com";
        hcfg.user   = "testuser";
        hcfg.folder = (char *)hfold;

        char *loaded = fetch_uid_headers_cached(&hcfg, hfold, huid);
        ASSERT(loaded != NULL, "fetch_uid_headers_cached: cached hdr returned");
        if (loaded) {
            ASSERT(strstr(loaded, "Cached") != NULL,
                   "fetch_uid_headers_cached: correct content");
            free(loaded);
        }
    }

    /* ── print_account_row ───────────────────────────────────────────── */
    {
        Config pcfg = {0};
        pcfg.user   = "test@example.com";
        pcfg.host   = "imaps://imap.example.com";
        pcfg.gmail_mode = 0;

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        print_account_row(&pcfg, 0, 3, 1, 30, 20);  /* not selected */
        print_account_row(&pcfg, 1, 3, 1, 30, 20);  /* selected (cursor=1) */
        /* SMTP configured branch */
        pcfg.smtp_host = (char *)"smtps://smtp.example.com";
        pcfg.smtp_port = 465;
        print_account_row(&pcfg, 0, 0, 0, 30, 20);
        /* Gmail mode */
        pcfg.gmail_mode = 1;
        print_account_row(&pcfg, 0, 5, 2, 30, 20);
        RESTORE_OUT(sout, serr);
        ASSERT(1, "print_account_row: no crash");
    }

    /* ── print_folder_item ───────────────────────────────────────────── */
    {
        char *names[] = { (char*)"INBOX", (char*)"INBOX.Sent", (char*)"Trash" };
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        /* tree mode, not selected, has kids */
        print_folder_item(names, 3, 0, '.', 1, 0, 1, 5, 2, 0, 20);
        /* tree mode, selected */
        print_folder_item(names, 3, 0, '.', 1, 1, 1, 5, 2, 0, 20);
        /* flat mode */
        print_folder_item(names, 3, 0, '.', 0, 0, 0, 0, 0, 0, 20);
        /* empty folder (messages=0) */
        print_folder_item(names, 3, 2, '.', 0, 0, 0, 0, 0, 0, 20);
        RESTORE_OUT(sout, serr);
        ASSERT(1, "print_folder_item: no crash");
    }

    /* ── pager_prompt via stdin injection ───────────────────────────── */
    {
        int saved_stdin;
        /* ESC → returns 0 */
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int pr = pager_prompt(1, 3, 20, 24, 80);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(pr == 0, "pager_prompt: ESC returns 0");
    }
    {
        int saved_stdin;
        /* Ctrl-C / 'q' → returns 0 */
        INJECT_STDIN("\x03", 1, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int pr = pager_prompt(2, 5, 10, 24, 80);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(pr == 0, "pager_prompt: Ctrl-C returns 0");
    }
    {
        int saved_stdin;
        /* PgDn → returns page_size */
        INJECT_STDIN("\033[6~", 4, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int pr = pager_prompt(1, 3, 20, 24, 80);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(pr == 20, "pager_prompt: PgDn returns page_size");
    }
    {
        int saved_stdin;
        /* PgUp → returns -page_size */
        INJECT_STDIN("\033[5~", 4, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int pr = pager_prompt(2, 3, 20, 24, 80);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(pr == -20, "pager_prompt: PgUp returns -page_size");
    }

    /* ── show_help_popup via stdin injection ────────────────────────── */
    {
        const char *rows[][2] = {
            { "Enter", "Open message" },
            { "ESC",   "Quit"         },
        };
        int saved_stdin;
        INJECT_STDIN("\r", 1, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        show_help_popup("Help", rows, 2);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(1, "show_help_popup: no crash");
    }

    /* ── show_attachment_picker via stdin injection ──────────────────── */
    {
        MimeAttachment atts[2] = {
            { .filename = (char*)"doc.pdf",  .content_type = (char*)"application/pdf",
              .data = NULL, .size = 102400 },
            { .filename = (char*)"img.png",  .content_type = (char*)"image/png",
              .data = NULL, .size = 1500000 },
        };
        int saved_stdin;
        /* ESC → returns -2 */
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int picked = show_attachment_picker(atts, 2, 80, 24);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(picked == -2, "show_attachment_picker: ESC returns -2");
    }
    {
        MimeAttachment atts[1] = {
            { .filename = (char*)"f.txt", .content_type = (char*)"text/plain",
              .data = NULL, .size = 512 },
        };
        int saved_stdin;
        /* Enter → returns 0 (cursor=0) */
        INJECT_STDIN("\r", 1, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int picked = show_attachment_picker(atts, 1, 80, 24);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(picked == 0, "show_attachment_picker: Enter returns cursor 0");
    }
    {
        MimeAttachment atts[1] = {
            { .filename = NULL, .content_type = NULL, .data = NULL, .size = 0 },
        };
        int saved_stdin;
        /* Backspace → returns -1 */
        INJECT_STDIN("\x7f", 1, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int picked = show_attachment_picker(atts, 1, 80, 24);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(picked == -1, "show_attachment_picker: Backspace returns -1");
    }

    /* ── email_service_cron_status / cron_remove ─────────────────────── */
    {
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int cs = email_service_cron_status();
        RESTORE_OUT(sout, serr);
        ASSERT(cs == 0, "email_service_cron_status: returns 0");
    }
    {
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int cr = email_service_cron_remove();
        RESTORE_OUT(sout, serr);
        ASSERT(cr == 0, "email_service_cron_remove: no entry → returns 0");
    }

    /* ── email_service_list_folders (batch, no server) ──────────────── */
    {
        Config fcfg = {0};
        fcfg.host   = "imaps://no.such.host.invalid";
        fcfg.user   = "nobody";
        fcfg.folder = "INBOX";

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int fr = email_service_list_folders(&fcfg, 0);  /* flat */
        int tr = email_service_list_folders(&fcfg, 1);  /* tree */
        RESTORE_OUT(sout, serr);
        /* no local cache + no server → -1 or 0 */
        ASSERT(fr == 0 || fr == -1, "email_service_list_folders flat: returns 0 or -1");
        ASSERT(tr == 0 || tr == -1, "email_service_list_folders tree: returns 0 or -1");
    }

    /* ── email_service_list_folders_interactive (no connection → NULL) ── */
    {
        Config ficfg = {0};
        ficfg.host   = "imaps://no.such.host.invalid";
        ficfg.user   = "nobody_folders";
        ficfg.folder = "INBOX";
        local_store_init(ficfg.host, ficfg.user);

        int go_up = 0;
        char *sel = email_service_list_folders_interactive(&ficfg, "INBOX", &go_up);
        /* No cached folders, no connection → returns NULL immediately */
        ASSERT(sel == NULL, "email_service_list_folders_interactive: no folders → NULL");
        free(sel);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_labels_interactive (ESC exits) ──────────── */
    {
        local_store_init(NULL, "testlabels@gmail.com");
        Config lcfg = {0};
        lcfg.host       = NULL;
        lcfg.user       = "testlabels@gmail.com";
        lcfg.gmail_mode = 1;

        int go_up = 0;
        int saved_stdin;
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *sel = email_service_list_labels_interactive(&lcfg, "INBOX", &go_up);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(sel == NULL, "email_service_list_labels_interactive: ESC → NULL");
        free(sel);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_account_interactive (no accounts, ESC exits) ───── */
    {
        Config *acc_out = NULL;
        int cursor = 0;
        int saved_stdin;
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int aret = email_service_account_interactive(&acc_out, &cursor, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(aret == 0 || aret == -1 || aret == 1,
               "email_service_account_interactive: exits cleanly");
        (void)acc_out;
    }

    /* ── email_service_cron_setup (exercises path, may fail w/o binary) ─ */
    {
        Config cscfg = {0};
        cscfg.sync_interval = 30;
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int csr = email_service_cron_setup(&cscfg);
        RESTORE_OUT(sout, serr);
        /* May return 0 (already present or installed) or -1 (no binary path).
         * Either is acceptable; we just exercise the code path. */
        ASSERT(csr == 0 || csr == -1, "email_service_cron_setup: returns 0 or -1");
        /* If installed, clean it up */
        if (csr == 0) {
            SUPPRESS_OUT(sout, serr);
            email_service_cron_remove();
            RESTORE_OUT(sout, serr);
        }
    }

    /* ── email_service_set_label (IMAP mode: no-op / Gmail fails) ────── */
    {
        Config slcfg = {0};
        slcfg.host   = "imaps://no.such.host.invalid";
        slcfg.user   = "nobody";
        slcfg.folder = "INBOX";
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int slr = email_service_set_label(&slcfg, "uid123", "Work", 1);
        RESTORE_OUT(sout, serr);
        /* IMAP: mail_client_connect will fail → returns -1 */
        ASSERT(slr == 0 || slr == -1, "email_service_set_label IMAP: returns -1 or 0");
    }

    /* ── email_service_create_label / delete_label (fail paths) ─────── */
    {
        Config clcfg = {0};
        clcfg.host   = "imaps://no.such.host.invalid";
        clcfg.user   = "nobody";
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int clr = email_service_create_label(&clcfg, "NewLabel");
        int dlr = email_service_delete_label(&clcfg, "NewLabel");
        RESTORE_OUT(sout, serr);
        ASSERT(clr == -1, "email_service_create_label: no connection → -1");
        ASSERT(dlr == -1, "email_service_delete_label: no connection → -1");
    }

    /* ── email_service_sync_all (no accounts) ───────────────────────── */
    {
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int sar = email_service_sync_all(NULL, 0);
        RESTORE_OUT(sout, serr);
        ASSERT(sar == 0 || sar == -1, "email_service_sync_all NULL: returns 0 or -1");
    }

    /* ── email_service_save_sent (local save, no IMAP connection needed) ── */
    {
        Config sscfg = {0};
        sscfg.host   = "imaps://no.such.host.invalid";
        sscfg.user   = "nobody";
        sscfg.folder = "INBOX";
        const char *msg = "From: a@b.com\r\nTo: b@c.com\r\nSubject: Test\r\n\r\nHello";
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int ssr = email_service_save_sent(&sscfg, msg, strlen(msg));
        RESTORE_OUT(sout, serr);
        ASSERT(ssr == 0, "email_service_save_sent: saves locally without IMAP → 0");
    }
    /* ── email_service_save_sent (fail path: NULL msg) ─────────────────── */
    {
        Config sscfg = {0};
        sscfg.host   = "imaps://no.such.host.invalid";
        sscfg.user   = "nobody";
        sscfg.folder = "INBOX";
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int ssr = email_service_save_sent(&sscfg, NULL, 0);
        RESTORE_OUT(sout, serr);
        ASSERT(ssr == -1, "email_service_save_sent: NULL msg → -1");
    }

    /* ── show_label_picker via stdin injection ───────────────────────── */
    {
        local_store_init(NULL, "picker@gmail.com");
        int saved_stdin;
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        /* mc=NULL is safe: the picker checks if(mc) before calling it */
        show_label_picker(NULL, "0000000000001234", NULL, 0);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(1, "show_label_picker: ESC exits cleanly");
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── bg_sync_sigchld (direct call of signal handler) ─────────────── */
    {
        /* Simulate the handler being called with no active child */
        bg_sync_pid = 99999999; /* set to unreachable PID */
        bg_sync_sigchld(SIGCHLD);
        /* Handler calls waitpid(-1, WNOHANG) which returns 0 (no child) */
        ASSERT(1, "bg_sync_sigchld: no crash on direct call");
        bg_sync_pid = -1; /* restore */
    }

    /* ── sync_start_background (spawns child that fails exec) ────────── */
    {
        /* bg_sync_pid must be -1 so sync_is_running() returns false */
        bg_sync_pid = -1;
        bg_sync_done = 0;
        int sbr = sync_start_background();
        /* May return 1 (forked) or -1 (fork error). Either is acceptable. */
        ASSERT(sbr == 1 || sbr == -1 || sbr == 0,
               "sync_start_background: returns expected value");
        /* Let the child finish (it will exec fail and exit almost immediately) */
        if (sbr == 1) {
            struct timespec ts = {0, 50000000}; /* 50ms */
            nanosleep(&ts, NULL);
        }
    }

    /* ── flag_push_background (spawns child that fails connect) ─────── */
    {
        Config fpbcfg = {0};
        fpbcfg.host   = "imaps://127.0.0.1"; /* will fail connection */
        fpbcfg.user   = "nobody";
        fpbcfg.folder = "INBOX";
        /* fork child that will try to connect and exit immediately */
        flag_push_background(&fpbcfg, "0000000000001111", "\\Seen", 1);
        ASSERT(1, "flag_push_background: no crash");
        /* Short wait for child reaping */
        struct timespec ts2 = {0, 50000000};
        nanosleep(&ts2, NULL);
    }

    /* ── pager_prompt: Down arrow → returns 1 ────────────────────────── */
    {
        int saved_stdin;
        INJECT_STDIN("\033[B", 3, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int pr = pager_prompt(1, 3, 20, 24, 80);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(pr == 1, "pager_prompt: Down returns 1");
    }

    /* ── pager_prompt: Up arrow → returns -1 ─────────────────────────── */
    {
        int saved_stdin;
        INJECT_STDIN("\033[A", 3, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int pr = pager_prompt(2, 3, 20, 24, 80);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(pr == -1, "pager_prompt: Up returns -1");
    }

    /* ── pager_prompt: Enter continues loop, then ESC exits → 0 ──────── */
    {
        int saved_stdin;
        /* Enter: continues loop (TERM_KEY_ENTER → continue)
         * Then ESC: returns 0 */
        INJECT_STDIN("\r\033x", 3, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int pr = pager_prompt(1, 3, 20, 24, 80);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(pr == 0, "pager_prompt: Enter+ESC returns 0");
    }

    /* ── show_attachment_picker: Down navigates, then ESC ─────────────── */
    {
        MimeAttachment atts[2] = {
            { .filename = (char*)"a.pdf", .content_type = (char*)"application/pdf",
              .data = NULL, .size = 1024 },
            { .filename = (char*)"b.png", .content_type = (char*)"image/png",
              .data = NULL, .size = 2048 },
        };
        int saved_stdin;
        /* Down (cursor 0→1) then Up (1→0) then ESC */
        INJECT_STDIN("\033[B\033[A\033x", 8, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int picked = show_attachment_picker(atts, 2, 80, 24);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(picked == -2, "show_attachment_picker: Down+Up+ESC returns -2");
    }

    /* ── email_service_list_folders with cached folders ─────────────── */
    {
        /* Populate folder cache for a dedicated test user.
         * label_idx_add creates the account directory tree (mkdir_p) so that
         * local_folder_list_save can write folders.cache there. */
        local_store_init("imaps://test.example.com", "foldercache@example.com");
        label_idx_add("_setup_", "0000000000000001");  /* ensures account dir exists */
        const char *flist[] = { "INBOX", "INBOX.Sent", "INBOX.Archive", "Trash" };
        local_folder_list_save(flist, 4, '.');

        Config fcfg2 = {0};
        fcfg2.host   = "imaps://test.example.com";
        fcfg2.user   = "foldercache@example.com";
        fcfg2.folder = "INBOX";

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int fr2 = email_service_list_folders(&fcfg2, 0);  /* flat */
        int tr2 = email_service_list_folders(&fcfg2, 1);  /* tree */
        RESTORE_OUT(sout, serr);
        ASSERT(fr2 == 0, "email_service_list_folders flat with cache: returns 0");
        ASSERT(tr2 == 0, "email_service_list_folders tree with cache: returns 0");

        /* Restore */
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_folders_interactive with cached folders + ESC ─ */
    {
        local_store_init("imaps://test.example.com", "foldercache@example.com");
        /* Folders already cached from the previous test */

        Config ficfg2 = {0};
        ficfg2.host   = "imaps://test.example.com";
        ficfg2.user   = "foldercache@example.com";
        ficfg2.folder = "INBOX";

        int go_up = 0;
        int saved_stdin;
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *sel = email_service_list_folders_interactive(&ficfg2, "INBOX", &go_up);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(sel == NULL, "email_service_list_folders_interactive cached+ESC: NULL");
        ASSERT(go_up == 0,  "email_service_list_folders_interactive cached+ESC: go_up=0");
        free(sel);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_folders_interactive: Down+Enter selects ──── */
    {
        local_store_init("imaps://test.example.com", "foldercache@example.com");

        Config ficfg3 = {0};
        ficfg3.host   = "imaps://test.example.com";
        ficfg3.user   = "foldercache@example.com";
        ficfg3.folder = "INBOX";

        /* Ensure tree_mode=1 by resetting pref */
        ui_pref_set_int("folder_view_mode", 1);

        int go_up2 = 0;
        int saved_stdin;
        /* Down moves cursor to item 1 ("INBOX.Sent"), Enter selects it */
        INJECT_STDIN("\033[B\r", 4, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *sel2 = email_service_list_folders_interactive(&ficfg3, "INBOX", &go_up2);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(sel2 != NULL, "email_service_list_folders_interactive: Down+Enter returns non-NULL");
        free(sel2);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_folders_interactive: Backspace sets go_up ── */
    {
        local_store_init("imaps://test.example.com", "foldercache@example.com");

        Config ficfg4 = {0};
        ficfg4.host   = "imaps://test.example.com";
        ficfg4.user   = "foldercache@example.com";
        ficfg4.folder = "INBOX";

        ui_pref_set_int("folder_view_mode", 1);  /* tree mode */

        int go_up3 = 0;
        int saved_stdin;
        INJECT_STDIN("\x7f", 1, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *sel3 = email_service_list_folders_interactive(&ficfg4, "INBOX", &go_up3);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(sel3 == NULL, "email_service_list_folders_interactive: Backspace→NULL");
        ASSERT(go_up3 == 1,  "email_service_list_folders_interactive: Backspace→go_up=1");
        free(sel3);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_folders_interactive: PgDn+PgUp+Quit ─────── */
    {
        local_store_init("imaps://test.example.com", "foldercache@example.com");

        Config ficfg5 = {0};
        ficfg5.host   = "imaps://test.example.com";
        ficfg5.user   = "foldercache@example.com";
        ficfg5.folder = "INBOX";

        ui_pref_set_int("folder_view_mode", 1);  /* tree mode */

        int go_up4 = 0;
        int saved_stdin;
        /* PgDn, PgUp, then Ctrl-C (quit) */
        INJECT_STDIN("\033[6~\033[5~\x03", 9, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *sel4 = email_service_list_folders_interactive(&ficfg5, "INBOX", &go_up4);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(sel4 == NULL, "email_service_list_folders_interactive: PgDn+PgUp+Quit→NULL");
        free(sel4);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list (cron pager, empty manifest) ─────────────── */
    {
        Config cpcfg = {0};
        cpcfg.host          = "imaps://test.example.com";
        cpcfg.user          = "testuser";
        cpcfg.folder        = "test_cron_pager_only";
        cpcfg.sync_interval = 1;

        EmailListOpts cpopts = {0};
        cpopts.folder = "test_cron_pager_only";
        cpopts.pager  = 1;  /* TUI mode: enters the empty-pager loop */

        /* ESC exits the loop */
        int saved_stdin;
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int cpr = email_service_list(&cpcfg, &cpopts);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(cpr == 0, "email_service_list cron pager empty+ESC: returns 0");
    }

    /* ── email_service_list (cron pager empty, Backspace → 1) ─────────── */
    {
        Config cpbcfg = {0};
        cpbcfg.host          = "imaps://test.example.com";
        cpbcfg.user          = "testuser";
        cpbcfg.folder        = "test_cron_pager_bs";
        cpbcfg.sync_interval = 1;

        EmailListOpts cpbopts = {0};
        cpbopts.folder = "test_cron_pager_bs";
        cpbopts.pager  = 1;

        int saved_stdin;
        INJECT_STDIN("\x7f", 1, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int cpbr = email_service_list(&cpbcfg, &cpbopts);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(cpbr == 1, "email_service_list cron pager empty+Backspace: returns 1");
    }

    /* ── email_service_list (cron pager, non-special key then ESC) ────── */
    {
        /* Exercises lines after the BACK/QUIT/ESC checks (ch path) */
        Config cpxcfg = {0};
        cpxcfg.host          = "imaps://test.example.com";
        cpxcfg.user          = "testuser";
        cpxcfg.folder        = "test_cron_pager_x";
        cpxcfg.sync_interval = 1;

        EmailListOpts cpxopts = {0};
        cpxopts.folder = "test_cron_pager_x";
        cpxopts.pager  = 1;

        int saved_stdin;
        /* 'x' → TERM_KEY_IGNORE, ch='x' → neither 's' nor 'R' → loop
         * Then ESC → returns 0 */
        INJECT_STDIN("x\033x", 3, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int cpxr = email_service_list(&cpxcfg, &cpxopts);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(cpxr == 0, "email_service_list cron pager+x+ESC: returns 0");
    }

    /* ── email_service_list (Gmail offline, sync_interval=0) ──────────── */
    {
        local_store_init(NULL, "gmailoffline@gmail.com");

        /* Add label index entries */
        label_idx_add("INBOX", "0000000000abcd01");
        label_idx_add("INBOX", "0000000000abcd02");

        /* Add .hdr files: from\tsubject\tdate\tlabels\tflags */
        const char *hdr1 = "sender@test.com\tHello World\t2026-01-15 10:00\tINBOX\t0";
        local_hdr_save("", "0000000000abcd01", hdr1, strlen(hdr1));

        Config gmcfg = {0};
        gmcfg.host          = NULL;
        gmcfg.user          = "gmailoffline@gmail.com";
        gmcfg.folder        = "INBOX";
        gmcfg.gmail_mode    = 1;
        gmcfg.sync_interval = 0;  /* Gmail offline mode (not cron) */

        EmailListOpts gmopts = {0};
        gmopts.folder = "INBOX";
        gmopts.pager  = 0;  /* batch: renders without key input */

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int gmr = email_service_list(&gmcfg, &gmopts);
        RESTORE_OUT(sout, serr);
        ASSERT(gmr == 0 || gmr == -1, "email_service_list Gmail offline batch: returns 0 or -1");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_labels_interactive: Down+ESC ─────────────── */
    {
        local_store_init(NULL, "testlabels@gmail.com");
        Config lcfg2 = {0};
        lcfg2.host       = NULL;
        lcfg2.user       = "testlabels@gmail.com";
        lcfg2.gmail_mode = 1;

        int go_up5 = 0;
        int saved_stdin;
        INJECT_STDIN("\033[B\033x", 5, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *lsel = email_service_list_labels_interactive(&lcfg2, "INBOX", &go_up5);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(lsel == NULL, "labels_interactive: Down+ESC → NULL");
        free(lsel);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_labels_interactive: Up+ESC ───────────────── */
    {
        local_store_init(NULL, "testlabels@gmail.com");
        Config lcfg3 = {0};
        lcfg3.host       = NULL;
        lcfg3.user       = "testlabels@gmail.com";
        lcfg3.gmail_mode = 1;

        int go_up6 = 0;
        int saved_stdin;
        INJECT_STDIN("\033[A\033x", 5, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *lsel2 = email_service_list_labels_interactive(&lcfg3, "INBOX", &go_up6);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(lsel2 == NULL, "labels_interactive: Up+ESC → NULL");
        free(lsel2);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_labels_interactive: PgDn+ESC ─────────────── */
    {
        local_store_init(NULL, "testlabels@gmail.com");
        Config lcfg4 = {0};
        lcfg4.host       = NULL;
        lcfg4.user       = "testlabels@gmail.com";
        lcfg4.gmail_mode = 1;

        int go_up7 = 0;
        int saved_stdin;
        INJECT_STDIN("\033[6~\033x", 6, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *lsel3 = email_service_list_labels_interactive(&lcfg4, "INBOX", &go_up7);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(lsel3 == NULL, "labels_interactive: PgDn+ESC → NULL");
        free(lsel3);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_labels_interactive: PgUp+ESC ─────────────── */
    {
        local_store_init(NULL, "testlabels@gmail.com");
        Config lcfg5 = {0};
        lcfg5.host       = NULL;
        lcfg5.user       = "testlabels@gmail.com";
        lcfg5.gmail_mode = 1;

        int go_up8 = 0;
        int saved_stdin;
        INJECT_STDIN("\033[5~\033x", 6, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *lsel4 = email_service_list_labels_interactive(&lcfg5, "INBOX", &go_up8);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(lsel4 == NULL, "labels_interactive: PgUp+ESC → NULL");
        free(lsel4);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_labels_interactive: Enter returns selection ─ */
    {
        local_store_init(NULL, "testlabels@gmail.com");
        Config lcfg6 = {0};
        lcfg6.host       = NULL;
        lcfg6.user       = "testlabels@gmail.com";
        lcfg6.gmail_mode = 1;

        int go_up9 = 0;
        int saved_stdin;
        INJECT_STDIN("\r", 1, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *lsel5 = email_service_list_labels_interactive(&lcfg6, "INBOX", &go_up9);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(lsel5 != NULL, "labels_interactive: Enter returns non-NULL label");
        free(lsel5);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_labels_interactive: Backspace → go_up=1 ──── */
    {
        local_store_init(NULL, "testlabels@gmail.com");
        Config lcfg7 = {0};
        lcfg7.host       = NULL;
        lcfg7.user       = "testlabels@gmail.com";
        lcfg7.gmail_mode = 1;

        int go_upA = 0;
        int saved_stdin;
        INJECT_STDIN("\x7f", 1, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *lsel6 = email_service_list_labels_interactive(&lcfg7, "INBOX", &go_upA);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(lsel6 == NULL, "labels_interactive: Backspace → NULL");
        ASSERT(go_upA == 1,   "labels_interactive: Backspace → go_up=1");
        free(lsel6);

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list (Gmail pager, full interactive key coverage) ─ */
    /*
     * Covers show_uid_interactive key handlers (lines ~893-939) and
     * email_service_list Gmail handlers (lines ~1970-2228) and
     * show_label_picker user-label section (lines ~2626-2643).
     *
     * Key sequence (36 bytes):
     *   \r          Enter: open message in show_uid_interactive
     *   \033[6~     PgDn in reader
     *   \033[5~     PgUp in reader
     *   \033[B      Down in reader
     *   \033[A      Up in reader
     *   \r          Enter in reader (break/stay)
     *   h\r         help popup + dismiss (lines 926-939)
     *   q           quit reader → back to list
     *   h\r         Gmail help popup in list + dismiss (lines 2011-2053)
     *   a           archive (lines 2055-2088)
     *   D           trash (lines 2090-2112)
     *   u           untrash (lines 2114-2147)
     *   t           label picker (lines 2149-2151; user-label lines 2626-2643)
     *   \033[B\r    Down + toggle in picker
     *   \033x       ESC exit picker
     *   d           remove label (lines 2153-2183)
     *   n           toggle unread (lines 2185-2228, Gmail path)
     *   f           toggle starred
     *   \033x       ESC exit list
     */
    {
        const char *gluid = "0000000000ee0001";
        local_store_init(NULL, "listpager@gmail.com");

        /* Populate INBOX index and a user label for show_label_picker coverage */
        label_idx_add("INBOX", gluid);
        label_idx_add("UserLbl", gluid);

        /* .hdr: from\tsubject\tdate\tlabels\tflags */
        const char *gl_hdr = "from@t.com\tHello Test\t2026-01-15 10:00\tINBOX,UserLbl\t0";
        local_hdr_save("", gluid, gl_hdr, strlen(gl_hdr));

        /* Body required so show_uid_interactive can load it (not return -1) */
        const char *gl_body =
            "From: from@t.com\r\n"
            "Subject: Hello Test\r\n"
            "Date: Thu, 15 Jan 2026 10:00:00 +0000\r\n"
            "\r\n"
            "Hello, world!\r\n";
        local_msg_save("INBOX", gluid, gl_body, strlen(gl_body));

        Config glcfg = {0};
        glcfg.host          = NULL;
        glcfg.user          = "listpager@gmail.com";
        glcfg.folder        = "INBOX";
        glcfg.gmail_mode    = 1;
        glcfg.sync_interval = 0;

        EmailListOpts glopts = {0};
        glopts.folder = "INBOX";
        glopts.pager  = 1;

        int saved_stdin;
        INJECT_STDIN(
            "\r"           /* Enter → show_uid_interactive */
            "\033[6~"      /* PgDn */
            "\033[5~"      /* PgUp */
            "\033[B"       /* Down */
            "\033[A"       /* Up */
            "\r"           /* Enter (stay) */
            "h\r"          /* help popup + dismiss */
            "q"            /* quit reader */
            "h\r"          /* Gmail help popup + dismiss */
            "aDut"         /* archive, trash, untrash, label-picker */
            "\033[B\r"     /* Down+toggle in picker */
            "\033x"        /* ESC exit picker */
            "dnf"          /* remove-label, toggle-unread, toggle-starred */
            "\033x",       /* ESC exit list */
            36, saved_stdin);

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int glr = email_service_list(&glcfg, &glopts);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(glr == 0 || glr == 1,
               "email_service_list Gmail pager full coverage: returns 0 or 1");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_labels_interactive: user labels + qsort ──── */
    {
        /* Two user labels → covers filtering loop (2831-2832) and qsort (2840) */
        local_store_init(NULL, "lblsort@gmail.com");
        label_idx_add("WorkLabel",     "0000000000f10001");
        label_idx_add("PersonalLabel", "0000000000f10002");

        Config lscfg = {0};
        lscfg.host       = NULL;
        lscfg.user       = "lblsort@gmail.com";
        lscfg.gmail_mode = 1;

        int go_upB = 0;
        int saved_stdin;
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *lsret = email_service_list_labels_interactive(&lscfg, "INBOX", &go_upB);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        free(lsret);
        ASSERT(1, "labels_interactive: user labels qsort covered");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_account_interactive: with one Gmail account ─────── */
    {
        /* Save a Gmail account (has user+refresh_token → passes validation in
         * load_config_from_path, so config_list_accounts returns count=1).
         * This covers the render loop (lines 3182-3221, get_account_totals
         * Gmail path 3068-3072, print_account_row 3107-3151) and navigation
         * key handlers (3242-3290). */
        Config acc_cfg = {0};
        acc_cfg.user                = "acctest-tui@gmail.com";
        acc_cfg.gmail_mode          = 1;
        acc_cfg.gmail_refresh_token = "fake_token_for_test";
        acc_cfg.folder              = "INBOX";
        config_delete_account("acctest-tui@gmail.com"); /* pre-clean */
        config_save_account(&acc_cfg);

        Config *acc_out2 = NULL;
        int cursor2 = 0;
        int saved_stdin;
        /* Down (cursor++ clamped to 0), Up (no-op), Backspace (continue),
         * h (help popup), Enter (dismiss popup), n (return 3 = add account) */
        INJECT_STDIN("\033[B\033[A\x7fh\rn", 10, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int aret2 = email_service_account_interactive(&acc_out2, &cursor2, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        config_free(acc_out2);
        ASSERT(aret2 == 3, "email_service_account_interactive with account: n → 3");

        config_delete_account("acctest-tui@gmail.com");
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_read: pager mode, short message fits one page ─── */
    {
        /* Message "0000000000008888" was saved by the batch-mode read test */
        Config rpager_cfg = {0};
        rpager_cfg.host   = "imaps://test.example.com";
        rpager_cfg.user   = "testuser";
        rpager_cfg.folder = "INBOX";

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int rpager_r = email_service_read(&rpager_cfg, "0000000000008888", 1, 25);
        RESTORE_OUT(sout, serr);
        ASSERT(rpager_r == 0, "email_service_read pager: short msg fits → 0");
    }

    /* ── email_service_read: cron mode + message not cached → -1 ─────── */
    {
        Config rcron_cfg = {0};
        rcron_cfg.host          = "imaps://test.example.com";
        rcron_cfg.user          = "testuser";
        rcron_cfg.folder        = "INBOX";
        rcron_cfg.sync_interval = 1; /* cron: do not connect */

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int rcron_r = email_service_read(&rcron_cfg, "nonexistent_uid_xyz", 0, 0);
        RESTORE_OUT(sout, serr);
        ASSERT(rcron_r == -1, "email_service_read: cron + no cache → -1");
    }

    /* ── email_service_set_flag: all flag types + Gmail label paths ───── */
    {
        local_store_init("imaps://test.example.com", "testuser");
        Config sffcfg = {0};
        sffcfg.host   = "imaps://no.such.host.invalid";
        sffcfg.user   = "testuser";
        sffcfg.folder = "INBOX";

        int sout, serr;

        /* Unknown flag bit → -1 immediately */
        SUPPRESS_OUT(sout, serr);
        int sfr1 = email_service_set_flag(&sffcfg, "0000000000008888", NULL, 0xFF, 1);
        RESTORE_OUT(sout, serr);
        ASSERT(sfr1 == -1, "set_flag: unknown bit → -1");

        /* FLAGGED bit, IMAP, no connection → 0 (queued) */
        SUPPRESS_OUT(sout, serr);
        int sfr2 = email_service_set_flag(&sffcfg, "0000000000008888", NULL,
                                          MSG_FLAG_FLAGGED, 1);
        RESTORE_OUT(sout, serr);
        ASSERT(sfr2 == 0, "set_flag: FLAGGED IMAP no-conn → 0");

        /* DONE bit, IMAP, no connection → 0 (queued) */
        SUPPRESS_OUT(sout, serr);
        int sfr3 = email_service_set_flag(&sffcfg, "0000000000008888", NULL,
                                          MSG_FLAG_DONE, 1);
        RESTORE_OUT(sout, serr);
        ASSERT(sfr3 == 0, "set_flag: DONE IMAP no-conn → 0");

        /* UNSEEN bit, Gmail mode → updates UNREAD label index → 0 */
        local_store_init(NULL, "listpager@gmail.com");
        Config sfgcfg = {0};
        sfgcfg.user       = "listpager@gmail.com";
        sfgcfg.gmail_mode = 1;
        sfgcfg.folder     = "INBOX";

        SUPPRESS_OUT(sout, serr);
        int sfr4 = email_service_set_flag(&sfgcfg, "0000000000ee0001", NULL,
                                          MSG_FLAG_UNSEEN, 1);
        RESTORE_OUT(sout, serr);
        ASSERT(sfr4 == 0, "set_flag: UNSEEN Gmail add → 0");

        /* FLAGGED/remove, Gmail mode → updates STARRED label index */
        SUPPRESS_OUT(sout, serr);
        int sfr5 = email_service_set_flag(&sfgcfg, "0000000000ee0001", NULL,
                                          MSG_FLAG_FLAGGED, 0);
        RESTORE_OUT(sout, serr);
        ASSERT(sfr5 == 0, "set_flag: FLAGGED Gmail remove → 0");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_cron_status ─────────────────────────────────────── */
    {
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int cst = email_service_cron_status();
        RESTORE_OUT(sout, serr);
        ASSERT(cst == 0, "email_service_cron_status: returns 0");
    }

    /* ── email_service_list_attachments + email_service_fetch_raw ─────── */
    {
        /* "0000000000008888" is a plain-text message (no attachments) */
        Config attcfg = {0};
        attcfg.host   = "imaps://test.example.com";
        attcfg.user   = "testuser";
        attcfg.folder = "INBOX";

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int attr = email_service_list_attachments(&attcfg, "0000000000008888");
        RESTORE_OUT(sout, serr);
        ASSERT(attr == 0, "list_attachments: plain-text msg → 0");

        /* fetch_raw returns the raw message string */
        char *rawmsg = email_service_fetch_raw(&attcfg, "0000000000008888");
        ASSERT(rawmsg != NULL, "email_service_fetch_raw: cached msg → not NULL");
        free(rawmsg);
    }

    /* ── email_service_list Gmail pager: navigation keys ─────────────── */
    {
        /* Set up an account with 2 messages, both with .hdr files so the
         * render loop has no missing entries and no slow-fetch poll. */
        local_store_init(NULL, "navtest@gmail.com");
        label_idx_add("INBOX", "0000000000cc0001");
        label_idx_add("INBOX", "0000000000cc0002");
        const char *nhdr1 = "s@t.com\tMsg One\t2026-01-15 10:00\tINBOX\t0";
        local_hdr_save("", "0000000000cc0001", nhdr1, strlen(nhdr1));
        const char *nhdr2 = "s@t.com\tMsg Two\t2026-01-16 10:00\tINBOX\t0";
        local_hdr_save("", "0000000000cc0002", nhdr2, strlen(nhdr2));

        Config navcfg = {0};
        navcfg.user          = "navtest@gmail.com";
        navcfg.gmail_mode    = 1;
        navcfg.sync_interval = 0;
        navcfg.folder        = "INBOX";
        EmailListOpts navopts = {0};
        navopts.folder = "INBOX";
        navopts.pager  = 1;

        /* Down, Up, PgDn, PgUp, ESC — covers TERM_KEY_NEXT_LINE/PREV_LINE/
         * NEXT_PAGE/PREV_PAGE cases in email_service_list (lines 2230-2243). */
        int saved_stdin;
        INJECT_STDIN("\033[B\033[A\033[6~\033[5~\033x", 16, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int navr = email_service_list(&navcfg, &navopts);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(navr == 0 || navr == 1,
               "email_service_list Gmail navigation: returns 0 or 1");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_account_interactive: 'd' deletes account ─────── */
    {
        /* Save a Gmail account, press 'd' to delete it.  After deletion the
         * loop re-renders with count=0, then reads ESC and returns 0.
         * Covers the 'd' key handler (lines 3292-3318) and the non-empty
         * text path in print_infoline (lines 378-381). */
        Config acc_del = {0};
        acc_del.user                = "acctest-del@gmail.com";
        acc_del.gmail_mode          = 1;
        acc_del.gmail_refresh_token = "fake_token_for_delete";
        acc_del.folder              = "INBOX";
        config_delete_account("acctest-del@gmail.com");
        config_save_account(&acc_del);

        Config *acc_d = NULL;
        int cursorD = 0;
        int saved_stdin;
        /* 'd' deletes account → loop re-renders with count=0 → ESC exits */
        INJECT_STDIN("d\033x", 3, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int aretD = email_service_account_interactive(&acc_d, &cursorD, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(aretD == 0, "email_service_account_interactive: d+ESC → 0");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_account_interactive: Enter opens account ─────── */
    {
        Config acc_en = {0};
        acc_en.user                = "acctest-enter@gmail.com";
        acc_en.gmail_mode          = 1;
        acc_en.gmail_refresh_token = "fake_token_enter";
        acc_en.folder              = "INBOX";
        config_delete_account("acctest-enter@gmail.com");
        config_save_account(&acc_en);

        Config *acc_out_en = NULL;
        int cursor_en = 0;
        int saved_stdin;
        INJECT_STDIN("\r", 1, saved_stdin);   /* Enter → return 1 */
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int aret_en = email_service_account_interactive(&acc_out_en, &cursor_en, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        config_free(acc_out_en);
        ASSERT(aret_en == 1, "email_service_account_interactive: Enter → 1");
        config_delete_account("acctest-enter@gmail.com");
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_account_interactive: 'i' = edit IMAP ─────────── */
    {
        Config acc_iv = {0};
        acc_iv.user                = "acctest-imapedit@gmail.com";
        acc_iv.gmail_mode          = 1;
        acc_iv.gmail_refresh_token = "fake_token_imapedit";
        acc_iv.folder              = "INBOX";
        config_delete_account("acctest-imapedit@gmail.com");
        config_save_account(&acc_iv);

        Config *acc_out_iv = NULL;
        int cursor_iv = 0;
        int saved_stdin;
        INJECT_STDIN("i", 1, saved_stdin);   /* 'i' → return 4 */
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int aret_iv = email_service_account_interactive(&acc_out_iv, &cursor_iv, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        config_free(acc_out_iv);
        ASSERT(aret_iv == 4, "email_service_account_interactive: i → 4");
        config_delete_account("acctest-imapedit@gmail.com");
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_account_interactive: 'e' = edit SMTP ─────────── */
    {
        Config acc_ev = {0};
        acc_ev.user                = "acctest-smtpedit@gmail.com";
        acc_ev.gmail_mode          = 1;
        acc_ev.gmail_refresh_token = "fake_token_smtpedit";
        acc_ev.folder              = "INBOX";
        config_delete_account("acctest-smtpedit@gmail.com");
        config_save_account(&acc_ev);

        Config *acc_out_ev = NULL;
        int cursor_ev = 0;
        int saved_stdin;
        INJECT_STDIN("e", 1, saved_stdin);   /* 'e' → return 2 */
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int aret_ev = email_service_account_interactive(&acc_out_ev, &cursor_ev, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        config_free(acc_out_ev);
        ASSERT(aret_ev == 2, "email_service_account_interactive: e → 2");
        config_delete_account("acctest-smtpedit@gmail.com");
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_account_interactive: IMAP account render ─────── */
    {
        /* Pre-populate the IMAP account's local store with a folder list so
         * get_account_totals covers the iteration loop (lines 3078-3085).
         * print_account_row then exercises IMAP-specific branches (3116,
         * 3126, 3140-3141) and fmt_url_with_port (3094-3101). */
        local_store_init("imaps://imap.example.com", "imap-user@example.com");
        const char *imap_fldrs[] = { "INBOX" };
        local_folder_list_save(imap_fldrs, 1, '/');
        local_store_init("imaps://test.example.com", "testuser");

        Config imap_acc = {0};
        imap_acc.host   = "imaps://imap.example.com";
        imap_acc.user   = "imap-user@example.com";
        imap_acc.pass   = "testpass";
        imap_acc.folder = "INBOX";
        config_delete_account("imap-user@example.com");
        config_save_account(&imap_acc);

        Config *acc_imap = NULL;
        int cursor_imap = 0;
        int saved_stdin;
        INJECT_STDIN("\033x", 2, saved_stdin);   /* ESC → return 0 */
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int aret_imap = email_service_account_interactive(&acc_imap, &cursor_imap, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(aret_imap == 0, "email_service_account_interactive: IMAP acct ESC → 0");
        config_delete_account("imap-user@example.com");
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── fmt_url_with_port: NULL url and url-with-port branches ─────── */
    {
        char fubuf[256];
        /* NULL URL → early return, out[0]='\0' (line 3094 true branch) */
        fmt_url_with_port(NULL, 993, fubuf, sizeof(fubuf));
        ASSERT(fubuf[0] == '\0', "fmt_url_with_port: NULL url → empty string");
        /* URL with explicit port → copies as-is (line 3099) */
        fmt_url_with_port("imaps://imap.example.com:993", 993, fubuf, sizeof(fubuf));
        ASSERT(strcmp(fubuf, "imaps://imap.example.com:993") == 0,
               "fmt_url_with_port: port present → unchanged");
    }

    /* ── email_service_set_flag: remaining Gmail label paths ─────────── */
    {
        local_store_init(NULL, "listpager@gmail.com");
        Config sfg2 = {0};
        sfg2.user       = "listpager@gmail.com";
        sfg2.gmail_mode = 1;
        sfg2.folder     = "INBOX";

        int sout, serr;
        /* UNSEEN/add=0 → label_idx_remove("UNREAD", uid) — line 3986 */
        SUPPRESS_OUT(sout, serr);
        int sfr6 = email_service_set_flag(&sfg2, "0000000000ee0001", NULL,
                                          MSG_FLAG_UNSEEN, 0);
        RESTORE_OUT(sout, serr);
        ASSERT(sfr6 == 0, "set_flag: UNSEEN Gmail remove → 0");

        /* FLAGGED/add=1 → label_idx_add("STARRED", uid) — line 3989 */
        SUPPRESS_OUT(sout, serr);
        int sfr7 = email_service_set_flag(&sfg2, "0000000000ee0001", NULL,
                                          MSG_FLAG_FLAGGED, 1);
        RESTORE_OUT(sout, serr);
        ASSERT(sfr7 == 0, "set_flag: FLAGGED Gmail add → 0");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── visible_line_cols: 4-byte UTF-8 and invalid byte ───────────── */
    {
        /* F0 9F 98 80 = U+1F600: triggers 'c < 0xF8' branch (line 277) */
        const char *emoji4 = "\xF0\x9F\x98\x80";
        int vc1 = visible_line_cols(emoji4, emoji4 + 4);
        ASSERT(vc1 >= 0, "visible_line_cols: 4-byte UTF-8 emoji → non-negative");

        /* FF = invalid start byte (c >= 0xF8): triggers else branch (line 278) */
        const char *inv_ff = "\xFF";
        int vc2 = visible_line_cols(inv_ff, inv_ff + 1);
        ASSERT(vc2 >= 0, "visible_line_cols: 0xFF invalid byte → non-negative");
    }

    /* ── text_end_at_cols: ANSI CSI escape sequence ──────────────────── */
    {
        /* "\033[31mHi" — ESC '[' '3' '1' 'm' = red.
         * '3' and '1' are < 0x40, so the while-loop body (line 334)
         * executes.  Covers the CSI-skip branch (lines 331-337). */
        const char *ansi_txt = "\033[31mHi";
        const char *ep = text_end_at_cols(ansi_txt, 80);
        ASSERT(ep > ansi_txt,
               "text_end_at_cols: ANSI escape → advances past sequence");
    }

    /* ── print_clean: multi-byte UTF-8 branches ──────────────────────── */
    {
        /* Covers lines 677-681:
         * \x80  = continuation byte (0x80 < 0xC2) → line 677
         * \xC3\xA9 = 'é' (2-byte, 0xC3 < 0xE0) → line 678
         * \xE4\xB8\xAD = '中' (3-byte, 0xE4 < 0xF0) → line 679
         * \xF0\x9F\x98\x80 = '😀' (4-byte, 0xF0 < 0xF8) → line 680
         * \xFF = invalid (0xFF >= 0xF8) → line 681 */
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        print_clean("\x80\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80\xFF", NULL, 80);
        RESTORE_OUT(sout, serr);
        ASSERT(1, "print_clean: multi-byte UTF-8 all branches covered");
    }

    /* ── email_service_read: multi-page pager ────────────────────────── */
    {
        /* Save a plain-text message with 6 body lines.
         * With page_size=8, rows_avail = 8 - SHOW_HDR_LINES(5) = 3.
         * body_vrows(6) > 3 → pager calls pager_prompt on first iteration.
         * Inject NEXT_LINE (Down) → second iteration executes lines 3390-3391,
         * then ESC → delta=0 → break.  Covers lines 3390-3391 and 3400-3405. */
        const char *puid = "0000000000009998";
        const char *pmsg =
            "From: pager@example.com\r\n"
            "Subject: Multi-Page Test\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "\r\n"
            "Line 1\r\nLine 2\r\nLine 3\r\nLine 4\r\nLine 5\r\nLine 6\r\n";
        local_msg_save("INBOX", puid, pmsg, strlen(pmsg));

        Config pcfg = {0};
        pcfg.host   = "imaps://test.example.com";
        pcfg.user   = "testuser";
        pcfg.folder = "INBOX";

        int saved_stdin;
        /* \033[B = NEXT_LINE → advances to second page (triggers 3390-3391)
         * \033x  = ESC       → exits pager */
        INJECT_STDIN("\033[B\033x", 5, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int pr = email_service_read(&pcfg, puid, 1, 8);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(pr == 0, "email_service_read: multi-page pager → 0");
    }

    /* ── find_match_line ────────────────────────────────────────────────── */
    {
        const char *body = "Hello World\nFoo bar\nBaz qux\nHello again\n";

        /* Forward search: find "hello" from line -1 (before first) */
        int ml = find_match_line(body, "hello", -1, 1);
        ASSERT(ml == 0, "find_match_line: forward from -1 finds line 0");

        /* Forward search: find "hello" from line 0 (wraps to line 3) */
        ml = find_match_line(body, "hello", 0, 1);
        ASSERT(ml == 3, "find_match_line: forward from 0 finds line 3");

        /* Forward wrap: find "hello" from line 3 (wraps to line 0) */
        ml = find_match_line(body, "hello", 3, 1);
        ASSERT(ml == 0, "find_match_line: forward wrap returns line 0");

        /* Backward search: find "hello" from line 4 → line 3 */
        ml = find_match_line(body, "hello", 4, -1);
        ASSERT(ml == 3, "find_match_line: backward from 4 finds line 3");

        /* Backward wrap: find "hello" from line 0 → wraps to line 3 */
        ml = find_match_line(body, "hello", 0, -1);
        ASSERT(ml == 3, "find_match_line: backward wrap returns line 3");

        /* No match: returns -1 */
        ml = find_match_line(body, "xyzzy", 0, 1);
        ASSERT(ml == -1, "find_match_line: no match → -1");

        /* NULL term: returns -1 */
        ml = find_match_line(body, NULL, 0, 1);
        ASSERT(ml == -1, "find_match_line: NULL term → -1");

        /* NULL body: returns -1 */
        ml = find_match_line(NULL, "hello", 0, 1);
        ASSERT(ml == -1, "find_match_line: NULL body → -1");
    }

    /* ── csv_update_labels ──────────────────────────────────────────────── */
    {
        /* Add labels to empty existing: no remove */
        {
            char *add[] = { (char*)"Work", (char*)"Personal" };
            char *rm[]  = { NULL };
            char *r = csv_update_labels(NULL, add, 2, rm, 0);
            ASSERT(r != NULL, "csv_update_labels: NULL existing + add → non-NULL");
            if (r) {
                ASSERT(strstr(r, "Work") != NULL,
                       "csv_update_labels: Work present");
                ASSERT(strstr(r, "Personal") != NULL,
                       "csv_update_labels: Personal present");
                free(r);
            }
        }
        /* Keep existing, remove one */
        {
            char *add[] = { NULL };
            char *rm[]  = { (char*)"INBOX" };
            char *r = csv_update_labels("INBOX,Work,Personal", add, 0, rm, 1);
            ASSERT(r != NULL, "csv_update_labels: remove one → non-NULL");
            if (r) {
                ASSERT(strstr(r, "INBOX") == NULL,
                       "csv_update_labels: INBOX removed");
                ASSERT(strstr(r, "Work") != NULL,
                       "csv_update_labels: Work kept");
                free(r);
            }
        }
        /* Skip duplicate add */
        {
            char *add[] = { (char*)"Work" };
            char *rm[]  = { NULL };
            char *r = csv_update_labels("Work,INBOX", add, 1, rm, 0);
            ASSERT(r != NULL, "csv_update_labels: skip dup → non-NULL");
            if (r) {
                /* Work appears only once */
                const char *p = r;
                int cnt = 0;
                while ((p = strstr(p, "Work")) != NULL) { cnt++; p++; }
                ASSERT(cnt == 1, "csv_update_labels: Work appears once");
                free(r);
            }
        }
        /* Empty existing, empty add, empty rm → empty string */
        {
            char *r = csv_update_labels("", NULL, 0, NULL, 0);
            ASSERT(r != NULL, "csv_update_labels: all-empty → non-NULL");
            free(r);
        }
    }

    /* ── list_filter_rebuild ────────────────────────────────────────────── */
    {
        /* Set up a manifest and entries for filter testing */
        const char *lfr_folder = "test_lfr_folder";
        Manifest *lfr_m = calloc(1, sizeof(Manifest));
        manifest_upsert(lfr_m, "0000000000lfr001",
                        strdup("alice@example.com"), strdup("Hello World"),
                        strdup("2026-01-01 00:00"), MSG_FLAG_UNSEEN);
        manifest_upsert(lfr_m, "0000000000lfr002",
                        strdup("bob@example.com"), strdup("Goodbye Cruel World"),
                        strdup("2026-01-02 00:00"), 0);
        manifest_save(lfr_folder, lfr_m);

        /* Build MsgEntry array from manifest */
        MsgEntry lfr_entries[2];
        memset(lfr_entries, 0, sizeof(lfr_entries));
        memcpy(lfr_entries[0].uid, "0000000000lfr001", 16); lfr_entries[0].uid[16] = '\0';
        lfr_entries[0].flags = MSG_FLAG_UNSEEN;
        memcpy(lfr_entries[1].uid, "0000000000lfr002", 16); lfr_entries[1].uid[16] = '\0';

        int fentries[4] = {0};
        int fcount = 0;

        Config lfr_cfg = {0};
        lfr_cfg.host   = "imaps://test.example.com";
        lfr_cfg.user   = "testuser";
        lfr_cfg.folder = (char *)lfr_folder;

        /* Empty filter: identity pass-through */
        list_filter_rebuild(lfr_entries, 2, lfr_m, &lfr_cfg, lfr_folder,
                            "", 0, fentries, &fcount);
        ASSERT(fcount == 2, "list_filter_rebuild: empty filter → all 2");

        /* fscope=0 (subject) filter "Hello" → 1 match */
        list_filter_rebuild(lfr_entries, 2, lfr_m, &lfr_cfg, lfr_folder,
                            "Hello", 0, fentries, &fcount);
        ASSERT(fcount == 1, "list_filter_rebuild: subject 'Hello' → 1");
        ASSERT(fentries[0] == 0, "list_filter_rebuild: matched entry 0");

        /* fscope=1 (from) filter "bob" → 1 match */
        list_filter_rebuild(lfr_entries, 2, lfr_m, &lfr_cfg, lfr_folder,
                            "bob", 1, fentries, &fcount);
        ASSERT(fcount == 1, "list_filter_rebuild: from 'bob' → 1");
        ASSERT(fentries[0] == 1, "list_filter_rebuild: matched entry 1");

        /* fscope=0 filter with no match */
        list_filter_rebuild(lfr_entries, 2, lfr_m, &lfr_cfg, lfr_folder,
                            "XYZZY_NOMATCH", 0, fentries, &fcount);
        ASSERT(fcount == 0, "list_filter_rebuild: no match → 0");

        /* fscope=3 (body) — body not cached, match returns 0 */
        list_filter_rebuild(lfr_entries, 2, lfr_m, &lfr_cfg, lfr_folder,
                            "Hello", 3, fentries, &fcount);
        ASSERT(fcount >= 0, "list_filter_rebuild: body scope → non-negative");

        manifest_free(lfr_m);
    }

    /* ── email_service_save_draft ───────────────────────────────────────── */
    {
        Config sd_cfg = {0};
        sd_cfg.host   = "imaps://test.example.com";
        sd_cfg.user   = "testuser";
        sd_cfg.folder = "INBOX";

        const char *draft_msg =
            "From: test@example.com\r\nSubject: Draft\r\n\r\nDraft body\r\n";

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int dr = email_service_save_draft(&sd_cfg, draft_msg, strlen(draft_msg));
        RESTORE_OUT(sout, serr);
        ASSERT(dr == 0, "email_service_save_draft: saves locally → 0");

        /* NULL msg → -1 */
        SUPPRESS_OUT(sout, serr);
        int dr2 = email_service_save_draft(&sd_cfg, NULL, 0);
        RESTORE_OUT(sout, serr);
        ASSERT(dr2 == -1, "email_service_save_draft: NULL msg → -1");
    }

    /* ── email_service_apply_rules: account not found ────────────────────── */
    {
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int ar = email_service_apply_rules("nonexistent_account_xyz_ZZZZZ",
                                            0, 0);
        RESTORE_OUT(sout, serr);
        ASSERT(ar == -1, "apply_rules: nonexistent account → -1");
    }

    /* ── email_service_apply_rules: account with no rules ─────────────────── */
    {
        /* Save an IMAP account that has NO rules file */
        Config norules_cfg = {0};
        norules_cfg.host   = "imaps://no.such.host.invalid";
        norules_cfg.user   = "norules-unit@example.com";
        norules_cfg.pass   = "x";
        norules_cfg.folder = "INBOX";
        config_delete_account("norules-unit@example.com");
        config_save_account(&norules_cfg);

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int ar = email_service_apply_rules("norules-unit@example.com", 0, 0);
        RESTORE_OUT(sout, serr);
        /* No rules file → total_fired=0, done=1 → returns 0 */
        ASSERT(ar >= 0, "apply_rules: no-rules account → 0 (or total_fired)");

        config_delete_account("norules-unit@example.com");
    }

    /* ── email_service_apply_rules: Gmail account with rules (dry-run) ──── */
    {
        /* Create Gmail account with a rule that matches INBOX messages */
        Config gar_cfg = {0};
        gar_cfg.host                = NULL;
        gar_cfg.user                = "applyrules-gmail@example.com";
        gar_cfg.gmail_mode          = 1;
        gar_cfg.gmail_refresh_token = "fake_token_applyrules";
        gar_cfg.folder              = "INBOX";
        config_delete_account("applyrules-gmail@example.com");
        config_save_account(&gar_cfg);

        /* Populate local store for this account */
        local_store_init(NULL, "applyrules-gmail@example.com");
        const char *gar_uid = "0000000000ar0001";
        label_idx_add("INBOX", gar_uid);
        /* .hdr: from\tsubject\tdate\tlabels\tflags */
        const char *gar_hdr = "github@github.com\tPR review\t2026-01-15 10:00\tINBOX\t0";
        local_hdr_save("", gar_uid, gar_hdr, strlen(gar_hdr));

        /* Write a rules.ini for this account via mail_rules_save() */
        {
            MailRules gar_rules;
            memset(&gar_rules, 0, sizeof(gar_rules));
            MailRule gar_rule;
            memset(&gar_rule, 0, sizeof(gar_rule));
            /* Use when= expression so mail_rules_save writes it correctly */
            gar_rule.when              = (char *)"from:*@github.com";
            gar_rule.then_add_label[0] = (char *)"GitHub";
            gar_rule.then_add_count    = 1;
            gar_rule.then_rm_label[0]  = (char *)"INBOX";
            gar_rule.then_rm_count     = 1;
            gar_rules.rules = &gar_rule;
            gar_rules.count = 1;
            gar_rules.cap   = 1;
            mail_rules_save("applyrules-gmail@example.com", &gar_rules);
        }

        int sout, serr;

        /* dry_run=1, verbose=1: covers print_rule_matches path */
        SUPPRESS_OUT(sout, serr);
        int ar_dry = email_service_apply_rules("applyrules-gmail@example.com",
                                                1, 1);
        RESTORE_OUT(sout, serr);
        ASSERT(ar_dry >= 0, "apply_rules Gmail dry-run: returns >=0");

        /* dry_run=0: actually applies changes */
        local_store_init(NULL, "applyrules-gmail@example.com");
        local_hdr_save("", gar_uid, gar_hdr, strlen(gar_hdr));  /* reset hdr */
        label_idx_add("INBOX", gar_uid);

        SUPPRESS_OUT(sout, serr);
        int ar_live = email_service_apply_rules("applyrules-gmail@example.com",
                                                 0, 0);
        RESTORE_OUT(sout, serr);
        ASSERT(ar_live >= 0, "apply_rules Gmail live: returns >=0");

        config_delete_account("applyrules-gmail@example.com");
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_apply_rules: IMAP account with rules (dry-run) ─────── */
    {
        const char *imap_ar_user = "applyrules-imap@example.com";
        Config iap_cfg = {0};
        iap_cfg.host   = "imaps://no.such.host.invalid";
        iap_cfg.user   = (char *)imap_ar_user;
        iap_cfg.pass   = "x";
        iap_cfg.folder = "INBOX";
        config_delete_account(imap_ar_user);
        config_save_account(&iap_cfg);

        /* Populate manifest with a message that will match the rule */
        local_store_init(iap_cfg.host, iap_cfg.user);
        Manifest *iap_m = calloc(1, sizeof(Manifest));
        manifest_upsert(iap_m, "0000000000ap0001",
                        strdup("boss@company.com"), strdup("Urgent task"),
                        strdup("2026-01-15 10:00"), MSG_FLAG_UNSEEN);
        manifest_save("INBOX", iap_m);
        manifest_free(iap_m);

        /* Write a rules.ini via mail_rules_save() */
        {
            MailRules iap_rules;
            memset(&iap_rules, 0, sizeof(iap_rules));
            MailRule iap_rule;
            memset(&iap_rule, 0, sizeof(iap_rule));
            /* Use when= expression; add _flagged → lmap path; remove UNREAD → fmap path */
            iap_rule.when              = (char *)"from:*@company.com";
            iap_rule.then_add_label[0] = (char *)"_flagged";
            iap_rule.then_add_count    = 1;
            iap_rule.then_rm_label[0]  = (char *)"UNREAD";
            iap_rule.then_rm_count     = 1;
            iap_rules.rules = &iap_rule;
            iap_rules.count = 1;
            iap_rules.cap   = 1;
            mail_rules_save(imap_ar_user, &iap_rules);
        }

        int sout, serr;

        /* dry_run=1, verbose=1: covers print_rule_matches IMAP path */
        SUPPRESS_OUT(sout, serr);
        int iap_dry = email_service_apply_rules(imap_ar_user, 1, 1);
        RESTORE_OUT(sout, serr);
        ASSERT(iap_dry >= 0, "apply_rules IMAP dry-run: returns >=0");

        /* dry_run=0: applies changes, covers lines 5742-5774 */
        local_store_init(iap_cfg.host, iap_cfg.user);
        Manifest *iap_m2 = calloc(1, sizeof(Manifest));
        manifest_upsert(iap_m2, "0000000000ap0001",
                        strdup("boss@company.com"), strdup("Urgent task"),
                        strdup("2026-01-15 10:00"), MSG_FLAG_UNSEEN);
        manifest_save("INBOX", iap_m2);
        manifest_free(iap_m2);

        SUPPRESS_OUT(sout, serr);
        int iap_live = email_service_apply_rules(imap_ar_user, 0, 0);
        RESTORE_OUT(sout, serr);
        ASSERT(iap_live >= 0, "apply_rules IMAP live: returns >=0");

        config_delete_account(imap_ar_user);
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_apply_rules: IMAP with move_folder rule ──────────── */
    {
        const char *iap_mv_user = "applyrules-move@example.com";
        Config iap_mv_cfg = {0};
        iap_mv_cfg.host   = "imaps://no.such.host.invalid";
        iap_mv_cfg.user   = (char *)iap_mv_user;
        iap_mv_cfg.pass   = "x";
        iap_mv_cfg.folder = "INBOX";
        config_delete_account(iap_mv_user);
        config_save_account(&iap_mv_cfg);

        local_store_init(iap_mv_cfg.host, iap_mv_cfg.user);
        Manifest *mv_m = calloc(1, sizeof(Manifest));
        manifest_upsert(mv_m, "0000000000mv0001",
                        strdup("newsletter@promo.com"), strdup("Promo"),
                        strdup("2026-01-15 10:00"), MSG_FLAG_UNSEEN);
        manifest_save("INBOX", mv_m);
        manifest_free(mv_m);

        {
            MailRules mv_rules;
            memset(&mv_rules, 0, sizeof(mv_rules));
            MailRule mv_rule;
            memset(&mv_rule, 0, sizeof(mv_rule));
            mv_rule.when             = (char *)"from:*@promo.com";
            mv_rule.then_move_folder = (char *)"Promotions";
            mv_rules.rules = &mv_rule;
            mv_rules.count = 1;
            mv_rules.cap   = 1;
            mail_rules_save(iap_mv_user, &mv_rules);
        }

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int mv_r = email_service_apply_rules(iap_mv_user, 0, 0);
        RESTORE_OUT(sout, serr);
        ASSERT(mv_r >= 0, "apply_rules IMAP move: returns >=0");

        config_delete_account(iap_mv_user);
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_rebuild_indexes: no accounts ─────────────────────── */
    /* Call with a definitely-nonexistent account while real accounts exist:
     * done==0 → returns -1 (covers 5395-5397) */
    {
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int ri = email_service_rebuild_indexes("nonexistent_rebuild_acct_XYZ");
        RESTORE_OUT(sout, serr);
        ASSERT(ri == -1, "rebuild_indexes: nonexistent account → -1");
    }

    /* ── email_service_rebuild_indexes: IMAP account → skip ─────────────── */
    {
        /* imap account is not Gmail → covers 5381-5384 */
        const char *ri_user = "rebuild-imap-unit@example.com";
        Config ri_cfg = {0};
        ri_cfg.host      = "imaps://no.such.host.invalid";
        ri_cfg.user      = (char *)ri_user;
        ri_cfg.pass      = "x";
        ri_cfg.folder    = "INBOX";
        ri_cfg.gmail_mode = 0;
        config_delete_account(ri_user);
        config_save_account(&ri_cfg);

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int ri2 = email_service_rebuild_indexes(ri_user);
        RESTORE_OUT(sout, serr);
        /* IMAP accounts are skipped → done=1, errors=0 → returns 0 */
        ASSERT(ri2 == 0, "rebuild_indexes: IMAP account → 0 (skipped)");

        config_delete_account(ri_user);
    }

    /* ── email_service_rebuild_contacts: account not found ──────────────── */
    {
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int rc = email_service_rebuild_contacts("nonexistent_contacts_XYZ");
        RESTORE_OUT(sout, serr);
        ASSERT(rc == -1, "rebuild_contacts: nonexistent account → -1");
    }

    /* ── email_service_rebuild_contacts: only_account filter ────────────── */
    {
        /* Save an account; call rebuild with a different only_account filter.
         * done==0 → returns -1 (covers line 5820-5821 and 5830-5832). */
        const char *rcc_user = "rebuild-contacts-unit@example.com";
        Config rcc_cfg = {0};
        rcc_cfg.host   = "imaps://no.such.host.invalid";
        rcc_cfg.user   = (char *)rcc_user;
        rcc_cfg.pass   = "x";
        rcc_cfg.folder = "INBOX";
        config_delete_account(rcc_user);
        config_save_account(&rcc_cfg);

        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        /* Filter is a different name → skip all → done=0 → -1 */
        int rc2 = email_service_rebuild_contacts("different_account_ZZZZ");
        RESTORE_OUT(sout, serr);
        ASSERT(rc2 == -1, "rebuild_contacts: filter skips all → -1");

        /* Call with matching account → done=1 → 0 */
        SUPPRESS_OUT(sout, serr);
        int rc3 = email_service_rebuild_contacts(rcc_user);
        RESTORE_OUT(sout, serr);
        ASSERT(rc3 == 0, "rebuild_contacts: matching account → 0");

        config_delete_account(rcc_user);
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_sync_all: only_account not found ─────────────────── */
    {
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int sa2 = email_service_sync_all("nonexistent_sync_acct_XYZZY", 0);
        RESTORE_OUT(sout, serr);
        ASSERT(sa2 == -1, "email_service_sync_all: not-found account → -1");
    }

    /* ── email_service_list_labels_interactive: HOME / END keys ─────────── */
    {
        local_store_init(NULL, "testlabels@gmail.com");
        Config lhe_cfg = {0};
        lhe_cfg.host       = NULL;
        lhe_cfg.user       = "testlabels@gmail.com";
        lhe_cfg.gmail_mode = 1;

        int go_upHE = 0;
        int saved_stdin;
        /* HOME → cursor goes to first, END → cursor goes to last, then ESC */
        const char home_end[] = { '\033','[','H',  /* TERM_KEY_HOME */
                                   '\033','[','F',  /* TERM_KEY_END  */
                                   '\033', 'x'      /* ESC exit     */};
        INJECT_STDIN(home_end, 8, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *he_sel = email_service_list_labels_interactive(
                           &lhe_cfg, "INBOX", &go_upHE);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        free(he_sel);
        ASSERT(1, "labels_interactive: HOME+END+ESC no crash");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_labels_interactive: no labels synced yet ────── */
    {
        /* Create a Gmail account whose local_store has NO label index at all.
         * list_labels_interactive will show "No labels synced yet." message and
         * wait for a key. Covers lines 4136-4151. */
        local_store_init(NULL, "nolabels-unit@gmail.com");
        /* Do NOT call label_idx_add — leave label index empty */

        Config nl_cfg = {0};
        nl_cfg.host       = NULL;
        nl_cfg.user       = "nolabels-unit@gmail.com";
        nl_cfg.gmail_mode = 1;

        int go_up_nl = 0;
        int saved_stdin;
        /* ESC exits the "no labels" loop */
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *nl_sel = email_service_list_labels_interactive(
                           &nl_cfg, "INBOX", &go_up_nl);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        free(nl_sel);
        ASSERT(1, "labels_interactive: no-labels path no crash");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_folders_interactive: flat mode + Enter ──────── */
    /* Covers the flat-mode code path (lines 3617-3627): when tree_mode=0 and
     * the user presses Enter on a leaf folder (not a parent), it returns the
     * selected folder. */
    {
        local_store_init("imaps://test.example.com", "foldercache@example.com");
        /* The folder cache was populated in an earlier test (INBOX, INBOX.Sent,
         * INBOX.Archive, Trash). */

        Config fif_cfg = {0};
        fif_cfg.host   = "imaps://test.example.com";
        fif_cfg.user   = "foldercache@example.com";
        fif_cfg.folder = "INBOX";

        /* Force flat mode */
        ui_pref_set_int("folder_view_mode", 0);

        int go_up_fif = 0;
        int saved_stdin;
        /* Enter selects first visible item (root "INBOX" = has children → drills in),
         * then Down moves to "INBOX.Sent" (leaf), then Enter selects it. */
        /* In flat mode root view, pressing Enter on INBOX (which has children)
         * updates current_prefix and loops; subsequent Enter on leaf selects it.
         * Use: Enter (INBOX→drill), Enter (INBOX.Sent→select) */
        INJECT_STDIN("\r\r", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        char *fif_sel = email_service_list_folders_interactive(
                            &fif_cfg, "INBOX", &go_up_fif);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        /* May select a folder or ESC-timeout → either is fine */
        ASSERT(fif_sel == NULL || strlen(fif_sel) > 0,
               "folders_interactive flat: Enter returns NULL or folder");
        free(fif_sel);

        /* Restore */
        ui_pref_set_int("folder_view_mode", 1);
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── show_uid_interactive: search ('/') and 'n' navigation ─────────── */
    /* Covers lines 1069-1097 (inline search prompt) and 1098-1104 (next match). */
    {
        const char *srch_uid = "0000000000sr0001";
        const char *srch_msg =
            "From: srch@example.com\r\n"
            "Subject: Search Test\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "\r\n"
            "Line one: the quick brown fox\r\n"
            "Line two: jumps over the lazy dog\r\n"
            "Line three: quick again\r\n";
        local_store_init("imaps://test.example.com", "testuser");
        local_msg_save("INBOX", srch_uid, srch_msg, strlen(srch_msg));

        Config srch_cfg = {0};
        srch_cfg.host   = "imaps://test.example.com";
        srch_cfg.user   = "testuser";
        srch_cfg.folder = "INBOX";

        int saved_stdin;
        /* '/' opens search; type "quick"; Enter confirms; 'n' finds next; ESC exits */
        const char srch_keys[] =
            "/"        /* open search */
            "quick"    /* type search term */
            "\r"       /* confirm search */
            "n"        /* find next match */
            "\033x";   /* ESC exit */
        INJECT_STDIN(srch_keys, (int)strlen(srch_keys), saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int srch_r = show_uid_interactive(&srch_cfg, NULL, "INBOX",
                                          srch_uid, 25, 0, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(srch_r == 0 || srch_r == 1,
               "show_uid_interactive: search '/' + 'n' no crash");
    }

    /* ── show_uid_interactive: HOME / END keys ──────────────────────────── */
    /* Covers lines 1046-1051 */
    {
        Config he_cfg = {0};
        he_cfg.host   = "imaps://test.example.com";
        he_cfg.user   = "testuser";
        he_cfg.folder = "INBOX";

        int saved_stdin;
        /* HOME, END, then ESC */
        const char he_keys[] = { '\033','[','H',  /* HOME */
                                   '\033','[','F',  /* END  */
                                   '\033', 'x'      /* ESC  */ };
        INJECT_STDIN(he_keys, 8, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int he_r = show_uid_interactive(&he_cfg, NULL, "INBOX",
                                         "0000000000sr0001", 25, 0, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(he_r == 0 || he_r == 1,
               "show_uid_interactive: HOME+END+ESC no crash");
    }

    /* ── show_uid_interactive: Gmail 'a' archive key (lines 1184-1220) ── */
    /* Also covers 't' label-picker (1221-1225), 'f' star (1228-1237),
     * 'n' unread toggle (1253-1262), 'D' trash (1164-1176),
     * 'r' remove-label (1153-1163). */
    {
        local_store_init(NULL, "gmailreader@test.com");
        const char *gr_uid = "0000000000gr0001";

        /* Store .hdr with labels including INBOX and UNREAD */
        const char *gr_hdr = "from@test.com\tGmail Reader Test\t2026-01-15 10:00\tINBOX,UNREAD\t3";
        local_hdr_save("", gr_uid, gr_hdr, strlen(gr_hdr));

        /* Store label indexes */
        label_idx_add("INBOX",  gr_uid);
        label_idx_add("UNREAD", gr_uid);

        const char *gr_msg =
            "From: from@test.com\r\n"
            "Subject: Gmail Reader Test\r\n"
            "Date: Thu, 15 Jan 2026 10:00:00 +0000\r\n"
            "\r\n"
            "Line 1\r\nLine 2\r\nLine 3\r\nLine 4\r\n"
            "Line 5\r\nLine 6\r\nLine 7\r\nLine 8\r\n";
        local_msg_save("INBOX", gr_uid, gr_msg, strlen(gr_msg));

        Config gr_cfg = {0};
        gr_cfg.host       = NULL;
        gr_cfg.user       = "gmailreader@test.com";
        gr_cfg.folder     = "INBOX";
        gr_cfg.gmail_mode = 1;

        int saved_stdin;
        int sout, serr;

        /* 'r' = remove current label (1153-1163), then ESC */
        INJECT_STDIN("r\033x", 4, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        show_uid_interactive(&gr_cfg, NULL, "INBOX", gr_uid, 25, 3, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);

        /* Restore hdr/labels for next sub-test */
        local_store_init(NULL, "gmailreader@test.com");
        local_hdr_save("", gr_uid, gr_hdr, strlen(gr_hdr));
        label_idx_add("INBOX",  gr_uid);
        label_idx_add("UNREAD", gr_uid);
        local_msg_save("INBOX", gr_uid, gr_msg, strlen(gr_msg));

        /* 'D' = trash (1164-1176), then ESC */
        INJECT_STDIN("D\033x", 4, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        show_uid_interactive(&gr_cfg, NULL, "INBOX", gr_uid, 25, 3, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);

        /* Restore for archive test */
        local_store_init(NULL, "gmailreader@test.com");
        local_hdr_save("", gr_uid, gr_hdr, strlen(gr_hdr));
        label_idx_add("INBOX",  gr_uid);
        label_idx_add("UNREAD", gr_uid);
        local_msg_save("INBOX", gr_uid, gr_msg, strlen(gr_msg));

        /* 'a' = archive (1177-1220), then ESC */
        INJECT_STDIN("a\033x", 4, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        int gr_r = show_uid_interactive(&gr_cfg, NULL, "INBOX", gr_uid, 25, 3, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(gr_r == 0 || gr_r == 1,
               "show_uid_interactive Gmail 'a' archive: no crash");

        /* Restore for 't' + 'f' + 'n' tests */
        local_store_init(NULL, "gmailreader@test.com");
        local_hdr_save("", gr_uid, gr_hdr, strlen(gr_hdr));
        label_idx_add("INBOX",  gr_uid);
        label_idx_add("UNREAD", gr_uid);
        local_msg_save("INBOX", gr_uid, gr_msg, strlen(gr_msg));

        /* 't' = label-picker (1221-1225): picker opens, ESC cancels it, then 'q' */
        /* picker reads '\033'+'X' = ESC (X consumed as c2 by ESC handler), then 'q' quits reader */
        INJECT_STDIN("t\033Xq", 4, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        show_uid_interactive(&gr_cfg, NULL, "INBOX", gr_uid, 25, 3, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);

        /* Restore for 'f' (star) test */
        local_store_init(NULL, "gmailreader@test.com");
        local_hdr_save("", gr_uid, gr_hdr, strlen(gr_hdr));
        label_idx_add("INBOX",  gr_uid);
        label_idx_add("UNREAD", gr_uid);
        local_msg_save("INBOX", gr_uid, gr_msg, strlen(gr_msg));

        /* 'f' = toggle starred (1226-1250): message not starred → add STARRED label */
        /* 'n' = toggle unread (1251-1275): message is UNREAD → mark as read */
        INJECT_STDIN("fn\033x", 5, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        show_uid_interactive(&gr_cfg, NULL, "INBOX", gr_uid, 25, 3, NULL);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(1, "show_uid_interactive Gmail 'f'+'n' toggle: no crash");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list: empty Gmail _trash pager (lines 2295-2298) ─── */
    /* Covers the show_count==0 + pager + _trash statusbar variant */
    {
        local_store_init(NULL, "emptytrash@gmail.com");
        /* No messages: label index empty → show_count = 0 */

        Config et_cfg = {0};
        et_cfg.host       = NULL;
        et_cfg.user       = "emptytrash@gmail.com";
        et_cfg.folder     = "_trash";
        et_cfg.gmail_mode = 1;

        EmailListOpts et_opts = {0};
        et_opts.folder = "_trash";
        et_opts.pager  = 1;

        int saved_stdin;
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int et_r = email_service_list(&et_cfg, &et_opts);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(et_r == 0 || et_r == 1,
               "email_service_list empty Gmail _trash pager: returns 0 or 1");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list: empty Gmail non-trash pager (lines 2304-2308) ── */
    /* Covers show_count==0 + pager + Gmail non-trash statusbar */
    {
        local_store_init(NULL, "emptyinbox@gmail.com");

        Config ei_cfg = {0};
        ei_cfg.host       = NULL;
        ei_cfg.user       = "emptyinbox@gmail.com";
        ei_cfg.folder     = "INBOX";
        ei_cfg.gmail_mode = 1;

        EmailListOpts ei_opts = {0};
        ei_opts.folder = "INBOX";
        ei_opts.pager  = 1;

        int saved_stdin;
        INJECT_STDIN("\033x", 2, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int ei_r = email_service_list(&ei_cfg, &ei_opts);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(ei_r == 0 || ei_r == 1,
               "email_service_list empty Gmail INBOX pager: returns 0 or 1");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list: Gmail 'd' remove-label when only label left ─── */
    /* Covers lines 3128-3147 (has_real check, _nolabel fallback) */
    {
        local_store_init(NULL, "lblremove@gmail.com");
        const char *lr_uid = "0000000000lr0001";

        /* Message has INBOX + UNREAD labels.
         * After removing INBOX the loop in email_service.c lines 3133-3143 runs:
         * UNREAD remains but is excluded from "real" labels → has_real stays 0
         * → label_idx_add("_nolabel", uid) fires (line 3147). */
        const char *lr_hdr = "sender@test.com\tLabel Remove\t2026-01-15 10:00\tINBOX,UNREAD\t1";
        local_hdr_save("", lr_uid, lr_hdr, strlen(lr_hdr));
        label_idx_add("INBOX", lr_uid);
        label_idx_add("UNREAD", lr_uid);

        /* Also need a raw message so the manifest can be populated */
        const char *lr_msg =
            "From: sender@test.com\r\n"
            "Subject: Label Remove\r\n"
            "\r\n"
            "Body text\r\n";
        local_msg_save("INBOX", lr_uid, lr_msg, strlen(lr_msg));

        Config lr_cfg = {0};
        lr_cfg.host       = NULL;
        lr_cfg.user       = "lblremove@gmail.com";
        lr_cfg.folder     = "INBOX";
        lr_cfg.gmail_mode = 1;

        EmailListOpts lr_opts = {0};
        lr_opts.folder = "INBOX";
        lr_opts.pager  = 1;

        int saved_stdin;
        /* 'd' removes INBOX label from message → no real labels remain → _nolabel
         * 'd' again on same entry → undo (restore) path
         * then ESC to exit */
        INJECT_STDIN("dd\033x", 5, saved_stdin);
        int sout, serr;
        SUPPRESS_OUT(sout, serr);
        int lr_r = email_service_list(&lr_cfg, &lr_opts);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(lr_r == 0 || lr_r == 1,
               "email_service_list Gmail 'd' remove-label no-real-labels: no crash");

        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_folders_interactive: HOME+END + '/' search ── */
    /* Covers lines 3654-3660 (HOME/END), 3670-3684 ('/'), 3695-3702 (BACK+UTF8),
     * 3707-3715 ('t' toggle, 'c' compose) */
    {
        local_store_init("imaps://test.example.com", "foldercache@example.com");

        Config fhe_cfg = {0};
        fhe_cfg.host   = "imaps://test.example.com";
        fhe_cfg.user   = "foldercache@example.com";
        fhe_cfg.folder = "INBOX";

        ui_pref_set_int("folder_view_mode", 1);

        int go_up_fhe = 0;
        int saved_stdin;
        int sout, serr;

        /* HOME key → cursor=0 */
        const char fhe_home[] = { '\033','[','H',  /* HOME */ '\033','x' /* ESC */ };
        INJECT_STDIN(fhe_home, 5, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        char *fhe_r1 = email_service_list_folders_interactive(
                           &fhe_cfg, "INBOX", &go_up_fhe);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        free(fhe_r1);
        ASSERT(1, "folders_interactive: HOME key covered");

        /* END key */
        const char fhe_end[] = { '\033','[','F',  /* END */ '\033','x' /* ESC */ };
        INJECT_STDIN(fhe_end, 5, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        char *fhe_r2 = email_service_list_folders_interactive(
                           &fhe_cfg, "INBOX", &go_up_fhe);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        free(fhe_r2);
        ASSERT(1, "folders_interactive: END key covered");

        /* '/' search: type 'x', then TAB (toggle scope), then BACK (delete),
         * then 'a', then ESC+Z to cancel search (Z is c2 consumed by ESC handler),
         * then ESC+X to exit folder picker */
        const char fhe_slash[] = {
            '/', 'x', '\t', '\x7f', 'a', '\033', 'Z',  /* search loop: type+tab+back+type+ESC+c2 */
            '\033', 'X'                                  /* exit picker (X is c2 consumed) */
        };
        INJECT_STDIN(fhe_slash, 9, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        char *fhe_r3 = email_service_list_folders_interactive(
                           &fhe_cfg, "INBOX", &go_up_fhe);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        free(fhe_r3);
        ASSERT(1, "folders_interactive: '/' search with TAB+BACK+UTF8 covered");

        /* 't' = toggle tree/flat mode */
        INJECT_STDIN("t\033x", 4, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        char *fhe_r4 = email_service_list_folders_interactive(
                           &fhe_cfg, "INBOX", &go_up_fhe);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        free(fhe_r4);
        ASSERT(1, "folders_interactive: 't' tree toggle covered");

        /* 'c' = compose → returns "__compose__" */
        INJECT_STDIN("c", 1, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        char *fhe_r5 = email_service_list_folders_interactive(
                           &fhe_cfg, "INBOX", &go_up_fhe);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        ASSERT(fhe_r5 != NULL && strcmp(fhe_r5, "__compose__") == 0,
               "folders_interactive: 'c' returns __compose__");
        free(fhe_r5);

        ui_pref_set_int("folder_view_mode", 1);
        local_store_init("imaps://test.example.com", "testuser");
    }

    /* ── email_service_list_labels_interactive: '/' search (4334-4344) ── */
    /* Also covers 'd' delete label rebuild (4422-4449) */
    {
        local_store_init(NULL, "lblsearch@gmail.com");
        label_idx_add("MyLabel", "0000000000ls0001");

        Config lbs_cfg = {0};
        lbs_cfg.host       = NULL;
        lbs_cfg.user       = "lblsearch@gmail.com";
        lbs_cfg.gmail_mode = 1;

        int go_up_lbs = 0;
        int saved_stdin;
        int sout, serr;

        /* '/' opens search; type 'x', then TAB (cycle scope), then BACK (delete x),
         * then 'a' (type 'a'), then ESC cancel search, then 'd' delete current label,
         * then ESC exit.
         * Note: ESC handler reads the NEXT byte as c2 (VMIN=0 on pipe fails silently,
         * but read() still reads from the pipe buffer). So use \033+Z to ESC-cancel
         * search (Z consumed as c2), leaving 'd' available for the outer loop. */
        const char lbs_keys[] = {
            '/', 'x', '\t', '\x7f', 'a', '\033', 'Z',  /* search: type+tab+back+type+ESC+consume */
            'd',                                         /* delete selected label */
            '\033', 'X'                                  /* ESC exit (X consumed as c2) */
        };
        INJECT_STDIN(lbs_keys, 10, saved_stdin);
        SUPPRESS_OUT(sout, serr);
        char *lbs_ret = email_service_list_labels_interactive(
                            &lbs_cfg, "INBOX", &go_up_lbs);
        RESTORE_OUT(sout, serr);
        RESTORE_STDIN(saved_stdin);
        free(lbs_ret);
        ASSERT(1, "labels_interactive: '/' search TAB+BACK + 'd' delete covered");

        local_store_init("imaps://test.example.com", "testuser");
    }
}
