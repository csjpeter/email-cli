#include "test_helpers.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <locale.h>
#include <fcntl.h>
#include <unistd.h>

/*
 * Include the full domain source so all static helpers are visible in
 * this translation unit.  email_service.c is NOT added to CMakeLists.txt
 * as a separate source — the #include below is the only compilation unit
 * that defines its symbols.
 */
#include "../../src/domain/email_service.c"

void test_email_service(void) {

    setlocale(LC_ALL, "");

    /* ── parse_uid_list ──────────────────────────────────────────────── */

    /* NULL input */
    {
        int *uids = NULL;
        int n = parse_uid_list(NULL, &uids);
        ASSERT(n == 0 && uids == NULL, "parse_uid_list: NULL input → 0");
    }

    /* Response has no "* SEARCH" token */
    {
        int *uids = NULL;
        int n = parse_uid_list("* OK done\r\n", &uids);
        ASSERT(n == 0 && uids == NULL, "parse_uid_list: no SEARCH line → 0");
    }

    /* Basic three-UID list */
    {
        int *uids = NULL;
        int n = parse_uid_list("* SEARCH 10 20 30\r\n", &uids);
        ASSERT(n == 3, "parse_uid_list: basic count");
        ASSERT(uids[0] == 10 && uids[1] == 20 && uids[2] == 30,
               "parse_uid_list: basic values");
        free(uids);
    }

    /* 40 UIDs — triggers the realloc branch (initial cap == 32) */
    {
        char big[1024];
        strcpy(big, "* SEARCH");
        for (int i = 1; i <= 40; i++) {
            char tmp[16];
            snprintf(tmp, sizeof(tmp), " %d", i);
            strcat(big, tmp);
        }
        int *uids = NULL;
        int n = parse_uid_list(big, &uids);
        ASSERT(n == 40, "parse_uid_list: 40 UIDs count");
        ASSERT(uids[0] == 1 && uids[39] == 40, "parse_uid_list: 40 UIDs boundary values");
        free(uids);
    }

    /* ── count_lines ─────────────────────────────────────────────────── */

    ASSERT(count_lines(NULL)         == 0, "count_lines: NULL → 0");
    ASSERT(count_lines("")           == 0, "count_lines: empty string → 0");
    ASSERT(count_lines("abc")        == 1, "count_lines: single line → 1");
    ASSERT(count_lines("a\nb")       == 2, "count_lines: two lines → 2");
    ASSERT(count_lines("a\nb\nc\n")  == 4, "count_lines: trailing newline → 4");

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

    /* Hard break — no spaces (lines 185-188): width=20, 25-char word */
    {
        char *r = word_wrap("aaaaaaaaaaaaaaaaaaaaaaaaa", 20);
        ASSERT(r != NULL, "word_wrap: hard break not NULL");
        ASSERT(strstr(r, "\n") != NULL, "word_wrap: hard break produces newline");
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
        print_body_page("Line 0\nLine 1\nLine 2\nLine 3\n", 1, 2);

        /* Body does not end with '\n': last segment hits the else branch
         * (printf("%s\n", p); break;) at lines 255-257 */
        print_body_page("Line 0\nNo newline here", 1, 5);

        /* from_line == 0, single print */
        print_body_page("only line", 0, 1);

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
        pipe(pipefd);
        fflush(stdout);
        int saved = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* Skip line 0; print lines 1-2 */
        print_body_page(body, 1, 2);

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
        pipe(pipefd);
        fflush(stdout);
        int saved = dup(STDOUT_FILENO);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        print_body_page(body, 1, 1);

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

    /* ── cmp_uid_entry ───────────────────────────────────────────────── */
    {
        UIDEntry a = {100, 1};  /* unseen */
        UIDEntry b = {200, 0};  /* seen   */
        /* unseen before seen (line 507-508) */
        ASSERT(cmp_uid_entry(&a, &b) < 0, "cmp_uid_entry: unseen before seen");
        ASSERT(cmp_uid_entry(&b, &a) > 0, "cmp_uid_entry: seen after unseen");
    }
    {
        UIDEntry c = {100, 1};
        UIDEntry d = {200, 1};
        /* both unseen: higher UID first (line 509) */
        ASSERT(cmp_uid_entry(&c, &d) > 0, "cmp_uid_entry: lower UID after higher");
        ASSERT(cmp_uid_entry(&d, &c) < 0, "cmp_uid_entry: higher UID before lower");
    }
    {
        UIDEntry e = {100, 0};
        UIDEntry f = {100, 0};
        /* equal: cmp == 0 */
        ASSERT(cmp_uid_entry(&e, &f) == 0, "cmp_uid_entry: equal entries → 0");
    }

    /* ── parse_list_line ─────────────────────────────────────────────── */

    /* Non-LIST line → NULL */
    {
        char sep = '.';
        char *r = parse_list_line("* OK done", &sep);
        ASSERT(r == NULL, "parse_list_line: non-LIST line → NULL");
    }

    /* Quoted separator + quoted mailbox */
    {
        char sep = '?';
        char *r = parse_list_line(
            "* LIST (\\HasChildren) \".\" \"INBOX.Sent\"", &sep);
        ASSERT(r != NULL, "parse_list_line: quoted mailbox not NULL");
        ASSERT(strcmp(r, "INBOX.Sent") == 0,
               "parse_list_line: quoted mailbox value");
        ASSERT(sep == '.', "parse_list_line: quoted separator value");
        free(r);
    }

    /* Unquoted separator (line 540) + unquoted mailbox (lines 552-554) */
    {
        char sep = '?';
        char *r = parse_list_line("* LIST () . INBOX", &sep);
        ASSERT(r != NULL, "parse_list_line: unquoted mailbox not NULL");
        ASSERT(strcmp(r, "INBOX") == 0,
               "parse_list_line: unquoted mailbox value");
        ASSERT(sep == '.', "parse_list_line: unquoted separator value");
        free(r);
    }

    /* Separator field absent (space only — unquoted, *p == ' ', no sep advance) */
    {
        char sep = '?';
        /* "* LIST ()  INBOX": after flags skip-spaces puts us at 'I' which
         * is not ' ' → sep='I', p advances, rest is parsed as mailbox "NBOX" */
        char *r = parse_list_line("* LIST ()  INBOX", &sep);
        /* We only check it doesn't crash and returns non-NULL */
        ASSERT(r != NULL, "parse_list_line: double-space sep does not crash");
        free(r);
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

    /* ── extract_fetch_literal ───────────────────────────────────────── */

    /* NULL input */
    {
        char *r = extract_fetch_literal(NULL, 0);
        ASSERT(r == NULL, "extract_fetch_literal: NULL → NULL");
    }

    /* No literal marker */
    {
        const char *resp = "* 1 FETCH (FLAGS (\\Seen))\r\n";
        char *r = extract_fetch_literal(resp, strlen(resp));
        ASSERT(r == NULL, "extract_fetch_literal: no literal → NULL");
    }

    /* Basic FETCH response: BODY.PEEK[] literal */
    {
        const char *msg = "From: alice@example.com\r\nSubject: Hi\r\n\r\nHello!\r\n";
        char resp[256];
        int n = snprintf(resp, sizeof(resp),
                         "* 1 FETCH (BODY[] {%zu}\r\n%s)\r\nA001 OK FETCH done\r\n",
                         strlen(msg), msg);
        char *r = extract_fetch_literal(resp, (size_t)n);
        ASSERT(r != NULL, "extract_fetch_literal: basic response → non-NULL");
        ASSERT(strcmp(r, msg) == 0, "extract_fetch_literal: content matches");
        free(r);
    }

    /* Response with \\r\\n line ending before literal */
    {
        const char *msg = "Subject: Test\r\n\r\nbody\r\n";
        char resp[256];
        int n = snprintf(resp, sizeof(resp),
                         "* 3 FETCH (BODY[] {%zu}\r\n%s)\r\n",
                         strlen(msg), msg);
        char *r = extract_fetch_literal(resp, (size_t)n);
        ASSERT(r != NULL, "extract_fetch_literal: CRLF before literal → non-NULL");
        ASSERT(strcmp(r, msg) == 0, "extract_fetch_literal: CRLF content correct");
        free(r);
    }

    /* Truncated response (reported size > available bytes) */
    {
        const char *resp = "* 1 FETCH (BODY[] {1000}\r\nshort)\r\n";
        size_t len = strlen(resp);
        char *r = extract_fetch_literal(resp, len);
        /* size (1000) > available bytes after CRLF → clamp to available */
        ASSERT(r != NULL, "extract_fetch_literal: truncated → non-NULL (clamped)");
        ASSERT(strncmp(r, "short)\r\n", 8) == 0,
               "extract_fetch_literal: truncated content clamped correctly");
        free(r);
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
}
