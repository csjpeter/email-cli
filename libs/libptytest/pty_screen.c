/**
 * @file pty_screen.c
 * @brief Virtual screen buffer with VT100 escape sequence parser.
 *
 * Minimal VT100 subset — only what TUI programs typically use:
 *   - Cursor positioning: CSI row;col H, CSI H (home)
 *   - Screen erase: CSI 2J (full), CSI K (to end of line), CSI 2K (full line)
 *   - SGR attributes: 0 (reset), 1 (bold), 2 (dim), 7 (reverse)
 *   - 24-bit colour: CSI 38;2;R;G;Bm (fg), CSI 48;2;R;G;Bm (bg) — parsed, not stored
 *   - Basic colours: CSI 3Xm, CSI 4Xm — parsed, not stored
 *   - Newline, carriage return, backspace, tab
 */

#include "pty_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Screen buffer management ────────────────────────────────────────── */

PtyScreen *pty_screen_new(int cols, int rows) {
    PtyScreen *scr = calloc(1, sizeof(*scr));
    if (!scr) return NULL;
    scr->cols = cols;
    scr->rows = rows;
    scr->cells = calloc((size_t)(cols * rows), sizeof(PtyCell));
    if (!scr->cells) { free(scr); return NULL; }
    /* Initialise cells with spaces */
    for (int i = 0; i < cols * rows; i++) {
        scr->cells[i].ch[0] = ' ';
        scr->cells[i].ch[1] = '\0';
    }
    return scr;
}

void pty_screen_free(PtyScreen *scr) {
    if (!scr) return;
    free(scr->cells);
    free(scr);
}

static PtyCell *cell_at(PtyScreen *scr, int row, int col) {
    if (row < 0 || row >= scr->rows || col < 0 || col >= scr->cols)
        return NULL;
    return &scr->cells[row * scr->cols + col];
}

/* ── Scroll up by one line ───────────────────────────────────────────── */

static void scroll_up(PtyScreen *scr) {
    memmove(&scr->cells[0],
            &scr->cells[scr->cols],
            (size_t)((scr->rows - 1) * scr->cols) * sizeof(PtyCell));
    /* Clear last row */
    for (int c = 0; c < scr->cols; c++) {
        PtyCell *cl = cell_at(scr, scr->rows - 1, c);
        cl->ch[0] = ' '; cl->ch[1] = '\0';
        cl->attr = PTY_ATTR_NONE;
    }
}

/* ── VT100 parser state machine ──────────────────────────────────────── */

/** @brief Parses and applies a CSI sequence ending with the given final byte. */
static void apply_csi(PtyScreen *scr, const char *params, int param_len, char final) {
    /* Parse semicolon-separated integer parameters */
    int args[16] = {0};
    int argc = 0;
    const char *p = params;
    const char *end = params + param_len;
    while (p < end && argc < 16) {
        int val = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        args[argc++] = val;
        if (p < end && *p == ';') p++;
    }

    /* Any CSI sequence cancels pending wrap */
    scr->pending_wrap = 0;

    switch (final) {
    case 'H': case 'f': /* CUP — cursor position */
        scr->cur_row = (argc >= 1 && args[0] > 0) ? args[0] - 1 : 0;
        scr->cur_col = (argc >= 2 && args[1] > 0) ? args[1] - 1 : 0;
        if (scr->cur_row >= scr->rows) scr->cur_row = scr->rows - 1;
        if (scr->cur_col >= scr->cols) scr->cur_col = scr->cols - 1;
        break;

    case 'A': /* CUU — cursor up */
        { int n = (argc >= 1 && args[0] > 0) ? args[0] : 1;
          scr->cur_row -= n;
          if (scr->cur_row < 0) scr->cur_row = 0; }
        break;

    case 'B': /* CUD — cursor down */
        { int n = (argc >= 1 && args[0] > 0) ? args[0] : 1;
          scr->cur_row += n;
          if (scr->cur_row >= scr->rows) scr->cur_row = scr->rows - 1; }
        break;

    case 'C': /* CUF — cursor forward */
        { int n = (argc >= 1 && args[0] > 0) ? args[0] : 1;
          scr->cur_col += n;
          if (scr->cur_col >= scr->cols) scr->cur_col = scr->cols - 1; }
        break;

    case 'D': /* CUB — cursor backward */
        { int n = (argc >= 1 && args[0] > 0) ? args[0] : 1;
          scr->cur_col -= n;
          if (scr->cur_col < 0) scr->cur_col = 0; }
        break;

    case 'J': /* ED — erase display */
        if (args[0] == 2) {
            /* Erase entire display */
            for (int i = 0; i < scr->cols * scr->rows; i++) {
                scr->cells[i].ch[0] = ' ';
                scr->cells[i].ch[1] = '\0';
                scr->cells[i].attr = PTY_ATTR_NONE;
            }
        }
        break;

    case 'K': /* EL — erase in line */
        { int mode = (argc >= 1) ? args[0] : 0;
          int start = (mode == 1 || mode == 2) ? 0 : scr->cur_col;
          int stop  = (mode == 0 || mode == 2) ? scr->cols : scr->cur_col + 1;
          for (int c = start; c < stop; c++) {
              PtyCell *cl = cell_at(scr, scr->cur_row, c);
              if (cl) { cl->ch[0] = ' '; cl->ch[1] = '\0'; cl->attr = PTY_ATTR_NONE; }
          } }
        break;

    case 'm': /* SGR — select graphic rendition */
        for (int i = 0; i < argc; i++) {
            if (args[i] == 0)      scr->cur_attr = PTY_ATTR_NONE;
            else if (args[i] == 1) scr->cur_attr |= PTY_ATTR_BOLD;
            else if (args[i] == 2) scr->cur_attr |= PTY_ATTR_DIM;
            else if (args[i] == 7) scr->cur_attr |= PTY_ATTR_REVERSE;
            /* Skip colour args: 30-37,38,39, 40-47,48,49, 90-97, 100-107 */
            else if (args[i] == 38 || args[i] == 48) {
                /* 38;2;R;G;B or 38;5;N — skip remaining args */
                if (i + 1 < argc && args[i + 1] == 2) i += 4; /* skip 2,R,G,B */
                else if (i + 1 < argc && args[i + 1] == 5) i += 2; /* skip 5,N */
            }
        }
        break;

    default:
        /* Unhandled CSI sequence — ignore */
        break;
    }
}

