/**
 * POSIX terminal implementation.
 * Uses termios(3), ioctl TIOCGWINSZ, wcwidth(3).
 */
#include "../terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <wchar.h>

/** Opaque saved terminal state. Definition lives here (hidden from header). */
struct TermRawState {
    struct termios saved;
    int            active;
};

int terminal_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    return 80;
}

int terminal_rows(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return (int)ws.ws_row;
    return 0;
}

int terminal_is_tty(int fd) {
    return isatty(fd);
}

TermRawState *terminal_raw_enter(void) {
    TermRawState *state = malloc(sizeof(TermRawState));
    if (!state) return NULL;

    if (tcgetattr(STDIN_FILENO, &state->saved) != 0) {
        free(state);
        return NULL;
    }

    struct termios raw = state->saved;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    state->active = 1;
    return state;
}

void terminal_raw_exit(TermRawState **state) {
    if (!state || !*state) return;
    if ((*state)->active)
        tcsetattr(STDIN_FILENO, TCSANOW, &(*state)->saved);
    free(*state);
    *state = NULL;
}

/**
 * Read one byte from STDIN_FILENO via read(2) — NOT via getchar()/stdio.
 *
 * Rationale: getchar() uses the C stdio buffer.  When getchar() calls
 * read(2) with VMIN=0 VTIME=1 and gets a 0-byte return (timeout for bare
 * ESC), stdio marks the FILE* EOF flag.  All subsequent getchar() calls
 * then return EOF immediately without blocking, causing an infinite
 * redraw loop in the TUI.  Using read(2) directly bypasses the stdio
 * layer entirely and avoids the EOF flag problem.
 *
 * Returns the byte value [0..255], or -1 on error/timeout.
 */
static int read_byte(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    return (n == 1) ? (int)c : -1;
}

static int g_last_printable = 0;

int terminal_last_printable(void) { return g_last_printable; }

TermKey terminal_read_key(void) {
    /* The terminal must already be in raw mode (VMIN=1, VTIME=0). */
    g_last_printable = 0;
    int c = read_byte();
    TermKey result = TERM_KEY_IGNORE;   /* unknown input → silent no-op */

    if (c == '\033') {
        /* Temporarily switch to VMIN=0 VTIME=1 (100 ms timeout) to drain
         * the escape sequence without blocking if it is a bare ESC. */
        struct termios t;
        tcgetattr(STDIN_FILENO, &t);
        struct termios drain = t;
        drain.c_cc[VMIN]  = 0;
        drain.c_cc[VTIME] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &drain);

        int c2 = read_byte();
        if (c2 == '[') {
            int c3 = read_byte();
            switch (c3) {
            case 'A': result = TERM_KEY_PREV_LINE; break;  /* ESC[A — Up arrow    */
            case 'B': result = TERM_KEY_NEXT_LINE; break;  /* ESC[B — Down arrow  */
            case 'C': result = TERM_KEY_IGNORE;    break;  /* ESC[C — Right arrow */
            case 'D': result = TERM_KEY_IGNORE;    break;  /* ESC[D — Left arrow  */
            case '5': read_byte(); result = TERM_KEY_PREV_PAGE; break; /* ESC[5~ PgUp */
            case '6': read_byte(); result = TERM_KEY_NEXT_PAGE; break; /* ESC[6~ PgDn */
            default:
                if (c3 != -1) {
                    int ch;
                    while ((ch = read_byte()) != -1) {
                        if ((ch >= 'A' && ch <= 'Z') ||
                            (ch >= 'a' && ch <= 'z') || ch == '~') break;
                    }
                }
                result = TERM_KEY_IGNORE;
                break;
            }
        } else {
            result = TERM_KEY_ESC;   /* bare ESC — go back */
        }

        /* Restore VMIN=1 VTIME=0 raw mode. */
        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    } else if (c == '\n' || c == '\r') {
        result = TERM_KEY_ENTER;
    } else if (c == 3 /* Ctrl-C */) {
        result = TERM_KEY_QUIT;
    } else if (c == 127 || c == 8 /* DEL / Backspace */) {
        result = TERM_KEY_BACK;
    } else if (c == '\t') {
        result = TERM_KEY_TAB;
    } else if (c >= 32 && c <= 126) {
        g_last_printable = c;
        result = TERM_KEY_IGNORE;
    }
    /* c == -1 (read error/timeout) → result stays TERM_KEY_IGNORE */

    return result;
}

int terminal_wcwidth(uint32_t cp) {
    int w = wcwidth((wchar_t)cp);
    return (w < 0) ? 0 : w;
}

int terminal_read_password(const char *prompt, char *buf, size_t size) {
    if (!buf || size == 0) return -1;

    int fd = fileno(stdin);
    int is_tty = isatty(fd);

    if (is_tty) {
        printf("%s: ", prompt);
        fflush(stdout);

        struct termios oldt, newt;
        tcgetattr(fd, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(unsigned)ECHO;
        tcsetattr(fd, TCSANOW, &newt);

        char *line = NULL;
        size_t len = 0;
        ssize_t nread = getline(&line, &len, stdin);

        tcsetattr(fd, TCSANOW, &oldt);
        printf("\n");

        if (nread == -1 || !line) {
            free(line);
            return -1;
        }

        /* Strip trailing newline */
        size_t slen = strlen(line);
        if (slen > 0 && (line[slen-1] == '\n' || line[slen-1] == '\r'))
            line[--slen] = '\0';
        if (slen > 0 && (line[slen-1] == '\r'))
            line[--slen] = '\0';

        if (slen >= size) slen = size - 1;
        memcpy(buf, line, slen);
        buf[slen] = '\0';
        free(line);
        return (int)slen;
    } else {
        /* Non-TTY: read from stdin without echo manipulation */
        char *line = NULL;
        size_t len = 0;
        ssize_t nread = getline(&line, &len, stdin);
        if (nread == -1 || !line) {
            free(line);
            return -1;
        }
        size_t slen = strlen(line);
        if (slen > 0 && (line[slen-1] == '\n' || line[slen-1] == '\r'))
            line[--slen] = '\0';
        if (slen > 0 && (line[slen-1] == '\r'))
            line[--slen] = '\0';
        if (slen >= size) slen = size - 1;
        memcpy(buf, line, slen);
        buf[slen] = '\0';
        free(line);
        return (int)slen;
    }
}
