#ifndef CORE_PATH_COMPLETE_H
#define CORE_PATH_COMPLETE_H

#include "input_line.h"

/**
 * @file path_complete.h
 * @brief Filesystem path Tab-completion for InputLine.
 *
 * Usage:
 *   InputLine il;
 *   input_line_init(&il, buf, sizeof(buf), initial);
 *   path_complete_attach(&il);
 *   int ok = input_line_run(&il, trow, "Save as: ");
 *   path_complete_reset();
 *
 * Behaviour:
 *   Tab        – scan entries matching the path prefix before the cursor,
 *                cycle forward through matches on repeated presses.
 *   Shift+Tab  – cycle backward through the current match list.
 *   Text after the cursor is preserved unchanged.
 *   A one-line completion bar is rendered at trow+1 while active.
 */

/**
 * Attach filesystem path completion callbacks to an InputLine.
 * Sets il->tab_fn, il->shift_tab_fn, and il->render_below.
 */
void path_complete_attach(InputLine *il);

/**
 * Release resources held by the completion state.
 * Must be called after input_line_run() returns.
 */
void path_complete_reset(void);

#endif /* CORE_PATH_COMPLETE_H */
