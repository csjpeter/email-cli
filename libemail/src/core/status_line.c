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
    /* A message never replaces the hints (policies/tui-interface-model.md,
     * EMAIL-285): it goes to the header's notice area.  A line with no hints
     * at all is a prompt or a progress line and shows the text itself. */
    if (message && *message && nhints <= 0)
        tui_statusline_set_message(sl, message, TUI_STYLE_DEFAULT);
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

int status_line_notice_is_error(const char *text) {
    static const char *const words[] = { "rror", "ailed", "annot", "Could not",
                                         "No connection", "refused", "Not a valid", "nvalid", "\xE2\x9C\x97" };
    if (!text) return 0;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (strstr(text, words[i])) return 1;
    return 0;
}

int status_line_notice_format(int tcols, const char *message, char *out, size_t outsz) {
    if (!out || outsz == 0) return -1;
    out[0] = '\0';
    if (!message) return 0;
    while (*message == ' ') message++;
    int max = tcols * 3 / 4;
    if (max < 1) return 0;
    size_t len = 0;
    int cols = 0;
    const unsigned char *p = (const unsigned char *)message;
    while (*p && *p != '\n' && cols < max) {
        size_t cl = 1;
        while ((p[cl] & 0xC0) == 0x80) cl++;
        if (len + cl >= outsz) break;
        memcpy(out + len, p, cl);
        len += cl; p += cl; cols++;
    }
    out[len] = '\0';
    return cols;
}

static int g_header_width;
static int g_notice_row = 2;

void status_line_set_header_width(int cols) { g_header_width = cols; }
void status_line_set_notice_row(int row) { g_notice_row = row; }

void status_line_notice_draw(int tcols, const char *message) {
    char text[512];
    int w = status_line_notice_format(tcols, message, text, sizeof(text));
    if (w <= 0) return;
    int col = tcols - w;                        /* right aligned */
    int row = 1;
    int colour = status_line_notice_is_error(text) ? 91 : 92;
    if (g_header_width > 0 && col < g_header_width + 2) {
        /* No room beside the header text: the free row under it, so that
         * neither the notice nor the header's counts are lost. */
        row = g_notice_row;
        col = 2;
        fprintf(stderr, "\033[%d;1H\033[0m\033[K", row);
        fprintf(stderr, "\033[1;%dm\033[%d;%dH%s\033[0m", colour, row, col + 1, text);
    } else {
        /* Coloured letters on the header's own blue band, never a filled box. */
        fprintf(stderr, "\033[1;%d;44m\033[%d;%dH%s\033[0m", colour, row, col + 1, text);
    }
    fflush(stderr);
}

void status_line_draw(int trows, int tcols,
                      const tui_statusline_hint *hints, int nhints,
                      const char *help_key, int cur, int total,
                      const char *message) {
    if (trows < 1 || tcols < 1) return;
    if (message && *message && nhints > 0) status_line_notice_draw(tcols, message);
    g_header_width = 0;                 /* valid for one frame only */
    g_notice_row = 2;
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
