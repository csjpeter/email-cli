/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-15 */
/* The one bottom line of the full-screen views: libtuikit's tui_statusline. */
#include "status_line.h"
#include "platform/terminal.h"

#include <stdio.h>
#include <string.h>

static void setup(tui_statusline *sl, const tui_statusline_hint *hints, int nhints,
                  const char *help_key, int cur, int total, const char *message) {
    tui_statusline_init(sl);
    if (nhints > STATUS_LINE_MAX_HINTS) nhints = STATUS_LINE_MAX_HINTS;
    tui_statusline_set_hints(sl, hints, nhints);
    if (help_key) tui_statusline_set_help(sl, help_key, "help");
    else          tui_statusline_set_help(sl, NULL, NULL);
    tui_statusline_set_position_index(sl, cur, total);
    if (message && *message) tui_statusline_set_message(sl, message, TUI_STYLE_DEFAULT);
}

int status_line_format(int tcols, const tui_statusline_hint *hints, int nhints,
                       const char *help_key, int cur, int total,
                       const char *message, char *out, size_t outsz) {
    tui_statusline sl;
    setup(&sl, hints, nhints, help_key, cur, total, message);
    int n = tui_statusline_format(&sl, tcols, out, outsz);
    tui_statusline_free(&sl);
    return n;
}

void status_line_draw(int trows, int tcols,
                      const tui_statusline_hint *hints, int nhints,
                      const char *help_key, int cur, int total,
                      const char *message) {
    if (trows < 1 || tcols < 1) return;
    char text[1024];
    if (status_line_format(tcols, hints, nhints, help_key, cur, total,
                           message, text, sizeof(text)) < 0)
        text[sizeof(text) - 1] = '\0';
    /* The text is laid out for tcols columns; pad the rest of the row. */
    int used = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if ((*p & 0xC0) != 0x80) used++;      /* one column per character */
    fprintf(stderr, "\033[%d;1H\033[0m\033[7m%s", trows, text);
    for (int i = used; i < tcols; i++) fputc(' ', stderr);
    fputs("\033[0m", stderr);
    fflush(stderr);
}
