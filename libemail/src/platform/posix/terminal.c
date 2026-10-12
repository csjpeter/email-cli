/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-27 */
/* POSIX terminal control with termios and ioctl. */
/**
 * POSIX terminal implementation.
 *
 * Size, raw mode, key decoding and character width are libtuikit's
 * (tui_tty_*, tui_cp_width), reached through the interface of terminal.h so
 * that no caller changes.  The masked password prompt stays here: libtuikit
 * has no masked prompt (see terminal_read_password).
 */
#include "../terminal.h"
#include <tuikit/tty.h>
#include <tuikit/text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

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
    free(*state);
    *state = NULL;
}

static int  g_last_printable  = 0;
static char g_last_utf8[5]    = "";

int         terminal_last_printable(void) { return g_last_printable; }
const char *terminal_last_utf8(void)      { return g_last_utf8; }

/** Map a decoded key of libtuikit to the program's TermKey.  A key with a
 *  modifier that this program has no meaning for is ignored; Alt+character is
 *  the ESC it has always been (the character is consumed with it). */
TermKey terminal_read_key(void) {
    g_last_printable = 0;
    g_last_utf8[0]   = '\0';

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

/**
 * Read one line with the echo replaced by mask characters.
 *
 * The terminal is switched out of canonical mode so that every keystroke can
 * be acknowledged on screen: a blind field gives the user no way to tell a
 * typed password from a dead keyboard.  One '*' is written per character (not
 * per byte, so a multi-byte UTF-8 character counts once), backspace erases
 * one character, and Ctrl-U erases the whole field.
 *
 * Reading goes through stdio (getchar) rather than read(2) so that the stream
 * the other wizard fields use with getline() stays in sync.
 *
 * ISIG is cleared for the duration, so Ctrl-C and Ctrl-Z cannot end or stop
 * the process while echo is off; Ctrl-C is handled here and cancels the field
 * after the terminal has been put back the way it was.
 *
 * @return Number of bytes placed in @p buf, or -1 when the field was
 *         cancelled (Ctrl-C), ended by EOF, or the terminal could not be
 *         reconfigured.  On -1 the buffer is wiped.
 */
static int read_password_masked(int fd, const char *prompt, char *buf, size_t size) {
    struct termios oldt, newt;
    if (tcgetattr(fd, &oldt) != 0) return -1;
    newt = oldt;
    newt.c_lflag &= ~(unsigned)(ECHO | ECHONL | ICANON | ISIG);
    /* IXON off: Ctrl-S/Ctrl-Q are data here, not flow control.
     * ICRNL off: the Enter key arrives as CR and is recognised as such. */
    newt.c_iflag &= ~(unsigned)(IXON | ICRNL);
    newt.c_cc[VMIN]  = 1;
    newt.c_cc[VTIME] = 0;
    /* TCSAFLUSH discards whatever was typed ahead: those keystrokes were
     * echoed by the previous, canonical-mode field and must not silently
     * become part of a password. */
    if (tcsetattr(fd, TCSAFLUSH, &newt) != 0) return -1;

    /* The prompt is written only once echo is off, so type-ahead arriving
     * between the two cannot be echoed. */
    printf("%s: ", prompt);
    fflush(stdout);

    size_t len    = 0;   /* bytes in buf */
    int    masks  = 0;   /* mask characters currently on screen */
    int    rc     = -1;
    int    pushed = -2;  /* a byte read ahead and not consumed yet */

    for (;;) {
        int c = (pushed != -2) ? pushed : getchar();
        pushed = -2;

        if (c == EOF || c == 0x04) break;                   /* EOF / Ctrl-D */
        if (c == '\r' || c == '\n') { rc = (int)len; break; }

        if (c == 0x03) {                                    /* Ctrl-C */
            printf("^C");
            break;
        }

        if (c == 0x15) {                                    /* Ctrl-U */
            while (masks > 0) { printf("\b \b"); masks--; }
            fflush(stdout);
            len = 0;
            continue;
        }

        if (c == 0x7F || c == 0x08) {                       /* Backspace */
            if (len > 0) {
                do { len--; } while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80);
                if (masks > 0) { printf("\b \b"); masks--; fflush(stdout); }
            }
            continue;
        }

        if (c == 0x1B) {                                    /* ESC */
            /* Swallow a CSI/SS3 sequence (arrow keys, Home, F-keys): its
             * bytes would otherwise land in the password unseen. */
            int n = getchar();
            if (n == '[' || n == 'O') {
                int f;
                do { f = getchar(); } while (f != EOF && (f < 0x40 || f > 0x7E));
            } else {
                pushed = n;   /* a lone ESC: drop the ESC, keep what followed */
            }
            continue;
        }

        if (c < 0x20) continue;                             /* other controls */

        if (len + 1 < size) {
            buf[len++] = (char)c;
            if (((unsigned char)c & 0xC0) != 0x80) {        /* not a UTF-8 tail */
                putchar('*');
                masks++;
                fflush(stdout);
            }
        } else {
            putchar('\a');                                  /* field full */
            fflush(stdout);
        }
    }

    buf[len] = '\0';
    tcsetattr(fd, TCSANOW, &oldt);
    printf("\n");
    fflush(stdout);

    if (rc < 0) {
        memset(buf, 0, size);
        return -1;
    }
    return rc;
}

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
    if (isatty(fd))
        return read_password_masked(fd, prompt, buf, size);
    return read_password_plain(buf, size);
}
