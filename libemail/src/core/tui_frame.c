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
#include <tuikit/filechooser.h>
#include <tuikit/screen.h>
#include <tuikit/textview.h>
#include <tuikit/text.h>
#include <tuikit/tty.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
    {   /* where the notice area may start (EMAIL-285) */
        int used = 0;
        tui_str_fit(header ? header : "", cols, &used);
        status_line_set_header_width(used);
    }

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
    if (row == 1) status_line_set_header_width(used);
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
        else if (d->kind == TUI_DIALOG_CONFIRM) tui_frame_modal_key(&ev, 0);   /* q and Backspace answer No (EMAIL-312) */
        tui_dialog_handle(d, &ev);
    }
    key_events_end(&winch);
    tui_decoder_free(&dec);
    return d->state;
}

static void draw_viewdoc(tui_canvas c, void *ctx) {
    tui_viewdoc_render((tui_viewdoc_page *)ctx, c);
}

tui_dialog_state tui_frame_run_viewdoc(tui_viewdoc_page *p) {
    tui_decoder dec;
    tui_decoder_init(&dec);
    KeyEvents winch;
    key_events_begin(&winch);
    while (p->state == TUI_DIALOG_OPEN) {
        int rows = terminal_rows(), cols = terminal_cols();
        if (rows <= 0) rows = 24;
        if (cols <= 0) cols = 80;
        tui_frame_overlay(draw_viewdoc, p, rows, cols);
        tui_event ev;
        int r = key_events_next(STDIN_FILENO, &dec, &ev, -1);
        if (r <= 0 || ev.type == TUI_EV_HANGUP) {
            p->state = TUI_DIALOG_CANCELLED;
            break;
        }
        if (ev.type == TUI_EV_RESIZE) continue;
        if (TUI_EV_IS_CTRL(&ev, 'c')) {   /* back, and the program quits */
            terminal_request_quit();
            ev.key = TUI_KEY_ESC; ev.mods = 0; ev.cp = 0; ev.utf8[0] = '\0';
        }
        tui_viewdoc_handle(p, &ev);
    }
    key_events_end(&winch);
    tui_decoder_free(&dec);
    return p->state;
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

/* ── File chooser (EMAIL-291) ─────────────────────────────────────────── */

static void draw_chooser(tui_canvas c, void *ctx) {
    tui_filechooser_render((tui_filechooser *)ctx, c);
}

/* The next UTF-8 character of s as a key event; returns its length, 0 at the end. */
static size_t char_event(const char *s, tui_event *ev) {
    unsigned char b = (unsigned char)s[0];
    size_t n = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
    uint32_t cp = n == 1 ? b : (uint32_t)(b & (0xFF >> (n + 1)));
    for (size_t i = 1; i < n; i++) {
        if (!s[i]) return 0;
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    }
    memset(ev, 0, sizeof(*ev));
    ev->type = TUI_EV_KEY;
    ev->key = TUI_KEY_CHAR;
    ev->cp = cp;
    memcpy(ev->utf8, s, n);
    return n;
}

int tui_frame_run_filechooser(const char *title, tui_fc_mode mode, const char *start,
                              const char *name, char *out, size_t outsz) {
    if (!out || outsz < 2) return -1;
    tui_filechooser fc;
    if (tui_filechooser_open(&fc, title, mode,
                             start, NULL) != 0)
        return -1;
    for (const char *p = name; p && *p;) {          /* the suggested name, typed */
        tui_event ev;
        size_t n = char_event(p, &ev);
        if (n == 0) break;
        tui_filechooser_handle(&fc, &ev);
        p += n;
    }
    tui_decoder dec;
    tui_decoder_init(&dec);
    KeyEvents winch;
    key_events_begin(&winch);
    while (fc.state == TUI_FC_ACTIVE) {
        int rows = terminal_rows(), cols = terminal_cols();
        if (rows <= 0) rows = 24;
        if (cols <= 0) cols = 80;
        static const char *const v[][2] = {
            { "Tab", "complete" }, { "Enter", "choose" }, { "Alt-H", "hidden" },
            { "Esc", "cancel" } };
        tui_frame_modal_status(v, 4, 0, 0);
        tui_frame_overlay(draw_chooser, &fc, rows, cols);
        tui_event ev;
        int r = key_events_next(STDIN_FILENO, &dec, &ev, 50);
        if (r < 0 || (r > 0 && ev.type == TUI_EV_HANGUP)) {
            fc.state = TUI_FC_CANCELLED;
            break;
        }
        if (r == 0) {                                /* idle: a directory may have arrived */
            tui_filechooser_poll(&fc);
            continue;
        }
        tui_filechooser_poll(&fc);
        if (ev.type == TUI_EV_RESIZE) continue;
        if (TUI_EV_IS_CTRL(&ev, 'c')) terminal_request_quit();
        tui_filechooser_handle(&fc, &ev);
    }
    key_events_end(&winch);
    tui_decoder_free(&dec);
    int rc = -1;
    if (fc.state == TUI_FC_OK && fc.path && strlen(fc.path) < outsz) {
        memcpy(out, fc.path, strlen(fc.path) + 1);
        rc = 0;
    }
    tui_filechooser_free(&fc);
    return rc;
}

/* ── Scrollable text and form modal (EMAIL-312) ──────────────────────── */

typedef struct { tui_textview *tv; } TextDraw;

static void draw_text(tui_canvas c, void *ctx) {
    TextDraw *d = ctx;
    int rows = tui_canvas_rows(c), cols = tui_canvas_cols(c);
    tui_canvas body = tui_canvas_sub(c, tui_rect_make(1, 1, rows - 2 > 0 ? rows - 2 : 1,
                                                       cols > 2 ? cols - 2 : cols));
    tui_textview_render(d->tv, body, TUI_STYLE_DEFAULT);
}

int tui_frame_run_textview(const char *title, const char *text, const char *accept,
                           const char *accept_label) {
    tui_textview tv;
    tui_textview_init(&tv);
    tui_textview_set_text(&tv, text ? text : "");
    tui_decoder dec;
    tui_decoder_init(&dec);
    KeyEvents winch;
    key_events_begin(&winch);
    int result = 0, done = 0;
    printf("\033[2J");
    while (!done) {
        int rows = terminal_rows(), cols = terminal_cols();
        if (rows <= 0) rows = 24;
        if (cols <= 0) cols = 80;
        tui_frame_print_bar(1, title, cols);
        TextDraw td = { &tv };
        tui_frame_overlay(draw_text, &td, rows, cols);
        int page = rows - 3 > 1 ? rows - 3 : 1;
        char key[2] = { accept && accept[0] ? accept[0] : 0, 0 };
        int total = tui_textview_lines(&tv);
        int top = tui_textview_top(&tv);
        if (accept && accept[0]) {
            const char *const v[][2] = { { "j/k", "scroll" }, { key, accept_label ? accept_label : "ok" },
                                         { "Esc/q/BS", "back" } };
            tui_frame_modal_status(v, 3, top + 1 > total ? total : top + 1, total);
        } else {
            static const char *const v[][2] = { { "j/k", "scroll" }, { "Esc/q/BS", "back" } };
            tui_frame_modal_status(v, 2, top + 1 > total ? total : top + 1, total);
        }
        tui_event ev;
        int r = key_events_next(STDIN_FILENO, &dec, &ev, -1);
        if (r <= 0 || ev.type == TUI_EV_HANGUP) { done = 1; break; }
        if (ev.type == TUI_EV_RESIZE) { printf("\033[2J"); continue; }
        if (ev.type != TUI_EV_KEY) continue;
        if (accept && ev.key == TUI_KEY_CHAR && ev.mods == 0 && ev.cp < 128 && ev.cp &&
            strchr(accept, (int)ev.cp)) {
            result = (int)ev.cp;
            break;
        }
        tui_frame_modal_key(&ev, 1);
        if (ev.key == TUI_KEY_ESC) break;
        if (TUI_EV_IS_CHAR(&ev, 'j')) tui_textview_scroll(&tv, 1);
        else if (TUI_EV_IS_CHAR(&ev, 'k')) tui_textview_scroll(&tv, -1);
        else if (ev.key == TUI_KEY_PGDN) tui_textview_scroll(&tv, page);
        else if (ev.key == TUI_KEY_PGUP) tui_textview_scroll(&tv, -page);
        else tui_textview_handle(&tv, &ev);
    }
    key_events_end(&winch);
    tui_decoder_free(&dec);
    tui_textview_free(&tv);
    printf("\033[2J");
    fflush(stdout);
    return result;
}

typedef struct { tui_fieldset *f; const char *title; } FormDraw;

static void draw_form(tui_canvas c, void *ctx) {
    FormDraw *d = ctx;
    int rows = tui_canvas_rows(c), cols = tui_canvas_cols(c);
    int h = d->f->nfields + 5, w = cols - 4 < 70 ? cols - 4 : 70;
    if (h > rows - 2) h = rows - 2;
    tui_rect box = tui_rect_center(tui_canvas_rect(c), h, w);
    tui_canvas_fill(c, box, TUI_STYLE_DEFAULT);
    tui_canvas_box(c, box, d->title, TUI_STYLE_DEFAULT);
    tui_canvas inner = tui_canvas_sub(c, tui_rect_inset(box, 1));
    tui_fieldset_render(d->f, inner);
}

tui_dialog_state tui_frame_run_fieldset(const char *title, tui_fieldset *f) {
    tui_decoder dec;
    tui_decoder_init(&dec);
    KeyEvents winch;
    key_events_begin(&winch);
    FormDraw fd = { f, title };
    while (f->state == TUI_DIALOG_OPEN) {
        int rows = terminal_rows(), cols = terminal_cols();
        if (rows <= 0) rows = 24;
        if (cols <= 0) cols = 80;
        static const char *const v[][2] = { { "Tab", "next field" }, { "Left/Right", "change" },
                                            { "Enter", "ok" }, { "Esc", "cancel" } };
        tui_frame_modal_status(v, 4, 0, 0);
        tui_frame_overlay(draw_form, &fd, rows, cols);
        tui_event ev;
        int r = key_events_next(STDIN_FILENO, &dec, &ev, -1);
        if (r <= 0 || ev.type == TUI_EV_HANGUP) { f->state = TUI_DIALOG_CANCELLED; break; }
        if (ev.type == TUI_EV_RESIZE) { printf("\033[2J"); continue; }
        if (TUI_EV_IS_CTRL(&ev, 'c')) terminal_request_quit();
        tui_fieldset_handle(f, &ev);
    }
    key_events_end(&winch);
    tui_decoder_free(&dec);
    return f->state;
}
