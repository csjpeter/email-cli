/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-04-12 */
/* Interface of the single-line input widget: libtuikit's line editor. */
#ifndef CORE_INPUT_LINE_H
#define CORE_INPUT_LINE_H

#include <tuikit/lineedit.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file input_line.h
 * @brief Single-line interactive text-input widget for the terminal UI.
 *
 * The widget is libtuikit's tui_lineedit (EMAIL-28); this header keeps the
 * program's names for it.  Keys: left/right, Home/End, Backspace, Delete,
 * Tab (tab_fn hook, e.g. path completion), Shift+Tab (shift_tab_fn), Enter
 * confirms, Esc/Ctrl-C cancel; the library adds the readline-style editing
 * keys (Ctrl-A/E/K/U/W/Y ...).
 *
 * The editor draws on one terminal row with the real terminal cursor.  The
 * caller supplies the buffer and optional hooks (render_below, tab_fn,
 * shift_tab_fn), set on the fields of the editor after input_line_init().
 */

/** The editor: the fields buf, size, len, cur, trow and the hooks. */
typedef tui_lineedit InputLine;

/**
 * @brief Initialise an editor with an optional initial string.
 *
 * @param il           Editor to initialise.
 * @param buf          Caller-owned character buffer.
 * @param bufsz        Size of buf in bytes (including NUL).
 * @param initial_text Initial content (may be NULL or ""); cursor at the end.
 */
static inline void input_line_init(InputLine *il, char *buf, size_t bufsz,
                                   const char *initial_text) {
    /* Callers edit a value in place and pass the buffer itself as the initial
     * text; the library clears the buffer before inserting, so that text is
     * kept aside first. */
    char *own = (initial_text && buf && initial_text == buf) ? strdup(buf) : NULL;
    tui_lineedit_init(il, buf, bufsz, own ? own : initial_text);
    free(own);
}

/**
 * @brief Run the interactive input loop on terminal row @p trow (1-based).
 *
 * @p prompt is printed after a two-column indent, before the editable text.
 * The terminal must be in raw mode.  Keys typed ahead of the one that ends
 * the loop stay in the input (libtuikit reads one byte at a time).
 *
 * @return 1 if confirmed (Enter), 0 if cancelled (Esc / Ctrl-C) or when the
 *         input ended.
 */
int input_line_run(InputLine *il, int trow, const char *prompt);

/**
 * @brief True when the last input_line_run() ended on ctrl-c (EMAIL-219), so
 *        a caller can quit the program instead of only going back.
 */
int input_line_ctrl_c(void);

#endif /* CORE_INPUT_LINE_H */
