#include "test_helpers.h"
#include "input_line.h"
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>

/* ── stdin/stdout redirection helpers ──────────────────────────────────── */

/**
 * Redirect STDIN_FILENO from a pipe containing the given bytes.
 * Saves the old stdin fd into *saved_stdin.
 * Returns the read-end fd (already dup2'd to STDIN_FILENO; close it after
 * restoring).
 */
static int stdin_push(const char *bytes, size_t len, int *saved_stdin) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    /* Write all bytes then close write-end so read side sees EOF */
    if (write(pipefd[1], bytes, len) != (ssize_t)len) {
        close(pipefd[0]); close(pipefd[1]); return -1;
    }
    close(pipefd[1]);
    *saved_stdin = dup(STDIN_FILENO);
    dup2(pipefd[0], STDIN_FILENO);
    close(pipefd[0]);
    return 0;
}

/** Restore STDIN_FILENO from saved_stdin fd. */
static void stdin_pop(int saved_stdin) {
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
}

/**
 * Suppress stdout and stderr for the duration of input_line_run
 * (which emits ANSI escape codes we don't want in test output).
 * On return, *saved_out and *saved_err hold the real fds.
 */
static void output_suppress(int *saved_out, int *saved_err) {
    fflush(stdout); fflush(stderr);
    int null_fd = open("/dev/null", O_WRONLY);
    *saved_out = dup(STDOUT_FILENO);
    *saved_err = dup(STDERR_FILENO);
    if (null_fd >= 0) {
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
    }
}

static void output_restore(int saved_out, int saved_err) {
    fflush(stdout); fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);  close(saved_out);
    dup2(saved_err, STDERR_FILENO); close(saved_err);
}

/**
 * Run input_line_run with injected keypress bytes.
 * Suppresses TUI output.  Returns the result of input_line_run.
 */
static int run_with_keys(InputLine *il, int trow, const char *prompt,
                         const char *keys, size_t keylen) {
    int saved_stdin;
    if (stdin_push(keys, keylen, &saved_stdin) != 0) return -99;

    int saved_out, saved_err;
    output_suppress(&saved_out, &saved_err);

    int result = input_line_run(il, trow, prompt);

    output_restore(saved_out, saved_err);
    stdin_pop(saved_stdin);
    return result;
}

