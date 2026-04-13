#ifndef CORE_INPUT_LINE_H
#define CORE_INPUT_LINE_H

#include <stddef.h>

/**
 * @file input_line.h
 * @brief Single-line interactive text-input widget for terminal TUI.
 *
 * Features:
 *   ← / →        move cursor left / right (by UTF-8 code point)
 *   Home / End   jump to start / end
 *   Backspace    delete character before cursor
 *   Delete       delete character at cursor
 *   Tab          calls optional tab_fn field (e.g. path completion)
 *   Shift+Tab    calls optional shift_tab_fn field (reverse completion)
 *   Enter        confirm → returns 1
 *   ESC / Ctrl-C cancel → returns 0
 *
 * The widget renders on a single terminal row using the real terminal
 * cursor (no reverse-video block).  The caller supplies the buffer and
 * an optional tab-completion callback.
 */

typedef struct InputLine InputLine;

struct InputLine {
    char  *buf;   /**< External buffer owned by the caller.        */
    size_t bufsz; /**< Capacity of buf including NUL terminator.   */
    size_t len;   /**< Current string length in bytes.             */
    size_t cur;   /**< Cursor byte offset (0 … len).               */
    int    trow;  /**< Terminal row (1-based); set by run().        */
    /** Optional hook called after each standard render (e.g. for
     *  drawing a completion list below the input row).  May be NULL. */
    void (*render_below)(const struct InputLine *);
    /** Callback for Tab key (forward completion).  May be NULL. */
    void (*tab_fn)(struct InputLine *);
    /** Callback for Shift+Tab (reverse completion).  May be NULL. */
    void (*shift_tab_fn)(struct InputLine *);
};

/**
 * @brief Initialise an InputLine with an optional initial string.
 *
 * @param il           Widget to initialise.
 * @param buf          Caller-owned character buffer.
 * @param bufsz        Size of buf in bytes (including NUL).
 * @param initial_text Initial content (may be NULL or "").
 */
void input_line_init(InputLine *il, char *buf, size_t bufsz,
                     const char *initial_text);

/**
 * @brief Run the interactive input loop.
 *
 * Renders at terminal row @p trow (1-based).  @p prompt is printed
 * before the editable text.  Completion callbacks are taken from
 * il->tab_fn, il->shift_tab_fn, and il->render_below (all may be NULL).
 *
 * @return 1 if confirmed (Enter), 0 if cancelled (ESC / Ctrl-C).
 */
int input_line_run(InputLine *il, int trow, const char *prompt);

#endif /* CORE_INPUT_LINE_H */
