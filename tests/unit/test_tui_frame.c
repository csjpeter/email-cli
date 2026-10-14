/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* Unit tests of the table frame: header bar, table and footer in one write. */
#include "test_helpers.h"
#include "tui_frame.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

static void cell_text(void *ctx, int i, int col, tui_table_cell *c) {
    (void)ctx; (void)col;
    char buf[32];
    snprintf(buf, sizeof(buf), "row %d", i);
    tui_table_cell_put(c, buf, TUI_STYLE_DEFAULT);
}

/* Draws one frame with stdout redirected to a file under the test's scratch
 * directory and returns the bytes (heap) or NULL. */
static char *draw_to_file(const char *header, int count, int rows, int cols,
                          unsigned flags, const char *sub, const char *foot,
                          int *rc_out) {
    char path[512];
    snprintf(path, sizeof(path), "%s", TP("tui_frame_out.txt"));
    int saved = dup(STDOUT_FILENO);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || saved < 0) return NULL;
    dup2(fd, STDOUT_FILENO);
    close(fd);

    static const tui_table_col cols_def[] = { { "Name", 10, 1, TUI_ALIGN_LEFT, 0, 0, 0 } };
    tui_table_model tm = { NULL, cell_text, NULL };
    tui_table t;
    tui_table_init(&t, cols_def, 1, &tm);
    tui_table_set_count(&t, count);
    *rc_out = tui_frame_draw_table(header, &t, rows, cols, flags, sub, foot);

    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = calloc(1, 65536);
    size_t n = buf ? fread(buf, 1, 65535, f) : 0;
    (void)n;
    fclose(f);
    return buf;
}

void test_tui_frame(void) {
    int rc = -1;
    char *out = draw_to_file("  Title here", 3, 24, 80, 0, NULL, NULL, &rc);
    int rc1 = rc;
    int has_title = out && strstr(out, "Title here") != NULL;
    /* A blank cell is skipped by the renderer, so "row 0" is "row" and "0". */
    int nrows = 0;
    for (const char *q = out; q && (q = strstr(q, "row")) != NULL; q += 3) nrows++;
    int has_rows  = nrows == 3;
    int has_head  = out && strstr(out, "Name") != NULL;
    free(out);
    ASSERT(rc1 == 0, "tui_frame: a frame is drawn");
    ASSERT(has_title, "tui_frame: the header text is written");
    ASSERT(has_rows, "tui_frame: the table rows are written");
    ASSERT(has_head, "tui_frame: the column head is written");

    out = draw_to_file("Plain", 1, 24, 80, TUI_FRAME_PLAIN_TITLE,
                       "sub hint", "foot hint", &rc);
    int rc2 = rc;
    int has_hints = out && strstr(out, "sub") && strstr(out, "foot");
    free(out);
    ASSERT(rc2 == 0, "tui_frame: a plain-title frame is drawn");
    ASSERT(has_hints, "tui_frame: subtitle and footer are written");

    out = draw_to_file("x", 1, 3, 80, 0, NULL, NULL, &rc);
    free(out);
    ASSERT(rc == -1, "tui_frame: a terminal under four rows is refused");
    out = draw_to_file("x", 1, 24, 0, 0, NULL, NULL, &rc);
    free(out);
    ASSERT(rc == -1, "tui_frame: zero columns are refused");
}
