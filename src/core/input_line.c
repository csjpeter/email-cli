#include "input_line.h"
#include "platform/terminal.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ── UTF-8 helpers ───────────────────────────────────────────────────── */

/** True if byte is a UTF-8 continuation byte (10xxxxxx). */
static int is_cont(unsigned char b) { return (b & 0xC0) == 0x80; }

/** Byte length of the UTF-8 code point starting at s. */
static size_t cp_len(const char *s) {
    unsigned char b = (unsigned char)*s;
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    return 4;
}

/**
 * Display column count for the first `bytes` bytes of s.
 * Uses terminal_wcwidth() for non-ASCII code points;
 * falls back to 1 column per code point if wcwidth returns <=0.
 */
static int display_cols(const char *s, size_t bytes) {
    int cols = 0;
    size_t i = 0;
    while (i < bytes) {
        unsigned char b = (unsigned char)s[i];
        if (b < 0x80) {
            cols++;
            i++;
        } else {
            /* Decode code point */
            uint32_t cp = 0;
            size_t clen = cp_len(s + i);
            if (clen == 2 && i + 1 < bytes)
                cp = ((uint32_t)(b & 0x1F) << 6) | ((unsigned char)s[i+1] & 0x3F);
            else if (clen == 3 && i + 2 < bytes)
                cp = ((uint32_t)(b & 0x0F) << 12)
                   | ((uint32_t)((unsigned char)s[i+1] & 0x3F) << 6)
                   | ((unsigned char)s[i+2] & 0x3F);
            else if (clen == 4 && i + 3 < bytes)
                cp = ((uint32_t)(b & 0x07) << 18)
                   | ((uint32_t)((unsigned char)s[i+1] & 0x3F) << 12)
                   | ((uint32_t)((unsigned char)s[i+2] & 0x3F) << 6)
                   | ((unsigned char)s[i+3] & 0x3F);
            else
                cp = b;  /* malformed — treat as 1 col */
            int w = terminal_wcwidth(cp);
            cols += (w > 0) ? w : 1;
            i += (clen <= bytes - i) ? clen : 1;
        }
    }
    return cols;
}

/* ── Editing operations ──────────────────────────────────────────────── */

static void il_move_left(InputLine *il) {
    if (il->cur == 0) return;
    il->cur--;
    while (il->cur > 0 && is_cont((unsigned char)il->buf[il->cur]))
        il->cur--;
}

static void il_move_right(InputLine *il) {
    if (il->cur >= il->len) return;
    il->cur++;
    while (il->cur < il->len && is_cont((unsigned char)il->buf[il->cur]))
        il->cur++;
}

static void il_backspace(InputLine *il) {
    if (il->cur == 0) return;
    size_t end = il->cur;
    il_move_left(il);
    size_t dlen = end - il->cur;
    memmove(il->buf + il->cur, il->buf + end, il->len - end + 1);
    il->len -= dlen;
}

static void il_delete_fwd(InputLine *il) {
    if (il->cur >= il->len) return;
    size_t start = il->cur;
    size_t end   = start + cp_len(il->buf + start);
    if (end > il->len) end = il->len;
    memmove(il->buf + start, il->buf + end, il->len - end + 1);
    il->len -= end - start;
}

static void il_insert(InputLine *il, char ch) {
    if (il->len + 1 >= il->bufsz) return;
    memmove(il->buf + il->cur + 1, il->buf + il->cur, il->len - il->cur + 1);
    il->buf[il->cur++] = ch;
    il->len++;
}

/* ── Rendering ───────────────────────────────────────────────────────── */

/** Returns the 1-based cursor column for the current insertion point. */
static int il_cursor_col(const InputLine *il, const char *prompt) {
    return 3 /* indent */ + display_cols(prompt, strlen(prompt))
                          + display_cols(il->buf, il->cur);
}

static void il_render(const InputLine *il, int trow, const char *prompt) {
    /* Move to row, clear line, print prompt and text — do NOT position the
     * cursor yet; input_line_run does that after render_below so the cursor
     * always ends up on the input row regardless of what render_below drew. */
    printf("\033[%d;1H\033[2K  %s%s", trow, prompt, il->buf);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void input_line_init(InputLine *il, char *buf, size_t bufsz,
                     const char *initial_text) {
    il->buf           = buf;
    il->bufsz         = bufsz;
    il->trow          = 0;
    il->render_below  = NULL;
    il->tab_fn        = NULL;
    il->shift_tab_fn  = NULL;
    if (initial_text) {
        size_t n = strlen(initial_text);
        if (n >= bufsz) n = bufsz - 1;
        memcpy(buf, initial_text, n);
        buf[n] = '\0';
        il->len = n;
    } else {
        buf[0] = '\0';
        il->len = 0;
    }
    il->cur = il->len;   /* cursor starts at end */
}

int input_line_run(InputLine *il, int trow, const char *prompt) {
    il->trow = trow;
    for (;;) {
        il_render(il, trow, prompt);
        if (il->render_below) il->render_below(il);
        /* Always reposition cursor on the input row after render_below
         * (which may have left the cursor elsewhere). */
        printf("\033[%d;%dH\033[?25h", trow, il_cursor_col(il, prompt));
        fflush(stdout);

        TermKey key = terminal_read_key();
        switch (key) {
        case TERM_KEY_ENTER:
            printf("\033[%d;1H\033[2K\033[?25l", trow + 1);
            fflush(stdout);
            return 1;

        case TERM_KEY_ESC:
        case TERM_KEY_QUIT:
            printf("\033[%d;1H\033[2K\033[?25l", trow + 1);
            fflush(stdout);
            return 0;

        case TERM_KEY_BACK:
            il_backspace(il);
            break;

        case TERM_KEY_DELETE:
            il_delete_fwd(il);
            break;

        case TERM_KEY_LEFT:
            il_move_left(il);
            break;

        case TERM_KEY_RIGHT:
            il_move_right(il);
            break;

        case TERM_KEY_HOME:
            il->cur = 0;
            break;

        case TERM_KEY_END:
            il->cur = il->len;
            break;

        case TERM_KEY_TAB:
            if (il->tab_fn) il->tab_fn(il);
            break;

        case TERM_KEY_SHIFT_TAB:
            if (il->shift_tab_fn) il->shift_tab_fn(il);
            break;

        case TERM_KEY_IGNORE: {
            int ch = terminal_last_printable();
            if (ch > 0) il_insert(il, (char)ch);
            break;
        }

        case TERM_KEY_NEXT_LINE:
        case TERM_KEY_PREV_LINE:
        case TERM_KEY_NEXT_PAGE:
        case TERM_KEY_PREV_PAGE:
            break;  /* ignored in single-line context */
        }
    }
}
