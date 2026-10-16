/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-27 */
/* Unit tests of the platform layer. */
#include "test_helpers.h"
#include "platform/terminal.h"
#include "platform/path.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <termios.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <sys/wait.h>

/* Feed `bytes` to terminal_read_key() through a pipe (closed afterwards, so a
 * lone ESC is a bare ESC) and perform `count` reads.  keys[i] is the TermKey,
 * typed[i] the printable character that read produced or 0. */
static void feed_keys(const char *bytes, int count, TermKey *keys, int *typed) {
    int pfd[2];
    if (pipe(pfd) != 0) { for (int i = 0; i < count; i++) { keys[i] = TERM_KEY_QUIT; typed[i] = -1; } return; }
    size_t len = strlen(bytes);
    ssize_t w = write(pfd[1], bytes, len); (void)w;
    close(pfd[1]);
    int saved = dup(STDIN_FILENO);
    dup2(pfd[0], STDIN_FILENO);
    close(pfd[0]);
    for (int i = 0; i < count; i++) {
        keys[i]  = terminal_read_key();
        typed[i] = terminal_last_printable();
    }
    dup2(saved, STDIN_FILENO);
    close(saved);
}

/* Run terminal_plain_output_if_needed() in a child with TERM=term and read
 * what the child then writes to stdout and to stderr (EMAIL-108). */
static int plain_output_of(const char *term, char *out, size_t cap)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(fds[0]);
        /* the parent's unwritten output is not the child's to write */
        __fpurge(stdout);
        __fpurge(stderr);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        if (term) setenv("TERM", term, 1); else unsetenv("TERM");
        terminal_plain_output_if_needed();
        printf("\033[1mbold\033[0m \033[38;5;12mblue\033[0m\n");
        fprintf(stderr, "\r\033[K  progress 42\n");
        fflush(stdout);
        _exit(0);
    }
    close(fds[1]);
    size_t n = 0;
    ssize_t r;
    while (n + 1 < cap && (r = read(fds[0], out + n, cap - 1 - n)) > 0) n += (size_t)r;
    out[n] = '\0';
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

