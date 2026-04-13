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
 * Returns the last printable ASCII character (32–126) that caused
 * terminal_read_key() to return TERM_KEY_IGNORE.
 * Returns 0 if the last ignored keystroke was not a printable character.
 */
int terminal_last_printable(void);

/**
 * Display column width of Unicode codepoint cp.
 * Returns 0 for non-printable/control characters,
 * 1 for normal characters, 2 for wide (CJK/emoji) characters.
 */
int terminal_wcwidth(uint32_t cp);

/**
 * Prompt for a password with echo suppressed.
 * Writes at most size-1 bytes to buf (NUL-terminated).
 * Returns the number of characters read, or -1 on error.
 */
int terminal_read_password(const char *prompt, char *buf, size_t size);

#endif /* PLATFORM_TERMINAL_H */
