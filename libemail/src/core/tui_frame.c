/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* One full-screen frame: a header bar and a libtuikit table, written at once. */
#include "tui_frame.h"
#include "key_events.h"
#include "platform/terminal.h"
#include "status_line.h"

#include <tuikit/buf.h>
#include <tuikit/canvas.h>
#include <tuikit/screen.h>
#include <tuikit/text.h>
#include <tuikit/tty.h>
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

/* ── Overlays (EMAIL-110) ────────────────────────────────────────────── */

int tui_frame_overlay(void (*draw)(tui_canvas c, void *ctx), void *ctx,
                      int rows, int cols) {
    if (rows < 1 || cols < 1 || !draw) return -1;
    tui_screen *scr = tui_screen_new(rows, cols);
    if (!scr) return -1;
    tui_buf out;
    tui_buf_init(&out);
    /* Every cell starts as a no-break space: the screen believes the
     * terminal shows that, so the render writes exactly the cells the
     * drawing changes, the blanks of a box among them (a letter would be
     * skipped wherever the drawing has the same letter). */
    for (int r = 0; r < rows; r++) tui_screen_fill(scr, r, 0, cols, 0xA0, TUI_STYLE_DEFAULT);
    int rc = -1;
    if (tui_screen_render(scr, &out) >= 0) {
        tui_buf_reset(&out);                         /* the baseline is not written */
        draw(tui_canvas_of(scr), ctx);
        if (tui_screen_render(scr, &out) >= 0) {
            fflush(stdout);
            fflush(stderr);
            rc = out.len == 0 || tui_buf_write_fd(&out, STDOUT_FILENO) == 0 ? 0 : -1;
        }
    }
    tui_buf_free(&out);
    tui_screen_free(scr);
    return rc;
}

static void draw_dialog(tui_canvas c, void *ctx) {
    tui_dialog_render((tui_dialog *)ctx, c);
}

void tui_frame_modal_key(tui_event *ev, int is_list) {
    if (!ev || ev->type != TUI_EV_KEY) return;
    if (TUI_EV_IS_CTRL(ev, 'c')) {
        terminal_request_quit();
        ev->key = TUI_KEY_ESC; ev->mods = 0; ev->cp = 0; ev->utf8[0] = '\0';
    } else if (TUI_EV_IS_CHAR(ev, 'q') ||
               (ev->key == TUI_KEY_BACKSPACE && ev->mods == 0)) {
        ev->key = TUI_KEY_ESC; ev->mods = 0; ev->cp = 0; ev->utf8[0] = '\0';
    } else if (is_list && TUI_EV_IS_CHAR(ev, 'g')) {
        ev->key = TUI_KEY_HOME; ev->cp = 0; ev->utf8[0] = '\0';
    } else if (is_list && TUI_EV_IS_CHAR(ev, 'G')) {
        ev->key = TUI_KEY_END; ev->cp = 0; ev->utf8[0] = '\0';
    }
}

void tui_frame_modal_status(const char *const verbs[][2], int nverbs,
                            int selected, int total) {
    int rows = terminal_rows(), cols = terminal_cols();
    if (rows <= 0) rows = 24;
    if (cols <= 0) cols = 80;
    tui_statusline_hint h[8];
    int n = 0;
    for (int i = 0; i < nverbs && n < 8; i++) {
        h[n].key = verbs[i][0];
        h[n].label = verbs[i][1];
        h[n].priority = i == nverbs - 1 ? SL_PRIO_KEEP : 90 - 10 * i;
        n++;
    }
    status_line_draw(rows, cols, h, n, NULL, total > 0 ? selected : 0, total, NULL);
}

tui_dialog_state tui_frame_run_dialog(tui_dialog *d) {
    tui_decoder dec;
    tui_decoder_init(&dec);
    KeyEvents winch;
    key_events_begin(&winch);
    while (d->state == TUI_DIALOG_OPEN) {
        int rows = terminal_rows(), cols = terminal_cols();
        if (rows <= 0) rows = 24;
        if (cols <= 0) cols = 80;
        int pick = d->kind == TUI_DIALOG_CHECKLIST || d->kind == TUI_DIALOG_MENU;
        if (d->kind == TUI_DIALOG_CHECKLIST) {
            static const char *const v[][2] = {
                { "j/k", "select" }, { "Space", "tick" }, { "Enter", "apply" },
                { "Esc/q/BS", "cancel" } };
            tui_frame_modal_status(v, 4, d->list.selected + 1, d->nitems);
        } else if (d->kind == TUI_DIALOG_MENU) {
            static const char *const v[][2] = {
                { "j/k", "select" }, { "Enter", "choose" }, { "Esc/q/BS", "close" } };
            tui_frame_modal_status(v, 3, d->list.selected + 1, d->nitems);
        }
        tui_frame_overlay(draw_dialog, d, rows, cols);
        tui_event ev;
        int r = key_events_next(STDIN_FILENO, &dec, &ev, -1);
        if (r <= 0 || ev.type == TUI_EV_HANGUP) {
            d->state = TUI_DIALOG_CANCELLED;
            break;
        }
        if (ev.type == TUI_EV_RESIZE) continue;
        if (pick) tui_frame_modal_key(&ev, 1);
        tui_dialog_handle(d, &ev);
    }
    key_events_end(&winch);
    tui_decoder_free(&dec);
    return d->state;
}

typedef struct { tui_table *table; int row, col, h, w; } RowsOverlay;

static void draw_rows(tui_canvas c, void *ctx) {
    RowsOverlay *o = ctx;
    tui_canvas sub = tui_canvas_sub(c, tui_rect_make(o->row - 1, o->col - 1, o->h, o->w));
    o->table->flags |= TUI_TABLE_NO_HEADER;
    tui_table_render(o->table, sub, TUI_STYLE_DEFAULT);
}

int tui_frame_overlay_rows(tui_table *table, int row, int col, int h, int w) {
    int rows = terminal_rows(), cols = terminal_cols();
    if (rows <= 0) rows = 24;
    if (cols <= 0) cols = 80;
    if (row < 1 || col < 1 || h < 1 || w < 1) return -1;
    if (row - 1 + h > rows) rows = row - 1 + h;      /* a terminal of unknown height */
    if (col - 1 + w > cols) w = cols - (col - 1);
    RowsOverlay o = { table, row, col, h, w };
    return tui_frame_overlay(draw_rows, &o, rows, cols);
}
