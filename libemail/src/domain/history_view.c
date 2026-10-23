/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-22 */
/* The history screen of the TUI (libtuikit's tui_histview). */
#include "history_view.h"
#include "history_service.h"
#include "key_events.h"
#include "tui_frame.h"
#include "platform/terminal.h"

#include <tuikit/histview.h>
#include <tuikit/canvas.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    tui_histview *view;
    int rows, cols;
} Draw;

static int revert_cb(void *ctx, uint64_t id, char *refusal, size_t size) {
    (void)ctx;
    return history_revert((unsigned long)id, refusal, size);
}

static void draw_view(tui_canvas c, void *ctx) {
    Draw *d = ctx;
    /* Row 1 is the title bar; the view has the rest. */
    tui_canvas sub = tui_canvas_sub(c, tui_rect_make(1, 0, d->rows - 1, d->cols));
    tui_histview_render(d->view, sub);
}

/** Loads the steps to show into @p h and builds the view's rows from them. */
static int load_rows(const Config *cfg, const char *object, OpHistory *h,
                     tui_histview_row **rows, int *n) {
    *rows = NULL;
    *n = 0;
    if (history_load(cfg ? cfg->user : NULL, NULL, h) != 0) return -1;
    if (h->count > 0) {
        *rows = calloc((size_t)h->count, sizeof(**rows));
        if (!*rows) return -1;
    }
    for (int i = 0; i < h->count; i++) {
        const OpStep *s = &h->steps[i];
        if (object ? strcmp(s->object, object) != 0 : !history_shown(s, 1)) continue;
        tui_histview_row *r = &(*rows)[(*n)++];
        r->id = s->id;
        r->time = s->time;
        r->who = s->who;
        r->operation = s->op;
        r->object = s->object;
        r->thing = s->thing;
        r->old_value = s->old_value;
        r->new_value = s->new_value;
        r->reverts = s->reverts;
        r->revertable = history_revertable(h, s);
    }
    return 0;
}

int history_view_run(const Config *cfg, const char *object) {
    tui_histview v;
    if (tui_histview_init(&v, object ? TUI_HISTVIEW_OBJECT : TUI_HISTVIEW_RECENT,
                          revert_cb, NULL) != 0)
        return 0;
    OpHistory h = {0};
    tui_histview_row *rows = NULL;
    int quit = 0;
    int dirty = 1;
    tui_decoder dec;
    tui_decoder_init(&dec);
    KeyEvents winch;
    key_events_begin(&winch);
    char title[512];
    if (object) snprintf(title, sizeof(title), " History of %s", object);
    else        snprintf(title, sizeof(title), " Recent changes");

    for (;;) {
        if (dirty) {
            /* The view keeps pointers into the rows it was given until the
             * next set_rows: free the old ones only after that call. */
            OpHistory nh = {0};
            tui_histview_row *nrows = NULL;
            int nn = 0;
            if (load_rows(cfg, object, &nh, &nrows, &nn) != 0) {
                tui_histview_set_failed(&v, 1);
                op_history_free(&nh);
                free(nrows);
            } else {
                tui_histview_set_rows(&v, nrows, nn);
                op_history_free(&h);
                free(rows);
                h = nh;
                rows = nrows;
            }
            dirty = 0;
        }
        int trows = terminal_rows(), tcols = terminal_cols();
        if (trows <= 0) trows = 24;
        if (tcols <= 0) tcols = 80;
        printf("\033[?25l");
        tui_frame_print_bar(1, title, tcols);
        Draw d = { &v, trows, tcols };
        tui_frame_overlay(draw_view, &d, trows, tcols);
        tui_event ev;
        int r = key_events_next(STDIN_FILENO, &dec, &ev, -1);
        if (r <= 0 || ev.type == TUI_EV_HANGUP) break;
        if (ev.type == TUI_EV_RESIZE) {
            printf("\033[2J");
            continue;
        }
        tui_histview_state st = tui_histview_handle(&v, &ev);
        if (st == TUI_HISTVIEW_BACK) break;
        if (st == TUI_HISTVIEW_QUIT) { quit = 1; break; }
        if (st == TUI_HISTVIEW_REVERTED) dirty = 1;
    }
    key_events_end(&winch);
    tui_decoder_free(&dec);
    tui_histview_free(&v);
    op_history_free(&h);
    free(rows);
    if (quit) terminal_request_quit();
    return quit ? -1 : 0;
}
