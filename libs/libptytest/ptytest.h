#ifndef PTYTEST_H
#define PTYTEST_H

/**
 * @file ptytest.h
 * @brief PTY-based terminal test library for TUI programs.
 *
 * Opens a pseudo-terminal, forks and execs the program under test,
 * sends keystrokes, and inspects the virtual screen buffer.
 * The VT100 parser handles cursor positioning, screen erase, and SGR
 * attributes (bold, dim, reverse, colour).
 *
 * Depends only on POSIX (openpty, fork, poll).
 */

#include <stddef.h>

/* ── Cell attributes (bitmask) ───────────────────────────────────────── */

#define PTY_ATTR_NONE    0
#define PTY_ATTR_BOLD    (1 << 0)
#define PTY_ATTR_DIM     (1 << 1)
#define PTY_ATTR_REVERSE (1 << 2)

/* ── Special keys ────────────────────────────────────────────────────── */

typedef enum {
    PTY_KEY_ENTER = 0x0D,
    PTY_KEY_ESC   = 0x1B,
    PTY_KEY_TAB   = 0x09,
    PTY_KEY_BACK  = 0x7F,
    PTY_KEY_UP    = 0x100,
    PTY_KEY_DOWN,
    PTY_KEY_RIGHT,
    PTY_KEY_LEFT,
    PTY_KEY_PGUP,
    PTY_KEY_PGDN,
    PTY_KEY_HOME,
    PTY_KEY_END,
    PTY_KEY_CTRL_C = 0x03,
    PTY_KEY_CTRL_D = 0x04,
} PtyKey;

/* ── Session opaque handle ───────────────────────────────────────────── */

typedef struct PtySession PtySession;

/* ── Session lifecycle ───────────────────────────────────────────────── */

/**
 * @brief Creates a PTY session with the given terminal size.
 * @param cols  Terminal width in columns.
 * @param rows  Terminal height in rows.
 * @return Session handle, or NULL on failure. Caller must pty_close().
 */
PtySession *pty_open(int cols, int rows);

/**
 * @brief Forks and execs a program inside the PTY.
 * @param s     Session handle.
 * @param argv  NULL-terminated argument vector.
 * @return 0 on success, -1 on failure.
 */
int pty_run(PtySession *s, const char *argv[]);

/**
 * @brief Closes the session and frees resources.
 *
 * Sends SIGTERM to the child if still running, then SIGKILL after 100ms.
 */
void pty_close(PtySession *s);

/* ── Input ───────────────────────────────────────────────────────────── */

/** @brief Sends raw bytes to the PTY master (child stdin). */
void pty_send(PtySession *s, const char *bytes, size_t len);

/** @brief Sends a single key (handles VT100 escape sequences for arrows etc). */
void pty_send_key(PtySession *s, PtyKey key);

/** @brief Sends a NUL-terminated string. */
void pty_send_str(PtySession *s, const char *str);

/* ── Synchronisation ─────────────────────────────────────────────────── */

/**
 * @brief Reads PTY output and updates the screen buffer until @p text appears.
 * @param s          Session handle.
 * @param text       Text to wait for (searched in the full screen buffer).
 * @param timeout_ms Maximum wait time in milliseconds.
 * @return 0 if found, -1 on timeout.
 */
int pty_wait_for(PtySession *s, const char *text, int timeout_ms);

/**
 * @brief Waits until no new output arrives for @p quiet_ms milliseconds.
 * @return 0 on success, -1 if the child process has exited.
 */
int pty_settle(PtySession *s, int quiet_ms);

/**
 * @brief Reads and processes any pending PTY output (non-blocking).
 * @return Number of bytes read, or 0 if nothing available.
 */
int pty_drain(PtySession *s);

/* ── Screen inspection ───────────────────────────────────────────────── */

/**
 * @brief Returns the character at (row, col) as a UTF-8 string.
 *
 * Returns a pointer to an internal buffer; valid until the next pty_* call.
 * Out-of-bounds access returns an empty string.
 */
const char *pty_cell_text(PtySession *s, int row, int col);

/**
 * @brief Returns the SGR attribute bitmask at (row, col).
 */
int pty_cell_attr(PtySession *s, int row, int col);

/**
 * @brief Checks if @p row contains @p text (substring match).
 * @return 1 if found, 0 otherwise.
 */
int pty_row_contains(PtySession *s, int row, const char *text);

/**
 * @brief Checks if the entire screen contains @p text.
 * @return 1 if found, 0 otherwise.
 */
int pty_screen_contains(PtySession *s, const char *text);

/**
 * @brief Extracts the text of a single row into a caller-provided buffer.
 * @param buf  Destination buffer.
 * @param size Buffer size.
 * @return Pointer to buf, NUL-terminated.
 */
char *pty_row_text(PtySession *s, int row, char *buf, size_t size);

/**
 * @brief Enable/disable raw byte tracing from master fd to stdout.
 */
void pty_trace_enable(int on);

/**
 * @brief Returns the terminal dimensions.
 */
void pty_get_size(PtySession *s, int *cols, int *rows);

#endif /* PTYTEST_H */
