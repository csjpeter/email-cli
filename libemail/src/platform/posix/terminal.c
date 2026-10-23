/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-27 */
/* POSIX terminal control with termios and ioctl. */
/**
 * POSIX terminal implementation.
 *
 * Size, raw mode, key decoding and character width are libtuikit's
 * (tui_tty_*, tui_cp_width), reached through the interface of terminal.h so
 * that no caller changes.  The masked password prompt on a terminal is
 * libtuikit's tui_tty_read_password_masked(); a stream that is not a terminal
 * is read through stdio, which the wizard's other fields share.
 */
#include "../terminal.h"
#include <tuikit/tty.h>
#include <tuikit/text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>

/** Opaque saved terminal state. Definition lives here (hidden from header). */
struct TermRawState {
    tui_tty_raw *raw;
};

int terminal_cols(void) {
    return tui_tty_cols(STDOUT_FILENO);
}

int terminal_rows(void) {
    return tui_tty_rows(STDOUT_FILENO);
}

int terminal_is_tty(int fd) {
    return tui_tty_is(fd);
}

TermRawState *terminal_raw_enter(void) {
    TermRawState *state = malloc(sizeof(TermRawState));
    if (!state) return NULL;
    /* No signal keys and no flow control: Ctrl-C arrives as a key, and
     * Ctrl-S cannot stop the terminal's output.  The library reads the mode
     * back and leaves it unchanged when the terminal did not take it. */
    state->raw = tui_tty_raw_enter(STDIN_FILENO, 0);
    if (!state->raw) {
        free(state);
        return NULL;
    }
    return state;
}

void terminal_raw_exit(TermRawState **state) {
    if (!state || !*state) return;
    tui_tty_raw_exit(&(*state)->raw);
    /* The table frames (tui_frame) hide the cursor; give it back with the terminal. */
    if (tui_tty_is(STDOUT_FILENO)) {
        static const char show[] = "\033[?25h";
        ssize_t n = write(STDOUT_FILENO, show, sizeof(show) - 1);
        (void)n;
    }
    free(*state);
    *state = NULL;
}

static int  g_last_printable  = 0;
static char g_last_utf8[5]    = "";

int         terminal_last_printable(void) { return g_last_printable; }
const char *terminal_last_utf8(void)      { return g_last_utf8; }

/* ── Resize and the minimum size (tui-interface-model) ───────────────── */

/* The screens draw once per key; a window resize must redraw at once, so
 * terminal_read_key() waits for input or SIGWINCH and returns "ignored" when
 * the size changed: every caller redraws at the top of its loop.  A resize
 * that arrives while the screen is being drawn is remembered in the flag and
 * is not lost.  Below 80x24 a short notice replaces whatever was drawn. */
#define TERM_MIN_COLS 80
#define TERM_MIN_ROWS 24

static volatile sig_atomic_t g_winch;

static void on_winch(int sig) {
    (void)sig;
    g_winch = 1;
}

static void install_winch_handler(void) {
    static int done;
    if (done) return;
    done = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, NULL);
}

static int too_small(void) {
    int c = tui_tty_cols(STDOUT_FILENO), r = tui_tty_rows(STDOUT_FILENO);
    return c > 0 && r > 0 && (c < TERM_MIN_COLS || r < TERM_MIN_ROWS);
}

static void show_too_small_notice(void) {
    int c = tui_tty_cols(STDOUT_FILENO), r = tui_tty_rows(STDOUT_FILENO);
    char msg[160];
    int n = snprintf(msg, sizeof(msg),
                     "\033[2J\033[HTerminal too small (%dx%d).\r\n"
                     "Needs %dx%d. Resize, or Esc to leave.",
                     c, r, TERM_MIN_COLS, TERM_MIN_ROWS);
    if (n > 0) {
        ssize_t w = write(STDOUT_FILENO, msg, (size_t)n);
        (void)w;
    }
}

/* Waits for input.  Returns 1 when a key is ready, 0 when the terminal was
 * resized (the caller returns "ignored" so that the screen is redrawn), -1
 * when waiting is not possible (not a terminal: read as before). */
