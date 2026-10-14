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
