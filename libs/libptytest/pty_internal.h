#ifndef PTY_INTERNAL_H
#define PTY_INTERNAL_H

/**
 * @file pty_internal.h
 * @brief Internal data structures shared between libptytest modules.
 */

#include "ptytest.h"
#include <stdint.h>
#include <sys/types.h>

/* ── Screen cell ─────────────────────────────────────────────────────── */

typedef struct {
    char    ch[8];   /**< UTF-8 character (up to 4 bytes + NUL) */
    int     attr;    /**< PTY_ATTR_* bitmask */
    uint8_t fg;      /**< Foreground colour: 0=default, 31=red, 32=green, 33=yellow, etc. */
} PtyCell;

/* ── Virtual screen buffer ───────────────────────────────────────────── */

typedef struct PtyScreen {
    int      cols, rows;
    int      cur_row, cur_col;
    int      cur_attr;        /**< Current SGR attribute state */
    uint8_t  cur_fg;          /**< Current foreground colour (0 = default) */
    int      pending_wrap;    /**< 1 = cursor is past last column, wrap on next char */
    PtyCell *cells;           /**< rows × cols cell array */
} PtyScreen;

PtyScreen  *pty_screen_new(int cols, int rows);
void        pty_screen_free(PtyScreen *scr);
void        pty_screen_feed(PtyScreen *scr, const char *data, size_t len);

/* ── PTY session ─────────────────────────────────────────────────────── */

struct PtySession {
    int        master_fd;
    pid_t      child_pid;
    int        cols, rows;
    PtyScreen *screen;
};

#endif /* PTY_INTERNAL_H */
