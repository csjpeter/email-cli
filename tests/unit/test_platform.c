#include "test_helpers.h"
#include "platform/terminal.h"
#include "platform/path.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>

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

    /* Non-tty stdin path: getline on an empty/closed stream returns -1. */
    if (!terminal_is_tty(STDIN_FILENO)) {
        int n = terminal_read_password("test", pwbuf, sizeof(pwbuf));
        ASSERT(n == -1 || n >= 0,
               "terminal_read_password non-tty: must not crash");
    }

    /* Non-tty stdin with CRLF input: covers the \\r stripping step
     * (terminal.c line 214: if (slen > 0 && line[slen-1] == '\\r')). */
    if (!terminal_is_tty(STDIN_FILENO)) {
        int pfd[2];
        if (pipe(pfd) == 0) {
            const char *crlf_input = "secret\r\n";
            write(pfd[1], crlf_input, 8);
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
    setenv("XDG_CACHE_HOME", "/tmp/test-xdg-cache", 1);
    cache = platform_cache_dir();
    ASSERT(cache != NULL, "platform_cache_dir XDG should not return NULL");
    ASSERT(strcmp(cache, "/tmp/test-xdg-cache") == 0,
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
    setenv("XDG_CONFIG_HOME", "/tmp/test-xdg-config", 1);
    cfg = platform_config_dir();
    ASSERT(cfg != NULL, "platform_config_dir XDG should not return NULL");
    ASSERT(strcmp(cfg, "/tmp/test-xdg-config") == 0,
           "platform_config_dir should respect XDG_CONFIG_HOME");
    unsetenv("XDG_CONFIG_HOME");
}
