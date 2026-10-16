/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* One full-screen frame: a header bar and a libtuikit table, written at once. */
#ifndef CORE_TUI_FRAME_H
#define CORE_TUI_FRAME_H

#include <tuikit/canvas.h>
#include <tuikit/dialog.h>
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

/**
 * @brief Draws over what the terminal shows, touching only the cells the
 *        drawing covers (the modals and the drop-down lists, EMAIL-110).
 *
 * @p draw gets a canvas of @p rows x @p cols cells whose cells all start
 * as "not drawn"; whatever it puts or fills is written to the terminal at
 * that position, every other cell is left as it is.  A cell the drawing
 * clears (a blank in the box of a modal) is written, so a box covers all
 * the cells it spans.  Nothing is written for the rows the drawing does not
 * reach, and the terminal cursor is not moved on purpose (callers place it).
 *
 * @return 0, or -1 when the drawing could not be built or written.
 */
int tui_frame_overlay(void (*draw)(tui_canvas c, void *ctx), void *ctx,
                      int rows, int cols);

/**
 * @brief Runs the modal @p d over the screen until it is no longer open:
 *        draws it as an overlay, reads the events (a resize draws it again
 *        at the new size) and gives them to tui_dialog_handle().
 *
 * The terminal must be in raw mode.  The caller redraws its own view
 * afterwards, as the cells under the box are gone.
 *
 * @return TUI_DIALOG_OK or TUI_DIALOG_CANCELLED (also when the input ends).
 */
tui_dialog_state tui_frame_run_dialog(tui_dialog *d);

/**
 * @brief Draws the rows of @p table, without its header, in the rectangle of
 *        @p h rows x @p w columns at terminal row @p row, column @p col
 *        (both 1-based), as an overlay (see tui_frame_overlay()).
 */
int tui_frame_overlay_rows(tui_table *table, int row, int col, int h, int w);

#endif /* CORE_TUI_FRAME_H */