static int wait_input_or_resize(void) {
    if (!tui_tty_is(STDIN_FILENO)) return 1;
    install_winch_handler();
    sigset_t block, orig;
    sigemptyset(&block);
    sigaddset(&block, SIGWINCH);
    sigprocmask(SIG_BLOCK, &block, &orig);
    int result;
    if (g_winch) {
        g_winch = 0;
        result = 0;
    } else {
        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        int pr = ppoll(&pfd, 1, NULL, &orig);
        if (pr < 0 && errno == EINTR) {
            g_winch = 0;
            result = 0;
        } else {
            result = 1;
        }
    }
    sigprocmask(SIG_SETMASK, &orig, NULL);
    return result;
}

/** Map a decoded key of libtuikit to the program's TermKey.  A key with a
 *  modifier that this program has no meaning for is ignored; Alt+character is
 *  the ESC it has always been (the character is consumed with it). */
static volatile int g_quit_requested;

void terminal_request_quit(void) {
    g_quit_requested = 1;
}

void terminal_clear_quit_request(void) {
    g_quit_requested = 0;
}

int terminal_quit_requested(void) {
    return g_quit_requested;
}

TermKey terminal_read_key(void) {
    g_last_printable = 0;
    g_last_utf8[0]   = '\0';

    if (g_quit_requested) return TERM_KEY_QUIT;     /* ctrl-c in a modal (EMAIL-219) */

    if (too_small()) {
        /* The notice stays until the window is large enough; only Esc and
         * Ctrl-C get through (to leave), everything else is swallowed. */
        for (;;) {
            show_too_small_notice();
            int w = wait_input_or_resize();
            if (w == 0) {
                if (!too_small()) return TERM_KEY_IGNORE;   /* redraw */
                continue;
            }
            tui_event nev;
            int nrc = tui_tty_read_key(STDIN_FILENO, &nev);
            if (nrc <= 0) return TERM_KEY_IGNORE;
            if (nev.type == TUI_EV_KEY &&
                (nev.key == TUI_KEY_ESC ||
                 (nev.key == TUI_KEY_CHAR && (nev.mods & TUI_MOD_CTRL) && nev.cp == 'c')))
                return nev.key == TUI_KEY_ESC ? TERM_KEY_ESC : TERM_KEY_QUIT;
            if (!too_small()) return TERM_KEY_IGNORE;
        }
    }
    if (wait_input_or_resize() == 0)
        return TERM_KEY_IGNORE;                 /* resized: redraw */
    if (too_small())
        return TERM_KEY_IGNORE;                 /* the notice comes next call */

    tui_event ev;
    int rc = tui_tty_read_key(STDIN_FILENO, &ev);
    if (rc <= 0 || ev.type != TUI_EV_KEY)
        return TERM_KEY_IGNORE;                 /* end of input, error, paste */

    if (ev.key == TUI_KEY_CHAR) {
        if (ev.mods & TUI_MOD_CTRL)
            return (ev.cp == 'c' && !(ev.mods & ~(unsigned)TUI_MOD_CTRL))
                       ? TERM_KEY_QUIT : TERM_KEY_IGNORE;
        if (ev.mods & TUI_MOD_ALT)
            return TERM_KEY_ESC;
        /* A printable character is reported as "ignored" with the text in
         * terminal_last_utf8(); ASCII also in terminal_last_printable(). */
        if (ev.cp >= 32 && ev.cp != 127 && !(ev.cp >= 0x80 && ev.cp <= 0x9F)) {
            snprintf(g_last_utf8, sizeof(g_last_utf8), "%s", ev.utf8);
            g_last_printable = (ev.cp <= 126) ? (int)ev.cp : 1;
        }
        return TERM_KEY_IGNORE;
    }

    /* Any modifier but Shift on Tab makes another key than the one this
     * program knows: it is dropped, not mistaken for the plain key. */
    if (ev.mods && !(ev.key == TUI_KEY_TAB && ev.mods == TUI_MOD_SHIFT))
        return TERM_KEY_IGNORE;

    switch (ev.key) {
    case TUI_KEY_ENTER:     return TERM_KEY_ENTER;
    case TUI_KEY_ESC:       return TERM_KEY_ESC;
    case TUI_KEY_BACKSPACE: return TERM_KEY_BACK;
    case TUI_KEY_TAB:       return (ev.mods & TUI_MOD_SHIFT) ? TERM_KEY_SHIFT_TAB
                                                             : TERM_KEY_TAB;
    case TUI_KEY_UP:        return TERM_KEY_PREV_LINE;
    case TUI_KEY_DOWN:      return TERM_KEY_NEXT_LINE;
    case TUI_KEY_LEFT:      return TERM_KEY_LEFT;
    case TUI_KEY_RIGHT:     return TERM_KEY_RIGHT;
    case TUI_KEY_HOME:      return TERM_KEY_HOME;
    case TUI_KEY_END:       return TERM_KEY_END;
    case TUI_KEY_PGUP:      return TERM_KEY_PREV_PAGE;
    case TUI_KEY_PGDN:      return TERM_KEY_NEXT_PAGE;
    case TUI_KEY_DELETE:    return TERM_KEY_DELETE;
    default:                return TERM_KEY_IGNORE;  /* Insert, F1-F12 */
    }
}

