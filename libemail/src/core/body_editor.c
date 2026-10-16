/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* The full-screen message body editor: libtuikit's tui_textedit with autosave. */
#include "body_editor.h"
#include "key_events.h"
#include "status_line.h"

#include <tuikit/buf.h>
#include <tuikit/canvas.h>
#include <tuikit/event.h>
#include <tuikit/screen.h>
#include <tuikit/textedit.h>
#include <tuikit/tty.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BODY_MAX (1024 * 1024)

/* Below this the editor shows only a notice (tui-interface-model). */
#define MIN_COLS 80
#define MIN_ROWS 24

static void draw(tui_textedit *te, const char *header, const char *note,
                 int rows, int cols) {
    tui_screen *scr = tui_screen_new(rows, cols);
    if (!scr) return;
    tui_canvas c = tui_canvas_of(scr);
    if (rows < MIN_ROWS || cols < MIN_COLS) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Terminal too small (%dx%d).", cols, rows);
        tui_canvas_put(c, 0, 0, msg, TUI_STYLE_DEFAULT);
        snprintf(msg, sizeof(msg), "Needs %dx%d. Resize; Esc saves and leaves.",
                 MIN_COLS, MIN_ROWS);
        if (rows > 1) tui_canvas_put(c, 1, 0, msg, TUI_STYLE_DEFAULT);
        tui_buf out;
        tui_buf_init(&out);
        if (tui_screen_render(scr, &out) >= 0) {
            fflush(stdout);
            tui_buf_write_fd(&out, STDOUT_FILENO);
        }
        tui_buf_free(&out);
        tui_screen_free(scr);
        return;
    }
    tui_canvas_fill(c, tui_rect_make(0, 0, 1, cols), TUI_STYLE_HEADER);
    tui_canvas_put(c, 0, 0, header ? header : "  Message body", TUI_STYLE_HEADER);
    if (rows > 3) {
        tui_canvas ec = tui_canvas_sub(c, tui_rect_make(1, 0, rows - 2, cols));
        tui_textedit_render(te, ec, TUI_STYLE_DEFAULT);
    }
    /* The one status line of the program (EMAIL-111): the hints that fit,
     * the note ("saved", "autosaved") in their place. */
    static const tui_statusline_hint hints[] = {
        { "Esc", "save and continue", SL_PRIO_KEEP },
        { "Ctrl-S", "save", SL_PRIO_HIGH },
    };
    char status[256];
    if (status_line_format(cols, hints, 2, NULL, 0, 0, note, status, sizeof(status)) < 0)
        status[0] = '\0';
    tui_style bar = tui_style_attrs(TUI_ATTR_REVERSE);
    tui_canvas_fill(c, tui_rect_make(rows - 1, 0, 1, cols), bar);
    tui_canvas_put(c, rows - 1, 0, status, bar);

    tui_buf out;
    tui_buf_init(&out);
    if (tui_screen_render(scr, &out) >= 0) {
        fflush(stdout);
        tui_buf_write_fd(&out, STDOUT_FILENO);
    }
    tui_buf_free(&out);
    tui_screen_free(scr);
}

int body_editor_run(char **body, const char *header, BodyEditorSave save, void *ctx) {
    tui_tty_raw *raw = tui_tty_raw_enter(STDIN_FILENO, 0);
    if (!raw) return -1;
    char *buf = malloc(BODY_MAX);
    if (!buf) { tui_tty_raw_exit(&raw); return -1; }
    tui_textedit te;
    tui_textedit_init(&te, buf, BODY_MAX, *body ? *body : "");

    static const char on[]  = "\033[?2004h";
    static const char off[] = "\033[?2004l";
    ssize_t w = write(STDOUT_FILENO, on, sizeof(on) - 1);
    (void)w;

    tui_decoder dec;
    tui_decoder_init(&dec);
    KeyEvents winch;
    key_events_begin(&winch);     /* a resize redraws at once (EMAIL-97) */
    const char *note = "";
    int saved_dirty = 0;
    for (;;) {
        int rows = tui_tty_rows(STDOUT_FILENO), cols = tui_tty_cols(STDOUT_FILENO);
        if (rows <= 0) rows = 24;
        if (cols <= 0) cols = 80;
        draw(&te, header, note, rows, cols);
        note = "";

        /* Idle with unsaved changes: save instead of waiting for a key. */
        tui_event ev;
        int want_idle = (te.dirty && !saved_dirty) ? BODY_EDITOR_AUTOSAVE_MS : -1;
        int rc = key_events_next(STDIN_FILENO, &dec, &ev, want_idle);
        if (rc == 0) {
            if (save) save(te.buf, ctx);
            saved_dirty = 1;
            note = "autosaved";
            continue;
        }
        if (rc < 0 || ev.type == TUI_EV_HANGUP) break;
        if (ev.type == TUI_EV_RESIZE) continue;
        if (ev.type == TUI_EV_KEY && ev.key == TUI_KEY_ESC) break;
        if (rows < MIN_ROWS || cols < MIN_COLS) continue;   /* nothing is drawn: no edits */
        if (TUI_EV_IS_CTRL(&ev, 's')) {
            if (save) save(te.buf, ctx);
            saved_dirty = 1;
            note = "saved";
            continue;
        }
        if (tui_textedit_handle(&te, &ev)) saved_dirty = 0;
    }

    if (save) save(te.buf, ctx);
    free(*body);
    *body = strdup(te.buf);
    w = write(STDOUT_FILENO, off, sizeof(off) - 1);
    (void)w;
    key_events_end(&winch);
    tui_decoder_free(&dec);
    free(buf);
    tui_tty_raw_exit(&raw);
    return *body ? 0 : -1;
}
