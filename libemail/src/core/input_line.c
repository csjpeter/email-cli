/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-12 */
/* The single-line input loop: libtuikit's line editor on one terminal row. */
#include "input_line.h"
#include "key_events.h"
#include <tuikit/buf.h>
#include <tuikit/event.h>
#include <tuikit/text.h>
#include <tuikit/tty.h>
#include <stdio.h>
#include <unistd.h>

/* The editing is libtuikit's (tui_lineedit_handle); the loop is this file's,
 * because tui_lineedit_run() does not wake for a terminal resize (EMAIL-109):
 * key_events_next() reports SIGWINCH as TUI_EV_RESIZE and the row is drawn
 * again at the new width.  The drawing is the same as tui_lineedit_run()'s,
 * with write(2), while this program's completion hooks print through stdio;
 * the hook of the caller is therefore called through a wrapper that flushes,
 * so its output lands before the cursor is put back on the row. */
static void (*caller_render_below)(const tui_lineedit *);

static void render_below_flushed(const tui_lineedit *le) {
    if (caller_render_below) caller_render_below(le);
    fflush(stdout);
}

/* Bytes and columns of one character of the text at @p pos. */
static uint32_t cp_at(const InputLine *le, size_t pos, int *bytes) {
    uint32_t cp = 0;
    int k = tui_utf8_decode(le->buf + pos, le->len - pos, &cp);
    if (bytes) *bytes = k;
    return cp;
}

/* Adjusts le->scroll so the cursor is inside a field of @p avail columns,
 * with room for it after the last character. */
static void fit_scroll(InputLine *le, int avail) {
    if (avail < 1) avail = 1;
    int c = tui_lineedit_cursor_col(le);
    int total = tui_strn_width(le->buf, le->len);
    if (c < le->scroll) le->scroll = c;
    if (c >= le->scroll + avail) le->scroll = c - avail + 1;
    /* No empty field to the right of a shortened text. */
    if (le->scroll > 0 && total - le->scroll < avail - 1) {
        le->scroll = total - avail + 1;
        if (le->scroll < 0) le->scroll = 0;
        if (c < le->scroll) le->scroll = c;
    }
}

/* The bytes of the text that fall into columns [scroll, scroll + avail),
 * with a leading blank where a wide character is cut by the left edge. */
static void visible_text(const InputLine *le, int avail, tui_buf *out) {
    int col = 0;
    size_t i = 0;
    while (i < le->len && col < le->scroll) {
        int k;
        uint32_t cp = cp_at(le, i, &k);
        col += tui_cp_width(cp) > 0 ? tui_cp_width(cp) : 0;
        i += (size_t)k;
    }
    while (i < le->len && tui_cp_width(cp_at(le, i, NULL)) == 0) {
        int k;
        cp_at(le, i, &k);
        i += (size_t)k;
    }
    int used = 0;
    if (col > le->scroll) {
        tui_buf_puts(out, " ");
        used = 1;
    }
    while (i < le->len) {
        int k;
        uint32_t cp = cp_at(le, i, &k);
        int w = tui_cp_width(cp);
        if (w < 0) w = 1;
        if (used + w > avail) break;
        tui_buf_append(out, le->buf + i, (size_t)k);
        used += w;
        i += (size_t)k;
    }
}

static int g_ended_on_ctrl_c;

int input_line_ctrl_c(void) { return g_ended_on_ctrl_c; }

int input_line_run(InputLine *il, int trow, const char *prompt) {
    g_ended_on_ctrl_c = 0;
    caller_render_below = il->render_below;
    if (caller_render_below) il->render_below = render_below_flushed;
    fflush(stdout);

    /* Column 3: the two-column indent of the screens. */
    const int col = 3;
    const int in_fd = STDIN_FILENO, out_fd = STDOUT_FILENO;
    if (!il->buf || il->size == 0) {
        il->render_below = caller_render_below;
        caller_render_below = NULL;
        return 0;
    }
    if (trow < 1) trow = 1;
    if (!prompt) prompt = "";
    il->trow = trow;

    tui_decoder d;
    tui_decoder_init(&d);
    KeyEvents winch;
    key_events_begin(&winch);
    tui_buf out;
    tui_buf_init(&out);
    int pw = tui_str_width(prompt);
    int rc;
    for (;;) {
        int avail = tui_tty_cols(out_fd) - (col - 1) - pw;
        fit_scroll(il, avail);
        tui_buf_reset(&out);
        tui_buf_printf(&out, "\033[%d;1H\033[2K\033[%d;%dH%s", trow, trow, col, prompt);
        visible_text(il, avail > 0 ? avail : 1, &out);
        if (tui_buf_write_fd(&out, out_fd) != 0) { rc = -1; break; }
        if (il->render_below) il->render_below(il);
        /* render_below may have moved the cursor: put it back on the row. */
        tui_buf_printf(&out, "\033[%d;%dH\033[?25h", trow,
                       col + pw + tui_lineedit_cursor_col(il) - il->scroll);
        if (tui_buf_write_fd(&out, out_fd) != 0) { rc = -1; break; }

        tui_event ev;
        int r = key_events_next(in_fd, &d, &ev, -1);
        if (r <= 0 || ev.type == TUI_EV_HANGUP) { rc = -1; break; }
        if (ev.type == TUI_EV_RESIZE) continue;       /* draw again at the new width */
        tui_edit_result res = tui_lineedit_handle(il, &ev);
        if (TUI_EV_IS_CTRL(&ev, 'c')) g_ended_on_ctrl_c = 1;
        if (res == TUI_EDIT_ACCEPT) { rc = 1; break; }
        if (res == TUI_EDIT_CANCEL) { rc = 0; break; }
    }
    /* The ending: clear the row below (a completion list), hide the cursor. */
    tui_buf_reset(&out);
    tui_buf_printf(&out, "\033[%d;1H\033[2K\033[?25l", trow + 1);
    tui_buf_write_fd(&out, out_fd);
    tui_buf_free(&out);
    key_events_end(&winch);
    tui_decoder_free(&d);

    il->render_below = caller_render_below;
    caller_render_below = NULL;
    tui_lineedit_free(il);   /* the kill buffer; the text stays in the caller's buf */
    return rc == 1;
}