int terminal_wcwidth(uint32_t cp) {
    int w = tui_cp_width(cp);
    return (w < 0) ? 0 : w;
}

/* ── Password input ──────────────────────────────────────────────────── */

/** Read one line from a stream that is not a terminal — no echo to suppress. */
static int read_password_plain(char *buf, size_t size) {
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

int terminal_read_password(const char *prompt, char *buf, size_t size) {
    if (!buf || size == 0) return -1;

    int fd = fileno(stdin);
    if (isatty(fd)) {
        /* One '*' per character, Backspace/Ctrl-U, keys typed ahead are
         * discarded, Ctrl-C and Ctrl-D give -1 with the buffer wiped. */
        fflush(stdout);   /* the library writes with write(2) */
        int n = tui_tty_read_password_masked(fd, STDOUT_FILENO, prompt, buf, size);
        if (n < 0) memset(buf, 0, size);
        return n;
    }
    return read_password_plain(buf, size);
}

int terminal_read_line(const char *prompt, int mask, char *buf, size_t size) {
    if (!buf || size == 0) return TERM_LINE_EOF;
    buf[0] = '\0';
    int fd = STDIN_FILENO;
    fflush(stdout);   /* the library writes with write(2) */
    tui_tty_raw *raw = tui_tty_raw_enter(fd, 0);
    if (!raw) return TERM_LINE_EOF;
    /* Keys typed ahead were echoed in clear by the previous field: a masked
     * field must not take them silently. */
    if (mask) tcflush(fd, TCIFLUSH);
    int rc = TERM_LINE_EOF;
    size_t len = 0;
    int shown = 0;    /* characters on the screen */
    if (prompt) {
        if (write(STDOUT_FILENO, prompt, strlen(prompt)) < 0 ||
            write(STDOUT_FILENO, ": ", 2) < 0)
            goto done;
    }
    for (;;) {
        tui_event ev;
        int r = tui_tty_read_key(fd, &ev);
        if (r <= 0) break;
        if (ev.type != TUI_EV_KEY) continue;
        if (ev.key == TUI_KEY_ENTER) { rc = (int)len; break; }
        if (ev.key == TUI_KEY_ESC) { rc = TERM_LINE_CANCEL; break; }
        if (TUI_EV_IS_CTRL(&ev, 'c')) {
            if (write(STDOUT_FILENO, "^C", 2) < 0) { /* the quit still counts */ }
            rc = TERM_LINE_QUIT;
            break;
        }
        if (TUI_EV_IS_CTRL(&ev, 'd')) {
            if (len == 0) break;
            continue;
        }
        if (TUI_EV_IS_CTRL(&ev, 'u')) {
            while (shown > 0) {
                if (write(STDOUT_FILENO, "\b \b", 3) < 0) break;
                shown--;
            }
            len = 0;
            continue;
        }
        if (ev.key == TUI_KEY_BACKSPACE) {
            if (len == 0) continue;
            size_t start = len;
            do { start--; } while (start > 0 && ((unsigned char)buf[start] & 0xC0) == 0x80);
            if (!mask) {
                /* a wide character takes its columns: erase them all */
                int w = tui_str_width(buf + start);
                for (int i = 0; i < (w > 0 ? w : 1); i++)
                    if (write(STDOUT_FILENO, "\b \b", 3) < 0) break;
            } else if (write(STDOUT_FILENO, "\b \b", 3) < 0) {
                /* nothing more to do */
            }
            len = start;
            buf[len] = '\0';
            shown--;
            continue;
        }
        if (ev.key != TUI_KEY_CHAR || (ev.mods & (TUI_MOD_CTRL | TUI_MOD_ALT))) continue;
        size_t n = strlen(ev.utf8);
        if (len + n >= size) {
            if (write(STDOUT_FILENO, "\a", 1) < 0) { /* only a bell */ }
            continue;
        }
        memcpy(buf + len, ev.utf8, n);
        len += n;
        buf[len] = '\0';
        shown++;
        if (mask) {
            if (write(STDOUT_FILENO, "*", 1) < 0) break;
        } else if (write(STDOUT_FILENO, ev.utf8, n) < 0) {
            break;
        }
    }
done:
    tui_tty_raw_exit(&raw);
    if (write(STDOUT_FILENO, "\n", 1) < 0) { /* the line is done anyway */ }
    if (rc < 0) memset(buf, 0, size);
    return rc;
}

int terminal_understands_escapes(void) {
    const char *t = getenv("TERM");
    return t && t[0] && strcmp(t, "dumb") != 0;
}

size_t terminal_strip_csi(TermStrip *st, const char *in, size_t n, char *out) {
    /* state 0 = text, 1 = after ESC, 2 = inside a CSI sequence */
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (st->state == 0) {
            if (c == 0x1B) st->state = 1;
            else out[o++] = (char)c;
        } else if (st->state == 1) {
            if (c == '[') st->state = 2;
            else { st->state = 0; if (c == 0x1B) st->state = 1; else out[o++] = (char)c; }
        } else if (c >= 0x40 && c <= 0x7E) {
            st->state = 0;
        }
    }
    return o;
}

