/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* One full-screen frame: a header bar and a libtuikit table, written at once. */
#include "tui_frame.h"

#include <tuikit/buf.h>
#include <tuikit/canvas.h>
#include <tuikit/screen.h>
#include <tuikit/text.h>
#include <stdio.h>
#include <unistd.h>

int tui_frame_draw_table(const char *header, tui_table *table, int rows, int cols,
                         unsigned flags, const char *subtitle, const char *footer) {
    if (rows < 4 || cols < 1) return -1;
    tui_screen *scr = tui_screen_new(rows, cols);
    if (!scr) return -1;

    tui_canvas c = tui_canvas_of(scr);
    tui_style title = (flags & TUI_FRAME_PLAIN_TITLE) ? TUI_STYLE_DEFAULT : TUI_STYLE_HEADER;
    if (!(flags & TUI_FRAME_PLAIN_TITLE))
        tui_canvas_fill(c, tui_rect_make(0, 0, 1, cols), title);
    tui_canvas_put(c, 0, 0, header ? header : "", title);

    if (subtitle && *subtitle)
        tui_canvas_put(c, 1, 0, subtitle, TUI_STYLE_DEFAULT);
    if (footer && *footer)
        tui_canvas_put(c, rows - 1, 0, footer, TUI_STYLE_DEFAULT);

    int body = rows - 2 - TUI_FRAME_BOTTOM_ROWS;
    if (body > 0) {
        tui_canvas tc = tui_canvas_sub(c, tui_rect_make(2, 0, body, cols));
        tui_table_render(table, tc, TUI_STYLE_DEFAULT);
    }

    tui_buf out;
    tui_buf_init(&out);
    long n = tui_screen_render(scr, &out);
    int rc = -1;
    if (n >= 0) {
        fflush(stdout);
        rc = tui_buf_write_fd(&out, STDOUT_FILENO) == 0 ? 0 : -1;
    }
    tui_buf_free(&out);
    tui_screen_free(scr);
    return rc;
}

void tui_frame_print_bar(int row, const char *text, int cols) {
    if (cols < 1) cols = 80;
    if (!text) text = "";
    int used = 0;
    size_t n = tui_str_fit(text, cols, &used);
    printf("\033[%d;1H\033[1;37;44m%.*s", row, (int)n, text);
    for (int i = used; i < cols; i++) putchar(' ');
    printf("\033[0m");
}
