/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-27 */
/* Interface of the platform's terminal size and raw mode control. */
#ifndef PLATFORM_TERMINAL_H
#define PLATFORM_TERMINAL_H

#include <stddef.h>
#include <stdint.h>

/** Logical key codes returned by terminal_read_key(). */
typedef enum {
    TERM_KEY_QUIT      = 0,  /* Ctrl-C                          */
    TERM_KEY_NEXT_PAGE = 1,  /* PgDn                            */
    TERM_KEY_PREV_PAGE = 2,  /* PgUp                            */
    TERM_KEY_NEXT_LINE = 3,  /* Down-arrow                      */
    TERM_KEY_PREV_LINE = 4,  /* Up-arrow                        */
    TERM_KEY_IGNORE    = 5,  /* unknown / Space                 */
    TERM_KEY_ENTER     = 6,  /* Enter (\n or \r)                */
    TERM_KEY_ESC       = 7,  /* bare ESC                        */
    TERM_KEY_BACK      = 8,  /* Backspace / DEL                 */
    TERM_KEY_TAB       = 9,  /* Tab (\t)                        */
    TERM_KEY_LEFT      = 10, /* Left arrow                      */
    TERM_KEY_RIGHT     = 11, /* Right arrow                     */
    TERM_KEY_HOME      = 12, /* Home key                        */
    TERM_KEY_END       = 13, /* End key                         */
    TERM_KEY_DELETE    = 14, /* Delete (forward delete)         */
    TERM_KEY_SHIFT_TAB = 15  /* Shift+Tab (backtab, ESC[Z)      */
} TermKey;

/** Opaque saved terminal state (used for raw-mode enter/exit). */
typedef struct TermRawState TermRawState;

/** Returns the terminal width in columns, or 80 if unknown. */
int terminal_cols(void);

/** Returns the terminal height in rows, or 0 if unknown. */
int terminal_rows(void);

/**
 * Does the terminal named by TERM understand escape sequences (colour,
 * cursor movement, line erase)?  0 when TERM is unset, empty or "dumb";
 * 1 for every other value, known or not (EMAIL-108).
 */
int terminal_understands_escapes(void);

/** State of terminal_strip_csi(): where in an escape sequence the text stands. */
typedef struct { int state; } TermStrip;

/**
 * Copy @p n bytes of @p in to @p out without the CSI escape sequences
 * (ESC [ parameters, final byte 0x40-0x7E) in them.  A sequence cut by the
 * end of a chunk continues in the next call with the same @p st.
 * @return number of bytes written; at most @p n.
 */
size_t terminal_strip_csi(TermStrip *st, const char *in, size_t n, char *out);

/**
 * For the programs that write plain output: when the terminal does not
 * understand escape sequences (terminal_understands_escapes() is 0), replace
 * stdout and stderr by streams that drop the CSI sequences, so a dumb
 * terminal, a log or a pipe is never filled with colour and erase codes.
 * Call once at the start of main().  A no-op otherwise.
 */
void terminal_plain_output_if_needed(void);

/** Returns 1 if fd is connected to a terminal, 0 otherwise. */
int terminal_is_tty(int fd);

/**
 * Save current terminal mode and enter raw mode
 * (no echo, no canonical, no signal generation).
 * Returns an allocated TermRawState on success, NULL on failure.
 * Caller must call terminal_raw_exit() to restore and free.
 */
TermRawState *terminal_raw_enter(void);

/**
 * Restore the terminal to the state saved in *state and free it.
 * Sets *state = NULL.  Safe to call with NULL or *state == NULL.
 */
void terminal_raw_exit(TermRawState **state);

/** RAII cleanup wrapper for terminal_raw_exit. */
static inline void terminal_raw_exit_ptr(TermRawState **p) {
    terminal_raw_exit(p);
}
#define RAII_TERM_RAW __attribute__((cleanup(terminal_raw_exit_ptr)))

/**
 * Read one keypress and return a TermKey code.
 * The terminal must already be in raw mode.
 * Fully consumes multi-byte escape sequences.
 */
TermKey terminal_read_key(void);

/**
 * Ask the program to quit at once (EMAIL-219): ctrl-c pressed in a modal
 * (a menu, a picker, a prompt) closes it, and the next terminal_read_key()
 * of the view underneath returns TERM_KEY_QUIT, as if ctrl-c had been
 * pressed there.  The request is never cleared.
 */
void terminal_request_quit(void);

/** Forget the request (for tests that press ctrl-c in a modal and go on). */
void terminal_clear_quit_request(void);

/** Nonzero once terminal_request_quit() was called and not cleared. */
int terminal_quit_requested(void);

/**
 * Returns the last printable ASCII character (32–126) that caused
 * terminal_read_key() to return TERM_KEY_IGNORE.
 * Returns 0 if the last ignored keystroke was not a printable character.
 */
int terminal_last_printable(void);

/**
 * Returns the last printable character as a NUL-terminated UTF-8 string.
 * Works for both ASCII and multi-byte UTF-8 characters.
 * Returns "" (empty string) if the last keystroke was not printable.
 */
const char *terminal_last_utf8(void);

/**
 * Display column width of Unicode codepoint cp.
 * Returns 0 for non-printable/control characters,
 * 1 for normal characters, 2 for wide (CJK/emoji) characters.
 */
int terminal_wcwidth(uint32_t cp);

/**
 * Prompt for a password without echoing it back.
 *
 * On a terminal every accepted character is acknowledged with one mask
 * character, so the user can see that typing arrives; backspace erases one
 * character and Ctrl-U erases the field.  Escape sequences (arrow keys and
 * friends) are discarded instead of being taken as password bytes, and
 * anything typed ahead of the prompt is discarded rather than used.
 * The terminal mode is always restored before this function returns.
 *
 * When stdin is not a terminal one line is read as-is: no mode changes, no
 * masking, no type-ahead handling.
 *
 * Writes at most size-1 bytes to buf (NUL-terminated).
 *
 * @return Number of bytes read, or -1 when the field was cancelled (Ctrl-C),
 *         ended by EOF, or the terminal could not be reconfigured.  On -1 the
 *         buffer is wiped, so a caller must not use a partial password.
 */
int terminal_read_password(const char *prompt, char *buf, size_t size);

#endif /* PLATFORM_TERMINAL_H */
