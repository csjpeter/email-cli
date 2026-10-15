/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-15 */
/* The one bottom line of the full-screen views: libtuikit's tui_statusline. */
#ifndef CORE_STATUS_LINE_H
#define CORE_STATUS_LINE_H

#include <tuikit/statusline.h>

/** @brief Most hints one view passes. */
#define STATUS_LINE_MAX_HINTS 40

/** Key priorities: a higher one is kept longer on a narrow terminal. */
enum { SL_PRIO_LOW = 10, SL_PRIO_MID = 50, SL_PRIO_HIGH = 80, SL_PRIO_KEEP = 100 };

/**
 * @file status_line.h
 * @brief One status line for every view (EMAIL-98).
 *
 * Replaces the former pair of rows (information line + reverse bar) by the
 * libtuikit widget: the last row is the bar, in reverse video.  What it shows
 * is, in this order: the message (until the view's next key), else the hints
 * that fit the width, with the position "[n/N]" right-aligned and always
 * kept, and the help hint when the view has one.
 */

/**
 * @brief Draw the line on the last row (@p trows) of a @p tcols wide terminal.
 *
 * @param trows    Terminal rows (the line is row trows).
 * @param tcols    Terminal columns.
 * @param hints    The view's hints (not copied; only read during the call).
 * @param nhints   Number of hints.
 * @param help_key Key of the help hint ("?" or "h"), NULL when the view has none.
 * @param cur      Position, 1-based; with @p total below 1 no position is shown.
 * @param total    Number of items.
 * @param message  Message that replaces the hints, NULL or "" for none.
 */
void status_line_draw(int trows, int tcols,
                      const tui_statusline_hint *hints, int nhints,
                      const char *help_key, int cur, int total,
                      const char *message);

/**
 * @brief The line as plain text (what status_line_draw() writes, without the
 *        reverse video and the padding); for tests and the help screen.
 * @return Bytes written, -1 when @p outsz was too small.
 */
int status_line_format(int tcols, const tui_statusline_hint *hints, int nhints,
                       const char *help_key, int cur, int total,
                       const char *message, char *out, size_t outsz);

#endif /* CORE_STATUS_LINE_H */
