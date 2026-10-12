/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-12 */
/* The single-line input loop: libtuikit's line editor on one terminal row. */
#include "input_line.h"
#include <tuikit/tty.h>
#include <stdio.h>
#include <unistd.h>

/* The editing and the loop are libtuikit's tui_lineedit_run().  It writes with
 * write(2), while this program's completion hooks print through stdio; the
 * hook of the caller is therefore called through a wrapper that flushes, so
 * its output lands before the library puts the cursor back on the row. */
static void (*caller_render_below)(const tui_lineedit *);

static void render_below_flushed(const tui_lineedit *le) {
    if (caller_render_below) caller_render_below(le);
    fflush(stdout);
}

int input_line_run(InputLine *il, int trow, const char *prompt) {
    caller_render_below = il->render_below;
    if (caller_render_below) il->render_below = render_below_flushed;
    fflush(stdout);

    /* Column 3: the two-column indent of the screens. */
    int rc = tui_lineedit_run(il, STDIN_FILENO, STDOUT_FILENO, trow, 3, prompt);

    il->render_below = caller_render_below;
    caller_render_below = NULL;
    tui_lineedit_free(il);   /* the kill buffer; the text stays in the caller's buf */
    return rc == 1;
}
