/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-12 */
/* The single-line input loop: libtuikit's editing model on one terminal row. */
#include "input_line.h"
#include <tuikit/text.h>
#include <tuikit/tty.h>
#include <stdio.h>

/* The editing (keys, cursor, UTF-8, kill ring) is libtuikit's tui_lineedit.
 * The loop is this program's: tui_lineedit_run() reads the terminal in chunks
 * and drops what follows the key that ends it, so characters typed ahead of
 * Enter (a pasted line, a script's keys) would be lost.  tui_tty_read_key()
 * takes one byte at a time and leaves the rest in the input. */
int input_line_run(InputLine *il, int trow, const char *prompt) {
    int prompt_cols = tui_str_width(prompt);
    int ok = 0;

    il->trow = trow;
    for (;;) {
        /* Clear the row, prompt and text, the completion row below, then
         * put the cursor back on the input row (the hook may have moved it). */
        printf("\033[%d;1H\033[2K  %s%s", trow, prompt, il->buf);
        if (il->render_below) il->render_below(il);
        printf("\033[%d;%dH\033[?25h", trow,
               3 + prompt_cols + tui_lineedit_cursor_col(il));
        fflush(stdout);

        tui_event ev;
        if (tui_tty_read_key(STDIN_FILENO, &ev) <= 0)
            break;                              /* end of input or error */
        tui_edit_result res = tui_lineedit_handle(il, &ev);
        if (res == TUI_EDIT_ACCEPT) { ok = 1; break; }
        if (res == TUI_EDIT_CANCEL || res == TUI_EDIT_EOF) break;
    }

    printf("\033[%d;1H\033[2K\033[?25l", trow + 1);
    fflush(stdout);
    tui_lineedit_free(il);   /* the kill buffer; the text stays in the caller's buf */
    return ok;
}