void pty_screen_feed(PtyScreen *scr, const char *data, size_t len) {
    const char *end = data + len;
    const char *p = data;

    while (p < end) {
        unsigned char ch = (unsigned char)*p;

        if (ch == '\033') {
            /* ESC sequence */
            if (p + 1 < end && p[1] == '[') {
                /* CSI sequence: collect parameters and final byte */
                const char *csi_start = p + 2;
                const char *q = csi_start;
                while (q < end && ((*q >= '0' && *q <= '9') || *q == ';' || *q == '?'))
                    q++;
                if (q < end) {
                    apply_csi(scr, csi_start, (int)(q - csi_start), *q);
                    p = q + 1;
                } else {
                    break; /* Incomplete sequence — wait for more data */
                }
            } else if (p + 1 < end && p[1] == ']') {
                /* OSC sequence: skip until ST (\033\\) or BEL (\007) */
                const char *q = p + 2;
                while (q < end) {
                    if (*q == '\007') { q++; break; }
                    if (*q == '\033' && q + 1 < end && q[1] == '\\') { q += 2; break; }
                    q++;
                }
                p = q;
            } else {
                p++; /* Skip lone ESC */
            }
            continue;
        }

        /* Control characters */
        if (ch == '\n') {
            scr->pending_wrap = 0;
            scr->cur_row++;
            if (scr->cur_row >= scr->rows) {
                scr->cur_row = scr->rows - 1;
                scroll_up(scr);
            }
            p++;
            continue;
        }
        if (ch == '\r') {
            scr->pending_wrap = 0;
            scr->cur_col = 0;
            p++;
            continue;
        }
        if (ch == '\b') {
            scr->pending_wrap = 0;
            if (scr->cur_col > 0) scr->cur_col--;
            p++;
            continue;
        }
        if (ch == '\t') {
            scr->pending_wrap = 0;
            scr->cur_col = (scr->cur_col + 8) & ~7;
            if (scr->cur_col >= scr->cols) scr->cur_col = scr->cols - 1;
            p++;
            continue;
        }
        if (ch < 0x20) {
            p++; /* Skip other control chars */
            continue;
        }

        /* Printable character (ASCII or UTF-8 lead byte) */
        {
            /* Pending wrap: deferred line advance, matching real terminal behaviour */
            if (scr->pending_wrap) {
                scr->pending_wrap = 0;
                scr->cur_col = 0;
                scr->cur_row++;
                if (scr->cur_row >= scr->rows) {
                    scr->cur_row = scr->rows - 1;
                    scroll_up(scr);
                }
            }

            PtyCell *cl = cell_at(scr, scr->cur_row, scr->cur_col);
            if (!cl) { p++; continue; }

            /* Determine UTF-8 byte count */
            int bytes = 1;
            if (ch >= 0xC0 && ch < 0xE0) bytes = 2;
            else if (ch >= 0xE0 && ch < 0xF0) bytes = 3;
            else if (ch >= 0xF0 && ch < 0xF8) bytes = 4;

            if (p + bytes > end) break; /* Incomplete — wait for more */

            int copy = bytes < (int)sizeof(cl->ch) ? bytes : (int)sizeof(cl->ch) - 1;
            memcpy(cl->ch, p, (size_t)copy);
            cl->ch[copy] = '\0';
            cl->attr = scr->cur_attr;

            scr->cur_col++;
            if (scr->cur_col >= scr->cols) {
                /* Pending wrap: stay at last column until next char */
                scr->cur_col = scr->cols - 1;
                scr->pending_wrap = 1;
            }

            p += bytes;
        }
    }
}
