/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* One full-screen frame: a header bar and a libtuikit table, written at once. */
#ifndef CORE_TUI_FRAME_H
#define CORE_TUI_FRAME_H

#include <tuikit/table.h>

/**
 * @file tui_frame.h
 * @brief Draws a list view with libtuikit's tui_table (EMAIL-79).
 *
 * A frame is the header bar on row 1, a blank row, the table (column heads,
 * ruler and rows) and nothing below it: the status bar and the information
 * line stay the caller's, on the last two rows.  The frame is built on a
 * tui_screen and written to stdout in one piece, so the program prints
 * nothing else on the rows it covers.
 */

/** Rows the frame leaves free under the table: the
 *  status bar (the information line is printed over the last table row). */
#define TUI_FRAME_BOTTOM_ROWS 1

/**
 * @brief Draws the header bar @p header and @p table on stdout, from the
 *        top-left corner, @p rows x @p cols cells of terminal.
 *
 * The table gets the rows between the header bar and the bottom rows; its
 * scroll position follows the selection (tui_table_render).
 *
 * @param header Text of the header bar (clipped to the width).
 * @param table  The table to draw; its viewport is updated.
 * @param rows   Terminal height.
 * @param cols   Terminal width.
 * @param flags  0, or TUI_FRAME_PLAIN_TITLE.
 * @param subtitle Plain text on the blank row 2 (a hint under the title), or NULL.
 * @param footer   Plain text on the last row (a hint line), or NULL.
 * @return 0, or -1 when the frame could not be built.
 */
int tui_frame_draw_table(const char *header, tui_table *table, int rows, int cols,
                         unsigned flags, const char *subtitle, const char *footer);

/** Frame flag: the first row is a plain title, not the header bar. */
#define TUI_FRAME_PLAIN_TITLE 1u

/**
 * @brief Prints @p text as the header bar (bold white on blue, full width) on
 *        terminal row @p row (1-based), for the screens that are not tables.
 *
 * The text is cut at a display-width boundary and padded to @p cols; the
 * cursor is left after the bar and the style is reset.
 */
void tui_frame_print_bar(int row, const char *text, int cols);

#endif /* CORE_TUI_FRAME_H */