/** The stream behind a filtered stdout or stderr. */
typedef struct { FILE *real; TermStrip strip; } PlainOut;

static ssize_t plain_write(void *cookie, const char *buf, size_t size) {
    PlainOut *p = cookie;
    char tmp[1024];
    size_t done = 0;
    while (done < size) {
        size_t chunk = size - done < sizeof(tmp) ? size - done : sizeof(tmp);
        size_t o = terminal_strip_csi(&p->strip, buf + done, chunk, tmp);
        if (o && (fwrite(tmp, 1, o, p->real) != o || fflush(p->real) != 0)) return -1;
        done += chunk;
    }
    return (ssize_t)size;
}

static int plain_close(void *cookie) {
    PlainOut *p = cookie;
    int rc = fflush(p->real);
    free(p);
    return rc;
}

static FILE *plain_stream(FILE *real) {
    PlainOut *p = calloc(1, sizeof(*p));
    if (!p) return real;
    p->real = real;
    cookie_io_functions_t io = { .write = plain_write, .close = plain_close };
    FILE *f = fopencookie(p, "w", io);
    if (!f) { free(p); return real; }
    /* line buffered, so that stdout and stderr keep their order */
    setvbuf(f, NULL, real == stderr ? _IONBF : _IOLBF, 0);
    return f;
}

static FILE *g_plain_out, *g_plain_err;

/* Closes the filtering streams at exit, which flushes them and frees their
 * cookies (a leak otherwise, which LeakSanitizer reports as a failed exit). */
static void plain_streams_close(void) {
    FILE *po = g_plain_out, *pe = g_plain_err;
    g_plain_out = g_plain_err = NULL;
    if (po) fclose(po);
    if (pe) fclose(pe);
}

void terminal_plain_output_if_needed(void) {
    if (terminal_understands_escapes()) return;
    FILE *out = stdout, *err = stderr;
    FILE *po = plain_stream(out), *pe = plain_stream(err);
    if (po != out) g_plain_out = po;
    if (pe != err) g_plain_err = pe;
    stdout = po;
    stderr = pe;
    atexit(plain_streams_close);
}