void test_platform(void) {

    /* ── terminal_wcwidth ───────────────────────────────────────────── */

    setlocale(LC_ALL, "");

    /* ASCII printable characters: width 1 */
    ASSERT(terminal_wcwidth('A')    == 1, "wcwidth: ASCII letter should be 1");
    ASSERT(terminal_wcwidth(' ')    == 1, "wcwidth: space should be 1");
    ASSERT(terminal_wcwidth('0')    == 1, "wcwidth: digit should be 1");

    /* Control characters: width 0 (not negative) */
    ASSERT(terminal_wcwidth('\n')   == 0, "wcwidth: newline should be 0");
    ASSERT(terminal_wcwidth('\t')   == 0, "wcwidth: tab should be 0");
    ASSERT(terminal_wcwidth(0x01)   == 0, "wcwidth: control char should be 0");

    /* Common accented Latin characters: width 1 */
    ASSERT(terminal_wcwidth(0x00E9) == 1, "wcwidth: e-acute (U+00E9) should be 1");
    ASSERT(terminal_wcwidth(0x00E1) == 1, "wcwidth: a-acute (U+00E1) should be 1");
    ASSERT(terminal_wcwidth(0x0151) == 1, "wcwidth: o-double-acute (U+0151) should be 1");

    /* CJK ideograph: width 2 */
    ASSERT(terminal_wcwidth(0x4E2D) == 2, "wcwidth: CJK U+4E2D should be 2");
    ASSERT(terminal_wcwidth(0x3042) == 2, "wcwidth: Hiragana U+3042 should be 2");

    /* ── terminal_is_tty ────────────────────────────────────────────── */

    /* When stdout is a pipe (as in test runner), fd 1 is not a tty */
    /* We cannot assert a specific value since it depends on the test
     * environment, but the function must return 0 or 1 without crashing. */
    int r = terminal_is_tty(STDOUT_FILENO);
    ASSERT(r == 0 || r == 1, "terminal_is_tty must return 0 or 1");

    /* Invalid fd must return 0 */
    ASSERT(terminal_is_tty(-1) == 0, "terminal_is_tty(-1) should return 0");
    ASSERT(terminal_is_tty(9999) == 0, "terminal_is_tty(9999) should return 0");

    /* ── terminal_cols ──────────────────────────────────────────────── */

    /* When stdout is not a tty (e.g. piped in CI), must fall back to 80. */
    if (!terminal_is_tty(STDOUT_FILENO)) {
        ASSERT(terminal_cols() == 80,
               "terminal_cols() should return 80 when stdout is not a tty");
    } else {
        /* On a real terminal it must be a positive value. */
        ASSERT(terminal_cols() > 0, "terminal_cols() must be positive");
    }

    /* ── terminal_rows ──────────────────────────────────────────────── */

    int rows = terminal_rows();
    if (!terminal_is_tty(STDOUT_FILENO)) {
        ASSERT(rows == 0, "terminal_rows() should return 0 when stdout is not a tty");
    } else {
        ASSERT(rows > 0, "terminal_rows() must be positive on a real terminal");
    }

    /* ── terminal_raw_enter / terminal_raw_exit ─────────────────────── */

    /* When stdin is not a tty (as in test runner), raw_enter must return NULL
     * gracefully (tcgetattr will fail). */
    if (!terminal_is_tty(STDIN_FILENO)) {
        TermRawState *s = terminal_raw_enter();
        ASSERT(s == NULL,
               "terminal_raw_enter should return NULL when stdin is not a tty");
        /* terminal_raw_exit(NULL) and terminal_raw_exit(&NULL) must be safe. */
        terminal_raw_exit(NULL);
        terminal_raw_exit(&s);   /* s is already NULL */
    }

    /* ── terminal_read_password — guard clauses ─────────────────────── */

    char pwbuf[64];
    /* NULL buf → -1 */
    ASSERT(terminal_read_password("test", NULL, 64) == -1,
           "terminal_read_password: NULL buf should return -1");
    /* size == 0 → -1 */
    ASSERT(terminal_read_password("test", pwbuf, 0) == -1,
           "terminal_read_password: size 0 should return -1");

    /* EOF on stdin → -1.
     *
     * This must read from a stream we closed ourselves, never the inherited
     * stdin: "not a tty" does not imply "empty or closed".  When the test
     * runner is started with a socket on stdin — as happens under a job
     * runner — getline() blocks forever and the whole suite hangs with no
     * output, which is exactly what it did before. */
    {
        int pfd[2];
        if (pipe(pfd) == 0) {
            close(pfd[1]);                    /* immediate EOF */
            int saved = dup(STDIN_FILENO);
            dup2(pfd[0], STDIN_FILENO);
            close(pfd[0]);
            clearerr(stdin);
            int n = terminal_read_password("test", pwbuf, sizeof(pwbuf));
            dup2(saved, STDIN_FILENO);
            close(saved);
            clearerr(stdin);
            ASSERT(n == -1, "terminal_read_password: EOF on stdin returns -1");
        }
    }

    /* Non-tty stdin with CRLF input: covers the \\r stripping step
     * (terminal.c line 214: if (slen > 0 && line[slen-1] == '\\r')). */
    {
        int pfd[2];
        if (pipe(pfd) == 0) {
            const char *crlf_input = "secret\r\n";
            ssize_t _wr = write(pfd[1], crlf_input, 8); (void)_wr;
            close(pfd[1]);
            int saved = dup(STDIN_FILENO);
            dup2(pfd[0], STDIN_FILENO);
            close(pfd[0]);
            clearerr(stdin); /* clear EOF from any previous non-tty read */
            char pwbuf2[64];
            int n2 = terminal_read_password("Password", pwbuf2, sizeof(pwbuf2));
            dup2(saved, STDIN_FILENO);
            close(saved);
            ASSERT(n2 == 6 && strcmp(pwbuf2, "secret") == 0,
                   "terminal_read_password: CRLF input strips \\r");
        }
    }

    /* A line longer than the buffer is truncated, never written past the end:
     * the caller passes a fixed array and the prompt has no way to grow it. */
    {
        int pfd[2];
        if (pipe(pfd) == 0) {
            const char *long_input = "0123456789abcdef\n";   /* 16 + newline */
            ssize_t _wr = write(pfd[1], long_input, strlen(long_input)); (void)_wr;
            close(pfd[1]);
            int saved = dup(STDIN_FILENO);
            dup2(pfd[0], STDIN_FILENO);
            close(pfd[0]);
            clearerr(stdin);
            char small[8];
            memset(small, 0x7F, sizeof(small));
            int n3 = terminal_read_password("Password", small, sizeof(small));
            dup2(saved, STDIN_FILENO);
            close(saved);
            clearerr(stdin);
            ASSERT(n3 == 7 && strcmp(small, "0123456") == 0,
                   "terminal_read_password: over-long input is truncated to size-1");
        }
    }

    /* ── terminal_read_key: escape sequences are consumed whole ─────── */
    {
        TermKey k[4]; int t[4];

        /* F1 (ESC O P) is no key and leaves nothing behind: the next read is
         * the 'x' that follows, not a 'P'. */
        feed_keys("\033OPx", 2, k, t);
        ASSERT(k[0] == TERM_KEY_IGNORE && t[0] == 0, "read_key: F1 (ESC O P) is ignored, not ESC");
        ASSERT(t[1] == 'x', "read_key: nothing of ESC O P is left to be typed");

        /* Ctrl+Up (ESC [ 1 ; 5 A): dropped whole. */
        feed_keys("\033[1;5Ax", 2, k, t);
        ASSERT(k[0] == TERM_KEY_IGNORE && t[0] == 0, "read_key: Ctrl+Up is ignored");
        ASSERT(t[1] == 'x', "read_key: '5A' of Ctrl+Up is not typed");

        feed_keys("\033[1;5Dy", 2, k, t);
        ASSERT(k[0] == TERM_KEY_IGNORE && t[1] == 'y', "read_key: Ctrl+Left leaves nothing behind");

        /* Shift+Delete, Ctrl+PgUp: modified '~' keys. */
        feed_keys("\033[3;2~z", 2, k, t);
        ASSERT(k[0] == TERM_KEY_IGNORE && t[1] == 'z', "read_key: Shift+Delete leaves nothing behind");
        feed_keys("\033[5;5~z", 2, k, t);
        ASSERT(k[0] == TERM_KEY_IGNORE && t[1] == 'z', "read_key: Ctrl+PgUp leaves nothing behind");

        /* F5 (ESC [ 1 5 ~) and Insert (ESC [ 2 ~): two-digit and unmapped
         * numbers are consumed to the '~' and are not Home or End. */
        feed_keys("\033[15~q", 2, k, t);
        ASSERT(k[0] == TERM_KEY_IGNORE && t[1] == 'q', "read_key: F5 is ignored and leaves nothing behind");
        feed_keys("\033[2~q", 2, k, t);
        ASSERT(k[0] == TERM_KEY_IGNORE && t[1] == 'q', "read_key: Insert is ignored");

        /* The keys that were already handled keep working. */
        feed_keys("\033[A", 1, k, t);
        ASSERT(k[0] == TERM_KEY_PREV_LINE, "read_key: ESC [ A is Up");
        feed_keys("\033[B", 1, k, t);
        ASSERT(k[0] == TERM_KEY_NEXT_LINE, "read_key: ESC [ B is Down");
        feed_keys("\033[3~", 1, k, t);
        ASSERT(k[0] == TERM_KEY_DELETE, "read_key: ESC [ 3 ~ is Delete");
        feed_keys("\033[5~", 1, k, t);
        ASSERT(k[0] == TERM_KEY_PREV_PAGE, "read_key: ESC [ 5 ~ is PgUp");
        feed_keys("\033[6~", 1, k, t);
        ASSERT(k[0] == TERM_KEY_NEXT_PAGE, "read_key: ESC [ 6 ~ is PgDn");
        feed_keys("\033[1~", 1, k, t);
        ASSERT(k[0] == TERM_KEY_HOME, "read_key: ESC [ 1 ~ is Home");
        feed_keys("\033[4~", 1, k, t);
        ASSERT(k[0] == TERM_KEY_END, "read_key: ESC [ 4 ~ is End");
        feed_keys("\033[Z", 1, k, t);
        ASSERT(k[0] == TERM_KEY_SHIFT_TAB, "read_key: ESC [ Z is Shift+Tab");

        /* Application cursor mode (SS3): the arrows must still be arrows. */
        feed_keys("\033OA", 1, k, t);
        ASSERT(k[0] == TERM_KEY_PREV_LINE, "read_key: ESC O A is Up");
        feed_keys("\033OB", 1, k, t);
        ASSERT(k[0] == TERM_KEY_NEXT_LINE, "read_key: ESC O B is Down");
        feed_keys("\033OC", 1, k, t);
        ASSERT(k[0] == TERM_KEY_RIGHT, "read_key: ESC O C is Right");
        feed_keys("\033OD", 1, k, t);
        ASSERT(k[0] == TERM_KEY_LEFT, "read_key: ESC O D is Left");
        feed_keys("\033OH", 1, k, t);
        ASSERT(k[0] == TERM_KEY_HOME, "read_key: ESC O H is Home");
        feed_keys("\033OF", 1, k, t);
        ASSERT(k[0] == TERM_KEY_END, "read_key: ESC O F is End");

        /* A lone ESC is still ESC. */
        feed_keys("\033", 1, k, t);
        ASSERT(k[0] == TERM_KEY_ESC, "read_key: a lone ESC is ESC");
    }

    /* ── terminal_raw_enter: flow control off, mode restored ─────────── */
    {
        int master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0) {
            const char *slave_name = ptsname(master);
            int slave = slave_name ? open(slave_name, O_RDWR | O_NOCTTY) : -1;
            if (slave >= 0) {
                struct termios before;
                tcgetattr(slave, &before);
                before.c_iflag |= IXON | IXOFF;           /* the usual default */
                before.c_lflag |= ICANON | ECHO | ISIG;
                tcsetattr(slave, TCSANOW, &before);

                int saved = dup(STDIN_FILENO);
                dup2(slave, STDIN_FILENO);
                TermRawState *st = terminal_raw_enter();
                ASSERT(st != NULL, "raw_enter: succeeds on a terminal");
                struct termios during;
                tcgetattr(slave, &during);
                ASSERT((during.c_iflag & IXON) == 0, "raw_enter: IXON is off (Ctrl-S is a key)");
                ASSERT((during.c_iflag & IXOFF) == 0, "raw_enter: IXOFF is off");
                ASSERT((during.c_lflag & (ICANON | ECHO | ISIG)) == 0,
                       "raw_enter: ICANON, ECHO and ISIG are off");
                terminal_raw_exit(&st);
                struct termios after;
                tcgetattr(slave, &after);
                ASSERT((after.c_iflag & IXON) != 0 && (after.c_lflag & ICANON) != 0,
                       "raw_exit: the saved mode is restored");
                ASSERT(st == NULL, "raw_exit: the state is cleared");
                dup2(saved, STDIN_FILENO);
                close(saved);
                close(slave);
            } else {
                ASSERT(0, "raw_enter test: the pty slave could not be opened");
            }
            close(master);
        } else {
            ASSERT(0, "raw_enter test: no pseudo-terminal could be made");
        }
    }

    /* ── platform_home_dir ──────────────────────────────────────────── */

    const char *home = platform_home_dir();
    ASSERT(home != NULL, "platform_home_dir should not return NULL");
    ASSERT(home[0] == '/', "platform_home_dir should return an absolute path");

    /* Must still work when HOME is unset (getpwuid fallback) */
    char saved_home[4096] = {0};
    const char *env_home = getenv("HOME");
    if (env_home) snprintf(saved_home, sizeof(saved_home), "%s", env_home);
    unsetenv("HOME");
    home = platform_home_dir();
    ASSERT(home != NULL, "platform_home_dir should fall back to getpwuid");
    if (saved_home[0]) setenv("HOME", saved_home, 1);

    /* ── platform_cache_dir ─────────────────────────────────────────── */

    /* Default: ~/.cache */
    unsetenv("XDG_CACHE_HOME");
    const char *cache = platform_cache_dir();
    ASSERT(cache != NULL, "platform_cache_dir should not return NULL");
    ASSERT(strstr(cache, ".cache") != NULL,
           "platform_cache_dir default should contain '.cache'");

    /* XDG override */
    setenv("XDG_CACHE_HOME", TP("test-xdg-cache"), 1);
    cache = platform_cache_dir();
    ASSERT(cache != NULL, "platform_cache_dir XDG should not return NULL");
    ASSERT(strcmp(cache, TP("test-xdg-cache")) == 0,
           "platform_cache_dir should respect XDG_CACHE_HOME");
    unsetenv("XDG_CACHE_HOME");

    /* ── platform_config_dir ────────────────────────────────────────── */

    /* Default: ~/.config */
    unsetenv("XDG_CONFIG_HOME");
    const char *cfg = platform_config_dir();
    ASSERT(cfg != NULL, "platform_config_dir should not return NULL");
    ASSERT(strstr(cfg, ".config") != NULL,
           "platform_config_dir default should contain '.config'");

    /* XDG override */
    setenv("XDG_CONFIG_HOME", TP("test-xdg-config"), 1);
    cfg = platform_config_dir();
    ASSERT(cfg != NULL, "platform_config_dir XDG should not return NULL");
    ASSERT(strcmp(cfg, TP("test-xdg-config")) == 0,
           "platform_config_dir should respect XDG_CONFIG_HOME");
    unsetenv("XDG_CONFIG_HOME");

    /* ── TERM and the escape sequences (EMAIL-108) ───────────────────── */
    {
        const char *saved = getenv("TERM");
        char *keep = saved ? strdup(saved) : NULL;
        unsetenv("TERM");
        ASSERT(!terminal_understands_escapes(), "TERM unset: no escape sequences");
        setenv("TERM", "", 1);
        ASSERT(!terminal_understands_escapes(), "TERM empty: no escape sequences");
        setenv("TERM", "dumb", 1);
        ASSERT(!terminal_understands_escapes(), "TERM=dumb: no escape sequences");
        setenv("TERM", "xterm-256color", 1);
        ASSERT(terminal_understands_escapes(), "TERM=xterm-256color: escape sequences");
        setenv("TERM", "linux", 1);
        ASSERT(terminal_understands_escapes(), "TERM=linux: escape sequences");
        setenv("TERM", "some-unknown-terminal", 1);
        ASSERT(terminal_understands_escapes(), "an unknown TERM is taken as ANSI");
        if (keep) { setenv("TERM", keep, 1); free(keep); } else unsetenv("TERM");

        /* The stripper, whole and cut at every possible place. */
        const char *in = "a\033[1mB\033[38;5;12mc\033[0m\r\033[Kd\033Xe";
        const char *want = "aBc\rdXe";
        char out[64];
        TermStrip st = {0};
        size_t o = terminal_strip_csi(&st, in, strlen(in), out);
        out[o] = '\0';
        ASSERT(strcmp(out, want) == 0, "CSI sequences are removed, other text kept");
        for (size_t cut = 0; cut <= strlen(in); cut++) {
            TermStrip s2 = {0};
            char o2[64];
            size_t n1 = terminal_strip_csi(&s2, in, cut, o2);
            size_t n2 = terminal_strip_csi(&s2, in + cut, strlen(in) - cut, o2 + n1);
            o2[n1 + n2] = '\0';
            ASSERT(strcmp(o2, want) == 0, "a sequence cut between two chunks is still removed");
        }

        /* The real thing: stdout and stderr of a child. */
        char got[256];
        ASSERT(plain_output_of("dumb", got, sizeof(got)) == 0, "child with TERM=dumb runs");
        ASSERT(strstr(got, "\033") == NULL, "TERM=dumb: no escape byte reaches the output");
        ASSERT(strstr(got, "bold blue") != NULL && strstr(got, "progress 42") != NULL,
               "TERM=dumb: the text of stdout and stderr is kept");
        ASSERT(plain_output_of(NULL, got, sizeof(got)) == 0, "child without TERM runs");
        ASSERT(strstr(got, "\033") == NULL, "TERM unset: no escape byte reaches the output");
        ASSERT(plain_output_of("xterm-256color", got, sizeof(got)) == 0, "child with xterm runs");
        ASSERT(strstr(got, "\033[1mbold") != NULL && strstr(got, "\033[K") != NULL,
               "TERM=xterm-256color: the escape sequences are written as they are");
    }
}