void test_input_line(void) {
    char buf[64];

    /* 1. Init with NULL initial_text → empty buffer, cursor at 0 */
    InputLine il;
    input_line_init(&il, buf, sizeof(buf), NULL);
    ASSERT(il.buf   == buf,       "buf pointer set");
    ASSERT(il.bufsz == sizeof(buf), "bufsz set");
    ASSERT(il.len   == 0,         "len 0 for NULL initial");
    ASSERT(il.cur   == 0,         "cur 0 for NULL initial");
    ASSERT(buf[0]   == '\0',      "buf NUL-terminated");

    /* 2. Init with empty string → same as NULL */
    input_line_init(&il, buf, sizeof(buf), "");
    ASSERT(il.len == 0, "len 0 for empty initial");
    ASSERT(il.cur == 0, "cur 0 for empty initial");

    /* 3. Init with text → len and cur at end */
    input_line_init(&il, buf, sizeof(buf), "hello");
    ASSERT(il.len == 5,           "len 5 for 'hello'");
    ASSERT(il.cur == 5,           "cur at end after init");
    ASSERT(strcmp(buf, "hello") == 0, "buf contains 'hello'");

    /* 4. Init truncates when text >= bufsz */
    char small[4];
    input_line_init(&il, small, sizeof(small), "toolong");
    ASSERT(il.len == 3,         "len truncated to bufsz-1");
    ASSERT(small[3] == '\0',    "buf NUL-terminated after truncation");

    /* 5. All callbacks NULL after init */
    input_line_init(&il, buf, sizeof(buf), "x");
    ASSERT(il.tab_fn       == NULL, "tab_fn NULL after init");
    ASSERT(il.shift_tab_fn == NULL, "shift_tab_fn NULL after init");
    ASSERT(il.render_below == NULL, "render_below NULL after init");

    /* 6. trow is 0 after init */
    ASSERT(il.trow == 0, "trow 0 after init");

    /* 7. UTF-8 multi-byte initial text */
    input_line_init(&il, buf, sizeof(buf), "héllo");
    /* 'é' is 2 bytes (0xC3 0xA9) → len = 6, cur = 6 */
    ASSERT(il.len == 6, "len accounts for UTF-8 bytes");
    ASSERT(il.cur == 6, "cur at byte end for UTF-8 text");
    ASSERT(strcmp(buf, "héllo") == 0, "UTF-8 content preserved");

    /* ── input_line_run tests ──────────────────────────────────────────── */

    /* 8. Enter on pre-filled buffer → returns 1, buffer unchanged */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hello");
        int res = run_with_keys(&ril, 5, "> ", "\r", 1);
        ASSERT(res == 1, "input_line_run: Enter returns 1");
        ASSERT(strcmp(rbuf, "hello") == 0, "input_line_run: buffer unchanged after Enter");
    }

    /* 9. ESC on pre-filled buffer → returns 0, buffer unchanged */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hello");
        /* Bare ESC: send ESC then a non-'[' byte so terminal_read_key treats it
         * as TERM_KEY_ESC (the fallback in the else branch). We use a second \r
         * which won't be read because input_line_run returns on ESC. */
        int res = run_with_keys(&ril, 5, "> ", "\033x", 2);
        ASSERT(res == 0, "input_line_run: ESC returns 0");
        ASSERT(strcmp(rbuf, "hello") == 0, "input_line_run: buffer unchanged after ESC");
    }

    /* 10. Ctrl-C → returns 0 */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "test");
        int res = run_with_keys(&ril, 5, "> ", "\x03", 1);
        ASSERT(res == 0, "input_line_run: Ctrl-C returns 0");
    }

    /* 11. Enter on empty buffer → returns 1 with empty buffer */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), NULL);
        int res = run_with_keys(&ril, 5, "Prompt: ", "\r", 1);
        ASSERT(res == 1, "input_line_run: Enter on empty buffer returns 1");
        ASSERT(rbuf[0] == '\0', "input_line_run: empty buffer stays empty");
    }

    /* 12. Type characters then Enter → buffer contains typed text */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), NULL);
        /* inject: 'h','i','\r' */
        int res = run_with_keys(&ril, 5, "> ", "hi\r", 3);
        ASSERT(res == 1, "input_line_run: typing then Enter returns 1");
        ASSERT(strcmp(rbuf, "hi") == 0, "input_line_run: typed chars in buffer");
    }

    /* 13. Backspace deletes last character */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "ab");
        /* DEL byte (backspace) then Enter */
        int res = run_with_keys(&ril, 5, "> ", "\x7f\r", 2);
        ASSERT(res == 1, "input_line_run: backspace+Enter returns 1");
        ASSERT(strcmp(rbuf, "a") == 0, "input_line_run: backspace deletes last char");
    }

    /* 14. Backspace at start of buffer → no change */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), NULL);
        /* backspace on empty → no-op, then Enter */
        int res = run_with_keys(&ril, 5, "> ", "\x7f\r", 2);
        ASSERT(res == 1, "input_line_run: backspace on empty returns 1");
        ASSERT(rbuf[0] == '\0', "input_line_run: backspace on empty is no-op");
    }

    /* 15. Left arrow then Backspace → deletes char before new cursor position */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "ab");
        /* Left arrow: ESC [ D  (3 bytes), then backspace, then Enter */
        int res = run_with_keys(&ril, 5, "> ", "\033[D\x7f\r", 6);
        ASSERT(res == 1, "input_line_run: left+backspace returns 1");
        ASSERT(strcmp(rbuf, "b") == 0, "input_line_run: left+backspace deletes 'a'");
    }

    /* 16. Right arrow at end → no change, then Enter */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hi");
        /* Right arrow at end is a no-op */
        int res = run_with_keys(&ril, 5, "> ", "\033[C\r", 4);
        ASSERT(res == 1, "input_line_run: right at end+Enter returns 1");
        ASSERT(strcmp(rbuf, "hi") == 0, "input_line_run: right at end is no-op");
    }

    /* 17. Home key moves cursor to start, then type char prepends it */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "bc");
        /* Home: ESC [ H, then type 'a', then Enter */
        int res = run_with_keys(&ril, 5, "> ", "\033[Ha\r", 5);
        ASSERT(res == 1, "input_line_run: Home+insert+Enter returns 1");
        ASSERT(strcmp(rbuf, "abc") == 0, "input_line_run: Home+insert prepends char");
    }

    /* 18. End key moves cursor to end, then type appends */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hi");
        /* Move to start first, then End, then type '!' */
        /* Home: ESC[H, End: ESC[F, type '!', Enter */
        int res = run_with_keys(&ril, 5, "> ", "\033[H\033[F!\r", 8);
        ASSERT(res == 1, "input_line_run: Home+End+insert+Enter returns 1");
        ASSERT(strcmp(rbuf, "hi!") == 0, "input_line_run: End+insert appends");
    }

    /* 19. Delete key (ESC[3~) removes char at cursor */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "abc");
        /* Home, then Delete: ESC[H ESC[3~, Enter */
        int res = run_with_keys(&ril, 5, "> ", "\033[H\033[3~\r", 8);
        ASSERT(res == 1, "input_line_run: Home+Delete+Enter returns 1");
        ASSERT(strcmp(rbuf, "bc") == 0, "input_line_run: Delete removes char at cursor");
    }

    /* 20. Delete at end (no-op) then Enter */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hi");
        /* Delete at end = no-op */
        int res = run_with_keys(&ril, 5, "> ", "\033[3~\r", 5);
        ASSERT(res == 1, "input_line_run: Delete at end+Enter returns 1");
        ASSERT(strcmp(rbuf, "hi") == 0, "input_line_run: Delete at end is no-op");
    }

    /* 21. Buffer full: inserting beyond bufsz is silently ignored */
    {
        char rbuf[4]; /* holds 3 chars + NUL */
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "abc");
        /* Try to insert 'd' when buffer is full, then Enter */
        int res = run_with_keys(&ril, 5, "> ", "d\r", 2);
        ASSERT(res == 1, "input_line_run: insert into full buffer returns 1");
        ASSERT(strcmp(rbuf, "abc") == 0, "input_line_run: insert into full buffer is no-op");
    }

    /* 22. Pre-filled 2-byte UTF-8 'é' in buffer, press Enter
     *     → il_render calls display_cols on the buffer → exercises 2-byte path
     *       (input_line.c:38-39) and cp_len 2-byte return (line 16) */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "\xC3\xA9");
        int res = run_with_keys(&ril, 5, "> ", "\r", 1);
        ASSERT(res == 1, "input_line_run: UTF-8 2-byte pre-filled + Enter returns 1");
        ASSERT((unsigned char)rbuf[0] == 0xC3,
               "input_line_run: 2-byte UTF-8 pre-filled preserved");
    }

    /* 23. Pre-filled 3-byte UTF-8 '中' in buffer, press Enter
     *     → exercises display_cols 3-byte path (input_line.c:40-43) */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "\xE4\xB8\xAD");
        int res = run_with_keys(&ril, 5, "> ", "\r", 1);
        ASSERT(res == 1, "input_line_run: UTF-8 3-byte pre-filled + Enter returns 1");
        ASSERT((unsigned char)rbuf[0] == 0xE4,
               "input_line_run: 3-byte UTF-8 pre-filled preserved");
    }

    /* 24. Pre-filled 4-byte UTF-8 😀 in buffer, press Enter
     *     → exercises display_cols 4-byte path (input_line.c:44-48) */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "\xF0\x9F\x98\x80");
        int res = run_with_keys(&ril, 5, "> ", "\r", 1);
        ASSERT(res == 1, "input_line_run: UTF-8 4-byte pre-filled + Enter returns 1");
        ASSERT((unsigned char)rbuf[0] == 0xF0,
               "input_line_run: 4-byte UTF-8 pre-filled preserved");
    }

    /* 25. Left over 2-byte UTF-8 → exercises cp_len 2-byte (input_line.c:16)
     *     and il_backspace over multi-byte sequence */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "\xC3\xA9");
        /* Left arrow moves cursor back over é (2 bytes), then Enter */
        int res = run_with_keys(&ril, 5, "> ", "\033[D\r", 4);
        ASSERT(res == 1, "input_line_run: left over 2-byte returns 1");
    }

    /* 26. Delete on 3-byte UTF-8 → exercises il_delete_fwd cp_len 3-byte
     *     (input_line.c:17,87) */
    {
        char rbuf[64];
        InputLine ril;
        /* "中x": Home, Delete removes '中' (3 bytes), leaving "x" */
        input_line_init(&ril, rbuf, sizeof(rbuf), "\xE4\xB8\xAD" "x");
        int res = run_with_keys(&ril, 5, "> ",
                                "\033[H" "\033[3~" "\r", 8);
        ASSERT(res == 1, "input_line_run: delete 3-byte UTF-8 returns 1");
        ASSERT(rbuf[0] == 'x' && rbuf[1] == '\0',
               "input_line_run: 3-byte UTF-8 deleted");
    }

    /* 27. Shift-Tab (ESC[Z) with no shift_tab_fn → no-op, then Enter
     *     → exercises terminal.c case 'Z' (TERM_KEY_SHIFT_TAB) */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hi");
        /* ESC [ Z = Shift-Tab, then Enter */
        int res = run_with_keys(&ril, 5, "> ", "\033[Z\r", 4);
        ASSERT(res == 1, "input_line_run: Shift-Tab+Enter returns 1");
        ASSERT(strcmp(rbuf, "hi") == 0, "input_line_run: Shift-Tab is no-op");
    }

    /* 28. ESC[1~ → TERM_KEY_HOME (nx == '~') → cursor to start
     *     → exercises terminal.c case '1' / nx=='~' branch */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "bc");
        /* ESC[1~ moves to start, type 'a', Enter → "abc" */
        int res = run_with_keys(&ril, 5, "> ", "\033[1~a\r", 6);
        ASSERT(res == 1, "input_line_run: ESC[1~+insert+Enter returns 1");
        ASSERT(strcmp(rbuf, "abc") == 0, "input_line_run: ESC[1~ moves to start");
    }

    /* 29. ESC[1x → TERM_KEY_IGNORE (nx != '~')
     *     → exercises terminal.c case '1' / nx!='~' branch */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hi");
        /* ESC[1x: c3='1', nx='x' → IGNORE, then Enter */
        int res = run_with_keys(&ril, 5, "> ", "\033[1x\r", 5);
        ASSERT(res == 1, "input_line_run: ESC[1x (IGNORE)+Enter returns 1");
        ASSERT(strcmp(rbuf, "hi") == 0, "input_line_run: ESC[1x is no-op");
    }

    /* 30. ESC[4~ → TERM_KEY_END → cursor to end
     *     → exercises terminal.c case '4' */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hi");
        /* Home, then ESC[4~ (End), type '!', Enter → "hi!" */
        int res = run_with_keys(&ril, 5, "> ", "\033[H\033[4~!\r", 9);
        ASSERT(res == 1, "input_line_run: ESC[4~+insert+Enter returns 1");
        ASSERT(strcmp(rbuf, "hi!") == 0, "input_line_run: ESC[4~ moves to end");
    }

    /* 31. ESC[7~ → TERM_KEY_HOME → cursor to start
     *     → exercises terminal.c case '7' */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "bc");
        /* ESC[7~ moves to start, type 'a', Enter → "abc" */
        int res = run_with_keys(&ril, 5, "> ", "\033[7~a\r", 6);
        ASSERT(res == 1, "input_line_run: ESC[7~+insert+Enter returns 1");
        ASSERT(strcmp(rbuf, "abc") == 0, "input_line_run: ESC[7~ moves to start");
    }

    /* 32. ESC[8~ → TERM_KEY_END → cursor to end
     *     → exercises terminal.c case '8' */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hi");
        /* Home, then ESC[8~ (End), type '!', Enter → "hi!" */
        int res = run_with_keys(&ril, 5, "> ", "\033[H\033[8~!\r", 9);
        ASSERT(res == 1, "input_line_run: ESC[8~+insert+Enter returns 1");
        ASSERT(strcmp(rbuf, "hi!") == 0, "input_line_run: ESC[8~ moves to end");
    }

    /* 33. Unknown ESC sequence → default drain → TERM_KEY_IGNORE → no-op
     *     → exercises terminal.c default case drain loop */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hi");
        /* ESC[9x: c3='9' → default, drains until 'x' (letter), then Enter */
        int res = run_with_keys(&ril, 5, "> ", "\033[9x\r", 5);
        ASSERT(res == 1, "input_line_run: unknown ESC seq+Enter returns 1");
        ASSERT(strcmp(rbuf, "hi") == 0, "input_line_run: unknown ESC seq is no-op");
    }

    /* 34. TAB key → TERM_KEY_TAB → no-op in input_line
     *     → exercises terminal.c line 146: result = TERM_KEY_TAB */
    {
        char rbuf[64];
        InputLine ril;
        input_line_init(&ril, rbuf, sizeof(rbuf), "hi");
        /* TAB then Enter: TAB is TERM_KEY_TAB which input_line ignores */
        int res = run_with_keys(&ril, 5, "> ", "\t\r", 2);
        ASSERT(res == 1, "input_line_run: TAB+Enter returns 1");
        ASSERT(strcmp(rbuf, "hi") == 0, "input_line_run: TAB is no-op");
    }
}
