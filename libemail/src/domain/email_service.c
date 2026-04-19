#include "email_service.h"
#include "config_store.h"
#include "input_line.h"
#include "path_complete.h"
#include "imap_client.h"
#include "mail_client.h"
#include "gmail_sync.h"
#include "local_store.h"
#include "mime_util.h"
#include "html_render.h"
#include "imap_util.h"
#include "raii.h"
#include "logger.h"
#include "platform/terminal.h"
#include "platform/path.h"
#include "platform/process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

/* ── Column-aware printing ───────────────────────────────────────────── */

/**
 * Print a UTF-8 string left-aligned in exactly `width` terminal columns.
 * Truncates at character boundaries so that the output never exceeds `width`
 * columns, then pads with spaces to reach exactly `width` columns.
 * Uses wcwidth(3) for per-character column measurement (handles multi-byte
 * UTF-8, wide/emoji characters, and combining marks correctly).
 * Requires setlocale(LC_ALL, "") to have been called in main().
 */
static void print_padded_col(const char *s, int width) {
    if (!s) s = "";
    /* width == 0: non-TTY batch mode — print full string, no truncation, no padding */
    if (width <= 0) { fputs(s, stdout); return; }
    const unsigned char *p = (const unsigned char *)s;
    int used = 0;

    while (*p) {
        /* Decode one UTF-8 code point. */
        uint32_t cp;
        int seqlen;
        if      (*p < 0x80) { cp = *p;        seqlen = 1; }
        else if (*p < 0xC2) { cp = 0xFFFD;    seqlen = 1; } /* invalid lead byte */
        else if (*p < 0xE0) { cp = *p & 0x1F; seqlen = 2; }
        else if (*p < 0xF0) { cp = *p & 0x0F; seqlen = 3; }
        else if (*p < 0xF8) { cp = *p & 0x07; seqlen = 4; }
        else                { cp = 0xFFFD;    seqlen = 1; } /* invalid lead byte */

        for (int i = 1; i < seqlen; i++) {
            if ((p[i] & 0xC0) != 0x80) { seqlen = i; cp = 0xFFFD; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }

        int w = terminal_wcwidth(cp);
        if (w == 0) { p += seqlen; continue; }  /* skip control/non-printable */
        if (used + w > width) break;             /* doesn't fit — stop here */

        fwrite(p, 1, (size_t)seqlen, stdout);
        used += w;
        p    += seqlen;
    }

    for (int i = used; i < width; i++) putchar(' ');
}

/** Print n copies of the double-horizontal-bar character ═ (U+2550). */
static void print_dbar(int n) {
    for (int i = 0; i < n; i++) fputs("\xe2\x95\x90", stdout);
}

/**
 * Count extra bytes introduced by multi-byte UTF-8 sequences in s.
 * printf("%-*s", w, s) pads by byte count; adding this value corrects
 * the width for strings containing accented/non-ASCII characters.
 */
static int utf8_extra_bytes(const char *s) {
    int extra = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) == 0x80) extra++;   /* continuation byte */
    return extra;
}

/**
 * Format an integer with space as thousands separator into buf (size >= 16).
 * Returns buf.  Zero → empty string (blank cell).
 */
static char *fmt_thou(char *buf, size_t sz, int n) {
    if (n <= 0) { buf[0] = '\0'; return buf; }
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%d", n);
    int len = (int)strlen(tmp);
    int out = 0;
    for (int i = 0; i < len; i++) {
        int rem = len - i;          /* digits remaining including this one */
        if (i > 0 && rem % 3 == 0)
            buf[out++] = ' ';
        buf[out++] = tmp[i];
    }
    buf[out] = '\0';
    (void)sz;
    return buf;
}

/**
 * Soft-wrap text at word boundaries so no output line exceeds `width`
 * terminal columns (measured by wcwidth).  Long words that exceed `width`
 * are emitted on a line of their own.  Returns a heap-allocated string;
 * caller must free.  Returns strdup(text) on allocation failure.
 */
static char *word_wrap(const char *text, int width) {
    if (!text) return NULL;
    if (width < 20) width = 20;

    size_t in_len = strlen(text);
    /* Hard breaks add one '\n' per `width` chars; space-breaks are net-zero. */
    char *out = malloc(in_len + in_len / (size_t)width + 4);
    if (!out) return strdup(text);
    char *wp = out;

    const char *src = text;
    while (*src) {
        /* Isolate one source line. */
        const char *eol      = strchr(src, '\n');
        const char *line_end = eol ? eol : src + strlen(src);

        /* Emit the source line as one or more width-limited output lines. */
        const char *seg = src;
        while (seg < line_end) {
            const unsigned char *p = (const unsigned char *)seg;
            int col = 0;
            const char *brk = NULL;   /* last candidate break (space) */

            while ((const char *)p < line_end) {
                uint32_t cp; int seqlen;
                if      (*p < 0x80) { cp = *p;        seqlen = 1; }
                else if (*p < 0xC2) { cp = 0xFFFD;    seqlen = 1; }
                else if (*p < 0xE0) { cp = *p & 0x1F; seqlen = 2; }
                else if (*p < 0xF0) { cp = *p & 0x0F; seqlen = 3; }
                else if (*p < 0xF8) { cp = *p & 0x07; seqlen = 4; }
                else                { cp = 0xFFFD;    seqlen = 1; }
                for (int i = 1; i < seqlen; i++) {
                    if ((p[i] & 0xC0) != 0x80) { seqlen = i; cp = 0xFFFD; break; }
                    cp = (cp << 6) | (p[i] & 0x3F);
                }
                if ((const char *)p + seqlen > line_end) break;

                int cw = terminal_wcwidth(cp);
                /* cw is already 0 for non-printable characters */
                if (col + cw > width) break;

                if (*p == ' ') brk = (const char *)p;
                col += cw;
                p   += seqlen;
            }

            const char *chunk_end = (const char *)p;

            if (chunk_end >= line_end) {
                /* Rest of line fits. */
                size_t n = (size_t)(line_end - seg);
                memcpy(wp, seg, n); wp += n;
                seg = line_end;
            } else if (brk) {
                /* Break at last space (replace space with newline). */
                size_t n = (size_t)(brk - seg);
                memcpy(wp, seg, n); wp += n;
                *wp++ = '\n';
                seg = brk + 1;
            } else {
                /* No space found: hard break. */
                size_t n = (size_t)(chunk_end - seg);
                if (n == 0) {
                    /* Single wide char exceeds width: emit it anyway. */
                    const unsigned char *u = (const unsigned char *)seg;
                    int sl = (*u < 0x80) ? 1
                           : (*u < 0xE0) ? 2
                           : (*u < 0xF0) ? 3 : 4;
                    memcpy(wp, seg, (size_t)sl); wp += sl; seg += sl;
                } else {
                    memcpy(wp, seg, n); wp += n;
                    *wp++ = '\n';
                    seg = chunk_end;
                }
            }
        }

        *wp++ = '\n';
        src = eol ? eol + 1 : line_end;
    }
    *wp = '\0';
    return out;
}

/* Forward declaration — defined after visible_line_cols (below). */
static void print_statusbar(int trows, int width, const char *text);
static void show_label_picker(MailClient *mc,
                               const char *uid);
static int is_system_or_special_label(const char *name);

/**
 * Pager prompt for the standalone `show` command.
 * Returns scroll delta: 0 = quit, positive = forward N lines, negative = back N.
 */
static int pager_prompt(int cur_page, int total_pages, int page_size,
                        int term_rows, int sb_width) {
    for (;;) {
        char sb[256];
        snprintf(sb, sizeof(sb),
                 "-- [%d/%d] PgDn/\u2193=scroll  PgUp/\u2191=back  ESC=quit --",
                 cur_page, total_pages);
        print_statusbar(term_rows, sb_width, sb);
        TermKey key = terminal_read_key();
        fprintf(stderr, "\r\033[K");
        fflush(stderr);

        switch (key) {
        case TERM_KEY_QUIT:
        case TERM_KEY_ESC:
        case TERM_KEY_BACK:      return 0;
        case TERM_KEY_NEXT_PAGE: return  page_size;
        case TERM_KEY_PREV_PAGE: return -page_size;
        case TERM_KEY_NEXT_LINE: return  1;
        case TERM_KEY_PREV_LINE: return -1;
        case TERM_KEY_ENTER:
        case TERM_KEY_TAB:
        case TERM_KEY_SHIFT_TAB:
        case TERM_KEY_LEFT:
        case TERM_KEY_RIGHT:
        case TERM_KEY_HOME:
        case TERM_KEY_END:
        case TERM_KEY_DELETE:
        case TERM_KEY_IGNORE:    continue;
        }
    }
}

/** Count newlines in s (= number of lines). */
/**
 * Count visible terminal columns in bytes [p, end), skipping ANSI SGR
 * and OSC escape sequences.  Uses terminal_wcwidth for multi-byte chars.
 */
static int visible_line_cols(const char *p, const char *end) {
    int cols = 0;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        /* Skip ANSI CSI sequence: ESC [ ... final_byte (0x40–0x7E) */
        if (c == 0x1b && p + 1 < end && (unsigned char)*(p + 1) == '[') {
            p += 2;
            while (p < end && ((unsigned char)*p < 0x40 || (unsigned char)*p > 0x7e))
                p++;
            if (p < end) p++;
            continue;
        }
        /* Skip OSC sequence: ESC ] ... BEL  or  ESC ] ... ESC \ */
        if (c == 0x1b && p + 1 < end && (unsigned char)*(p + 1) == ']') {
            p += 2;
            while (p < end) {
                if ((unsigned char)*p == 0x07) { p++; break; }
                if ((unsigned char)*p == 0x1b && p + 1 < end &&
                    (unsigned char)*(p + 1) == '\\') { p += 2; break; }
                p++;
            }
            continue;
        }
        /* Decode one UTF-8 codepoint */
        uint32_t cp; int sl;
        if      (c < 0x80) { cp = c;        sl = 1; }
        else if (c < 0xC2) { cp = 0xFFFD;   sl = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; sl = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; sl = 3; }
        else if (c < 0xF8) { cp = c & 0x07; sl = 4; }
        else               { cp = 0xFFFD;   sl = 1; }
        for (int i = 1; i < sl && p + i < end; i++) {
            if (((unsigned char)p[i] & 0xC0) != 0x80) { sl = i; cp = 0xFFFD; break; }
            cp = (cp << 6) | ((unsigned char)p[i] & 0x3F);
        }
        int w = terminal_wcwidth((wchar_t)cp);
        if (w > 0) cols += w;
        p += sl;
    }
    return cols;
}

/**
 * Count total visual (physical terminal) rows that 'body' occupies when
 * rendered in a terminal of 'term_cols' columns.  A logical line whose
 * visible width exceeds term_cols wraps onto ceil(width/term_cols) rows.
 * Semantics mirror count_lines: each newline-terminated segment plus the
 * final segment (even if empty) each contribute at least 1 visual row.
 */
static int count_visual_rows(const char *body, int term_cols) {
    if (!body || !*body || term_cols <= 0) return 0;
    int total = 0;
    const char *p = body;
    for (;;) {
        const char *eol = strchr(p, '\n');
        const char *seg_end = eol ? eol : (p + strlen(p));
        int cols = visible_line_cols(p, seg_end);
        int rows = (cols == 0 || cols <= term_cols) ? 1
                   : (cols + term_cols - 1) / term_cols;
        total += rows;
        if (!eol) break;
        p = eol + 1;
    }
    return total;
}

/* ── Interactive pager helpers ───────────────────────────────────────── */

/**
 * Print a reverse-video status bar at terminal row trows, exactly width columns wide.
 * text must not contain ANSI escapes that move the cursor off the line.
 */
/**
 * Return a pointer one past the last byte of @p text that still fits in
 * @p max_cols visible columns, skipping ANSI escape sequences.
 * The returned slice can be fputs'd directly; its visible width is <= max_cols.
 */
static const char *text_end_at_cols(const char *text, int max_cols) {
    const char *p = text;
    int cols = 0;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        /* Skip ANSI CSI escape */
        if (c == 0x1b && (unsigned char)*(p + 1) == '[') {
            const char *q = p + 2;
            while (*q && ((unsigned char)*q < 0x40 || (unsigned char)*q > 0x7e))
                q++;
            if (*q) q++;
            p = q;
            continue;
        }
        /* Decode UTF-8 codepoint width */
        uint32_t cp; int sl;
        if      (c < 0x80) { cp = c;        sl = 1; }
        else if (c < 0xC2) { cp = 0xFFFD;   sl = 1; }
        else if (c < 0xE0) { cp = c & 0x1F; sl = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; sl = 3; }
        else if (c < 0xF8) { cp = c & 0x07; sl = 4; }
        else               { cp = 0xFFFD;   sl = 1; }
        for (int i = 1; i < sl && p[i]; i++) {
            if (((unsigned char)p[i] & 0xC0) != 0x80) { sl = i; cp = 0xFFFD; break; }
            cp = (cp << 6) | ((unsigned char)p[i] & 0x3F);
        }
        int w = terminal_wcwidth((wchar_t)cp);
        if (w > 0 && cols + w > max_cols) break;
        if (w > 0) cols += w;
        p += sl;
    }
    return p;
}

static void print_statusbar(int trows, int width, const char *text) {
    fprintf(stderr, "\033[%d;1H\033[7m", trows);
    const char *end = text_end_at_cols(text, width);
    fwrite(text, 1, (size_t)(end - text), stderr);
    int used = visible_line_cols(text, end);
    int pad  = width - used;
    for (int i = 0; i < pad; i++) fputc(' ', stderr);
    fprintf(stderr, "\033[0m");
    fflush(stderr);
}

/**
 * Print a plain (non-reverse) info line at terminal row trows-1.
 * Used as the second-from-bottom status row for persistent informational messages.
 * If text is empty, the line is cleared to blank.
 */
static void print_infoline(int trows, int width, const char *text) {
    fprintf(stderr, "\033[%d;1H\033[0m", trows - 1);
    if (text && *text) {
        fputs(text, stderr);
        int used = visible_line_cols(text, text + strlen(text));
        int pad  = width - used;
        for (int i = 0; i < pad; i++) fputc(' ', stderr);
    } else {
        for (int i = 0; i < width; i++) fputc(' ', stderr);
    }
    fprintf(stderr, "\033[0m");
    fflush(stderr);
}

/**
 * Show a two-column help popup overlay and wait for any key to dismiss.
 *
 * @param title  Title displayed in the popup header.
 * @param rows   Array of {key_label, description} string pairs.
 * @param n      Number of rows.
 */
static void show_help_popup(const char *title,
                            const char *rows[][2], int n) {
    int tcols = terminal_cols();
    int trows = terminal_rows();
    if (tcols <= 0) tcols = 80;
    if (trows <= 0) trows = 24;

    /* Compute popup dimensions */
    int key_col_w = 12;   /* width of the key column */
    int desc_col_w = 44;  /* width of the description column */
    int inner_w = key_col_w + 2 + desc_col_w; /* key + "  " + desc */
    int box_w   = inner_w + 4;  /* "| " + inner + " |" */
    int box_h   = n + 4;        /* title + separator + n rows + bottom border */

    /* Center the popup */
    int col0 = (tcols - box_w) / 2;
    int row0 = (trows - box_h) / 2;
    if (col0 < 1) col0 = 1;
    if (row0 < 1) row0 = 1;

    /* Draw popup using stderr so it overlays stdout content */
    /* Top border */
    fprintf(stderr, "\033[%d;%dH\033[7m", row0, col0);
    fprintf(stderr, "\u250c");
    for (int i = 0; i < box_w - 2; i++) fprintf(stderr, "\u2500");
    fprintf(stderr, "\u2510\033[0m");

    /* Title row */
    fprintf(stderr, "\033[%d;%dH\033[7m\u2502 ", row0 + 1, col0);
    int tlen = (int)strlen(title);
    int pad_left  = (box_w - 4 - tlen) / 2;
    int pad_right = (box_w - 4 - tlen) - pad_left;
    for (int i = 0; i < pad_left;  i++) fputc(' ', stderr);
    fprintf(stderr, "%s", title);
    for (int i = 0; i < pad_right; i++) fputc(' ', stderr);
    fprintf(stderr, " \u2502\033[0m");

    /* Separator */
    fprintf(stderr, "\033[%d;%dH\033[7m\u251c", row0 + 2, col0);
    for (int i = 0; i < box_w - 2; i++) fprintf(stderr, "\u2500");
    fprintf(stderr, "\u2524\033[0m");

    /* Data rows */
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "\033[%d;%dH\033[7m\u2502 ", row0 + 3 + i, col0);
        /* key label — bold, left-padded to key_col_w */
        fprintf(stderr, "\033[1m%-*.*s\033[22m", key_col_w, key_col_w, rows[i][0]);
        fprintf(stderr, "  ");
        /* description — truncated to desc_col_w */
        fprintf(stderr, "%-*.*s", desc_col_w, desc_col_w, rows[i][1]);
        fprintf(stderr, " \u2502\033[0m");
    }

    /* Bottom border */
    fprintf(stderr, "\033[%d;%dH\033[7m\u2514", row0 + 3 + n, col0);
    for (int i = 0; i < box_w - 2; i++) fprintf(stderr, "\u2500");
    fprintf(stderr, "\u2518\033[0m");

    /* Footer: "Press any key to close" */
    const char *footer = " Press any key to close ";
    int flen = (int)strlen(footer);
    if (flen < box_w - 2) {
        int fc = col0 + (box_w - flen) / 2;
        fprintf(stderr, "\033[%d;%dH\033[2m%s\033[0m", row0 + 4 + n, fc, footer);
    }
    fflush(stderr);

    /* Wait for any key */
    terminal_read_key();

    /* Clear the popup area */
    for (int r = row0; r <= row0 + 4 + n; r++) {
        fprintf(stderr, "\033[%d;%dH\033[K", r, col0);
        for (int c = 0; c < box_w; c++) fputc(' ', stderr);
    }
    fflush(stderr);
}

/**
 * ANSI SGR state tracked while scanning skipped body lines.
 * Only the subset emitted by html_render() is handled.
 */
typedef struct {
    int bold, italic, uline, strike;
    int fg_on; int fg_r, fg_g, fg_b;
    int bg_on; int bg_r, bg_g, bg_b;
} AnsiState;

/** Scan bytes [begin, end) for SGR sequences and update *st. */
static void ansi_scan(const char *begin, const char *end, AnsiState *st)
{
    const char *p = begin;
    while (p < end) {
        if (*p != '\033' || p + 1 >= end || *(p+1) != '[') { p++; continue; }
        p += 2;
        char seq[64]; int si = 0;
        while (p < end && *p != 'm' && si < 62) seq[si++] = *p++;
        seq[si] = '\0';
        if (p < end && *p == 'm') p++;
        if      (!strcmp(seq,"0"))   { st->bold=0; st->italic=0; st->uline=0;
                                       st->strike=0; st->fg_on=0; st->bg_on=0; }
        else if (!strcmp(seq,"1"))   { st->bold   = 1; }
        else if (!strcmp(seq,"22"))  { st->bold   = 0; }
        else if (!strcmp(seq,"3"))   { st->italic = 1; }
        else if (!strcmp(seq,"23"))  { st->italic = 0; }
        else if (!strcmp(seq,"4"))   { st->uline  = 1; }
        else if (!strcmp(seq,"24"))  { st->uline  = 0; }
        else if (!strcmp(seq,"9"))   { st->strike = 1; }
        else if (!strcmp(seq,"29"))  { st->strike = 0; }
        else if (!strcmp(seq,"39"))  { st->fg_on  = 0; }
        else if (!strcmp(seq,"49"))  { st->bg_on  = 0; }
        else if (!strncmp(seq,"38;2;",5)) {
            st->fg_on = 1;
            sscanf(seq+5, "%d;%d;%d", &st->fg_r, &st->fg_g, &st->fg_b);
        }
        else if (!strncmp(seq,"48;2;",5)) {
            st->bg_on = 1;
            sscanf(seq+5, "%d;%d;%d", &st->bg_r, &st->bg_g, &st->bg_b);
        }
    }
}

/** Re-emit escapes needed to restore *st on a freshly-reset terminal. */
static void ansi_replay(const AnsiState *st)
{
    if (st->bold)   printf("\033[1m");
    if (st->italic) printf("\033[3m");
    if (st->uline)  printf("\033[4m");
    if (st->strike) printf("\033[9m");
    if (st->fg_on)  printf("\033[38;2;%d;%d;%dm", st->fg_r, st->fg_g, st->fg_b);
    if (st->bg_on)  printf("\033[48;2;%d;%d;%dm", st->bg_r, st->bg_g, st->bg_b);
}

/**
 * Print up to 'vrow_budget' visual rows from 'body', starting at visual
 * row 'from_vrow'.  A logical line whose visible width exceeds 'term_cols'
 * counts as ceil(width/term_cols) visual rows.
 *
 * Replays any ANSI SGR state accumulated in skipped content so that
 * multi-line styled spans remain correct across page boundaries.
 *
 * At least one logical line is always shown even if it alone exceeds the
 * budget (ensures very long URLs are never silently skipped).
 */
static void print_body_page(const char *body, int from_vrow, int vrow_budget,
                             int term_cols) {
    if (!body) return;

    /* ── Skip to from_vrow ──────────────────────────────────────────── */
    const char *p = body;
    int vrow = 0;
    while (*p) {
        const char *eol = strchr(p, '\n');
        const char *seg = eol ? eol : (p + strlen(p));
        int cols = visible_line_cols(p, seg);
        int rows = (cols == 0 || (term_cols > 0 && cols <= term_cols)) ? 1
                   : (cols + term_cols - 1) / term_cols;
        if (vrow + rows > from_vrow) break;   /* this line spans from_vrow */
        vrow += rows;
        p = eol ? eol + 1 : seg;
        if (!eol) break;
    }

    /* Restore ANSI state that was active at the start of the visible region */
    if (p > body) {
        AnsiState st = {0};
        ansi_scan(body, p, &st);
        ansi_replay(&st);
    }

    /* ── Display up to vrow_budget visual rows ───────────────────────── */
    int displayed = 0;
    int any_shown = 0;
    while (*p) {
        const char *eol = strchr(p, '\n');
        const char *seg = eol ? eol : (p + strlen(p));
        int cols = visible_line_cols(p, seg);
        int rows = (cols == 0 || (term_cols > 0 && cols <= term_cols)) ? 1
                   : (cols + term_cols - 1) / term_cols;

        /* Stop when budget exhausted, but always show at least one line */
        if (any_shown && displayed + rows > vrow_budget) break;

        if (eol) {
            printf("%.*s\n", (int)(eol - p), p);
            p = eol + 1;
        } else {
            printf("%s\n", p);
            p += strlen(p);
        }
        displayed += rows;
        any_shown = 1;
    }
}

/* ── Mail client helpers ─────────────────────────────────────────────── */

static MailClient *make_mail(const Config *cfg) {
    return mail_client_connect((Config *)cfg);
}

/* ── Folder status ───────────────────────────────────────────────────── */

typedef struct { int messages; int unseen; int flagged; } FolderStatus;

/** Read total, unseen and flagged counts for each folder/label from local storage.
 *  Instant — no server connection needed.
 *  IMAP: reads per-folder manifests.
 *  Gmail: total from .idx; unseen = L∩UNREAD, flagged = L∩STARRED (both via
 *         merge-join on sorted index files — accurate, no server contact needed).
 *  Returns heap-allocated array; caller must free(). */
static FolderStatus *fetch_all_folder_statuses(const Config *cfg,
                                                char **folders, int count) {
    FolderStatus *st = calloc((size_t)count, sizeof(FolderStatus));
    if (!st || count == 0) return st;
    if (cfg->gmail_mode) {
        /* Load UNREAD and STARRED indexes once; reuse across all labels. */
        char (*unread_uids)[17]  = NULL; int unread_count  = 0;
        char (*starred_uids)[17] = NULL; int starred_count = 0;
        label_idx_load("UNREAD",  &unread_uids,  &unread_count);
        label_idx_load("STARRED", &starred_uids, &starred_count);

        for (int i = 0; i < count; i++) {
            /* TRASH and SPAM use underscore-prefixed local index names. */
            const char *idx_name = folders[i];
            if (strcmp(folders[i], "TRASH") == 0) idx_name = "_trash";
            else if (strcmp(folders[i], "SPAM") == 0) idx_name = "_spam";

            /* User labels have internal IDs ("Label_xxxxxxxx") that differ
             * from their display names ("Felújítás").  .idx files are keyed
             * by ID, so translate display name → ID for the lookup. */
            char *id_alloc = (idx_name == folders[i])
                             ? local_gmail_label_id_lookup(idx_name)
                             : NULL;
            if (id_alloc) idx_name = id_alloc;

            st[i].messages = label_idx_count(idx_name);
            st[i].unseen   = label_idx_intersect_count(idx_name,
                                 (const char (*)[17])unread_uids,  unread_count);
            st[i].flagged  = label_idx_intersect_count(idx_name,
                                 (const char (*)[17])starred_uids, starred_count);
            free(id_alloc);
        }
        free(unread_uids);
        free(starred_uids);
    } else {
        for (int i = 0; i < count; i++)
            manifest_count_folder(folders[i], &st[i].messages,
                                  &st[i].unseen, &st[i].flagged);
    }
    return st;
}

/** Fetches headers or full message for a UID in <folder>.  Caller must free.
 *  Opens a new mail client connection each call.  For bulk fetching (sync), use
 *  a shared connection. */
static char *fetch_uid_content_in(const Config *cfg, const char *folder,
                                  const char *uid, int headers_only) {
    RAII_MAIL MailClient *mc = make_mail(cfg);
    if (!mc) return NULL;
    if (mail_client_select(mc, folder) != 0) return NULL;
    return headers_only ? mail_client_fetch_headers(mc, uid)
                        : mail_client_fetch_body(mc, uid);
}

/* ── Cached header fetch ─────────────────────────────────────────────── */

/** Fetches headers for uid/folder, using the header cache. Caller must free. */
static char *fetch_uid_headers_cached(const Config *cfg, const char *folder,
                                       const char *uid) {
    if (local_hdr_exists(folder, uid))
        return local_hdr_load(folder, uid);
    char *hdrs = fetch_uid_content_in(cfg, folder, uid, 1);
    if (hdrs)
        local_hdr_save(folder, uid, hdrs, strlen(hdrs));
    return hdrs;
}

/**
 * Like fetch_uid_headers_cached but uses an already-connected and folder-selected
 * MailClient instead of opening a new connection.  Falls back to the cache first.
 * Caller must free the returned string.
 */
static char *fetch_uid_headers_via(MailClient *mc, const char *folder, const char *uid) {
    if (local_hdr_exists(folder, uid))
        return local_hdr_load(folder, uid);
    char *hdrs = mail_client_fetch_headers(mc, uid);
    if (hdrs)
        local_hdr_save(folder, uid, hdrs, strlen(hdrs));
    return hdrs;
}

/* ── Show helpers ────────────────────────────────────────────────────── */

#define SHOW_WIDTH 80
#define SHOW_SEPARATOR \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n"

/*
 * Print s (cleaning control chars), truncating at max_cols display columns.
 * Falls back to `fallback` if s is NULL.  Uses terminal_wcwidth for accurate
 * multi-byte / wide-character measurement.
 */
static void print_clean(const char *s, const char *fallback, int max_cols) {
    const unsigned char *p = (const unsigned char *)(s ? s : fallback);
    int col = 0;
    while (*p) {
        uint32_t cp; int sl;
        if      (*p < 0x80) { cp = *p;        sl = 1; }
        else if (*p < 0xC2) { cp = 0xFFFD;    sl = 1; }
        else if (*p < 0xE0) { cp = *p & 0x1F; sl = 2; }
        else if (*p < 0xF0) { cp = *p & 0x0F; sl = 3; }
        else if (*p < 0xF8) { cp = *p & 0x07; sl = 4; }
        else                { cp = 0xFFFD;    sl = 1; }
        for (int i = 1; i < sl; i++) {
            if ((p[i] & 0xC0) != 0x80) { sl = i; cp = 0xFFFD; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        int w = terminal_wcwidth(cp);
        if (w < 0) w = 0;
        if (col + w > max_cols) break;
        if (cp < 0x20 && cp != '\t') putchar(' ');
        else fwrite(p, 1, (size_t)sl, stdout);
        col += w;
        p   += sl;
    }
}

static void print_show_headers(const char *from, const char *subject,
                                const char *date, const char *labels) {
    /* label = 9 chars ("From:    "), remaining = SHOW_WIDTH - 9 = 71 */
    printf("From:    "); print_clean(from,    "(none)", SHOW_WIDTH - 9); putchar('\n');
    printf("Subject: "); print_clean(subject, "(none)", SHOW_WIDTH - 9); putchar('\n');
    printf("Date:    "); print_clean(date,    "(none)", SHOW_WIDTH - 9); putchar('\n');
    if (labels && labels[0])
        printf("Labels:  %s\n", labels);
    printf(SHOW_SEPARATOR);
}

/* ── Attachment picker ───────────────────────────────────────────────── */


/* Determine the best directory to save attachments into.
 * Prefers ~/Downloads if it exists, else falls back to ~.
 * Returns a heap-allocated string the caller must free(). */
static char *attachment_save_dir(void) {
    const char *home = platform_home_dir();
    if (!home) return strdup(".");
    char dl[1024];
    snprintf(dl, sizeof(dl), "%s/Downloads", home);
    struct stat st;
    if (stat(dl, &st) == 0 && S_ISDIR(st.st_mode))
        return strdup(dl);
    return strdup(home);
}

/* Sanitise a filename component for use in a path (strip path separators). */
static char *safe_filename_for_path(const char *name) {
    if (!name || !*name) return strdup("attachment");
    char *s = strdup(name);
    if (!s) return NULL;
    for (char *p = s; *p; p++)
        if (*p == '/' || *p == '\\') *p = '_';
    return s;
}

/* Attachment picker: full-screen list, navigate with arrows, Enter to select.
 * Returns selected index (0-based), or -1 if Backspace (back), -2 if ESC/Quit. */
static int show_attachment_picker(const MimeAttachment *atts, int count,
                                  int tcols, int trows) {
    int cursor = 0;
    for (;;) {
        printf("\033[0m\033[H\033[2J");
        printf("  Attachments (%d):\n\n", count);
        for (int i = 0; i < count; i++) {
            const char *name  = atts[i].filename     ? atts[i].filename     : "(no name)";
            const char *ctype = atts[i].content_type ? atts[i].content_type : "";
            char sz[32];
            if (atts[i].size >= 1024 * 1024)
                snprintf(sz, sizeof(sz), "%.1f MB",
                         (double)atts[i].size / (1024.0 * 1024.0));
            else if (atts[i].size >= 1024)
                snprintf(sz, sizeof(sz), "%.0f KB",
                         (double)atts[i].size / 1024.0);
            else
                snprintf(sz, sizeof(sz), "%zu B", atts[i].size);

            if (i == cursor)
                printf("  \033[7m> %-36s  %-28s  %8s\033[0m\n", name, ctype, sz);
            else
                printf("    %-36s  %-28s  %8s\n", name, ctype, sz);
        }
        fflush(stdout);
        char sb[160];
        snprintf(sb, sizeof(sb),
                 "  \u2191\u2193=select  Enter=choose  Backspace=back  ESC=quit");
        print_statusbar(trows, tcols, sb);

        TermKey key = terminal_read_key();
        switch (key) {
        case TERM_KEY_BACK:    return -1;
        case TERM_KEY_ESC:
        case TERM_KEY_QUIT:    return -2;
        case TERM_KEY_ENTER:   return cursor;
        case TERM_KEY_NEXT_LINE:
        case TERM_KEY_NEXT_PAGE:
            if (cursor < count - 1) cursor++;
            break;
        case TERM_KEY_PREV_LINE:
        case TERM_KEY_PREV_PAGE:
            if (cursor > 0) cursor--;
            break;
        default: break;
        }
    }
}

/**
 * Show a message in interactive pager mode.
 * Returns 0 = back to list (Backspace/ESC/q), -1 = error.
 */
static int show_uid_interactive(const Config *cfg, const char *folder,
                                const char *uid, int page_size) {
    char *raw = NULL;
    if (local_msg_exists(folder, uid)) {
        raw = local_msg_load(folder, uid);
    } else {
        raw = fetch_uid_content_in(cfg, folder, uid, 0);
        if (raw) {
            local_msg_save(folder, uid, raw, strlen(raw));
            local_index_update(folder, uid, raw);
        }
    }
    if (!raw) {
        fprintf(stderr, "Could not load UID %s.\n", uid);
        return -1;
    }

    char *from_raw = mime_get_header(raw, "From");
    char *from     = from_raw ? mime_decode_words(from_raw) : NULL;
    free(from_raw);
    char *subj_raw = mime_get_header(raw, "Subject");
    char *subject  = subj_raw ? mime_decode_words(subj_raw) : NULL;
    free(subj_raw);
    char *date_raw = mime_get_header(raw, "Date");
    char *date     = date_raw ? mime_format_date(date_raw) : NULL;
    free(date_raw);
    /* Gmail: load labels from .hdr cache for display in reader header */
    char *show_labels = cfg->gmail_mode ? local_hdr_get_labels("", uid) : NULL;
    int term_cols = terminal_cols();
    int term_rows = terminal_rows();
    if (term_rows <= 0) term_rows = page_size;
    int wrap_cols = term_cols > SHOW_WIDTH ? SHOW_WIDTH : term_cols;
    char *body = NULL;
    char *html_raw = mime_get_html_part(raw);
    if (html_raw) {
        body = html_render(html_raw, wrap_cols, 1);
        free(html_raw);
    } else {
        char *plain = mime_get_text_body(raw);
        if (plain) {
            char *wrapped = word_wrap(plain, wrap_cols);
            if (wrapped) { free(plain); body = wrapped; }
            else body = plain;
        }
    }
    const char *body_text = body ? body : "(no readable text body)";
    char *body_wrapped = NULL; /* kept for free() at cleanup */

    /* Detect attachments once */
    int att_count = 0;
    MimeAttachment *atts = mime_list_attachments(raw, &att_count);

/* Header: From+Subject+Date+separator = 4 rows; +1 if Labels line present.
 * Footer: info line (trows-1) + statusbar (trows) = 2 rows. */
#define SHOW_HDR_LINES_INT 6  /* kept for #undef below */
    int hdr_rows    = (show_labels && show_labels[0]) ? 5 : 4;
    int rows_avail  = (term_rows > hdr_rows + 2) ? term_rows - hdr_rows - 2 : 1;
    int body_vrows  = count_visual_rows(body_text, term_cols);
    int total_pages = (body_vrows + rows_avail - 1) / rows_avail;
    if (total_pages < 1) total_pages = 1;

    /* Persistent info message — stays until replaced by a newer one */
    char info_msg[2048] = "";

    int result = 0;
    for (int cur_line = 0;;) {
        printf("\033[0m\033[H\033[2J");     /* reset attrs + clear screen */
        print_show_headers(from, subject, date, show_labels);
        print_body_page(body_text, cur_line, rows_avail, term_cols);
        printf("\033[0m");                  /* close any open ANSI from body */
        fflush(stdout);

        int cur_page = cur_line / rows_avail + 1;

        /* Info line (second from bottom) — persistent until overwritten */
        print_infoline(term_rows, wrap_cols, info_msg);

        /* Shortcut hints (bottom row) */
        {
            char sb[256];
            if (att_count > 0) {
                snprintf(sb, sizeof(sb),
                         "-- [%d/%d] PgDn/\u2193=scroll  PgUp/\u2191=back"
                         "  r=reply  a=save  A=save-all(%d)  Backspace/ESC/q=list --",
                         cur_page, total_pages, att_count);
            } else {
                snprintf(sb, sizeof(sb),
                         "-- [%d/%d] PgDn/\u2193=scroll  PgUp/\u2191=back"
                         "  r=reply  Backspace/ESC/q=list --",
                         cur_page, total_pages);
            }
            print_statusbar(term_rows, wrap_cols, sb);
        }

        TermKey key = terminal_read_key();
        fprintf(stderr, "\r\033[K");
        fflush(stderr);

        switch (key) {
        case TERM_KEY_BACK:
        case TERM_KEY_ESC:
        case TERM_KEY_QUIT:
            result = 0;          /* back to list */
            goto show_int_done;
        case TERM_KEY_NEXT_PAGE:
        {
            int next = cur_line + rows_avail;
            if (next < body_vrows) cur_line = next;
        }
            break;
        case TERM_KEY_ENTER:
            break;
        case TERM_KEY_PREV_PAGE:
            cur_line -= rows_avail;
            if (cur_line < 0) cur_line = 0;
            break;
        case TERM_KEY_NEXT_LINE:
            if (cur_line < body_vrows - 1) cur_line++;
            break;
        case TERM_KEY_PREV_LINE:
            if (cur_line > 0) cur_line--;
            break;
        case TERM_KEY_HOME:
            cur_line = 0;
            break;
        case TERM_KEY_END:
            cur_line = body_vrows > rows_avail ? body_vrows - rows_avail : 0;
            break;
        case TERM_KEY_LEFT:
        case TERM_KEY_RIGHT:
        case TERM_KEY_DELETE:
        case TERM_KEY_TAB:
        case TERM_KEY_SHIFT_TAB:
        case TERM_KEY_IGNORE: {
            int ch = terminal_last_printable();
            if (ch == 'r') {
                result = 2;      /* reply to this message */
                goto show_int_done;
            } else if (ch == 'q') {
                result = 0;      /* back to list */
                goto show_int_done;
            } else if (ch == 'h' || ch == '?') {
                static const char *help[][2] = {
                    { "PgDn / \u2193",   "Scroll down one page / one line"  },
                    { "PgUp / \u2191",   "Scroll up one page / one line"    },
                    { "Home / End",      "Jump to top / bottom of message"  },
                    { "r",              "Reply to this message"             },
                    { "a",              "Save an attachment"                },
                    { "A",              "Save all attachments"              },
                    { "Backspace",      "Back to message list"              },
                    { "ESC / q",        "Back to message list"              },
                    { "h / ?",          "Show this help"                    },
                };
                show_help_popup("Message reader shortcuts",
                                help, (int)(sizeof(help)/sizeof(help[0])));
                break;
            } else if (ch == 'a' && att_count > 0) {
                int sel = 0;
                if (att_count > 1) {
                    sel = show_attachment_picker(atts, att_count,
                                                 term_cols, term_rows);
                    if (sel == -2) {
                        break;       /* ESC/q → back to show view */
                    }
                    if (sel < 0) break;  /* Backspace → back to show */
                }
                /* Build suggested path and let user edit it */
                {
                    char *dir  = attachment_save_dir();
                    char *fname = safe_filename_for_path(atts[sel].filename);
                    char dest[2048];
                    snprintf(dest, sizeof(dest), "%s/%s",
                             dir ? dir : ".", fname ? fname : "attachment");
                    free(dir);
                    free(fname);
                    InputLine il;
                    input_line_init(&il, dest, sizeof(dest), dest);
                    path_complete_attach(&il);
                    int ok = input_line_run(&il, term_rows - 1, "Save as: ");
                    path_complete_reset();
                    /* Clear the edited line and the completion row */
                    printf("\033[%d;1H\033[2K\033[%d;1H\033[2K\033[?25l",
                           term_rows - 1, term_rows);
                    if (ok == 1) {
                        int r = mime_save_attachment(&atts[sel], dest);
                        snprintf(info_msg, sizeof(info_msg),
                                 r == 0 ? "  Saved: %.1900s"
                                        : "  Save FAILED: %.1900s", dest);
                    }
                }
            } else if (ch == 'A' && att_count > 0) {
                /* Save ALL attachments to a chosen directory */
                char *def_dir = attachment_save_dir();
                char dest_dir[2048];
                snprintf(dest_dir, sizeof(dest_dir), "%s",
                         def_dir ? def_dir : ".");
                free(def_dir);
                InputLine il;
                input_line_init(&il, dest_dir, sizeof(dest_dir), dest_dir);
                path_complete_attach(&il);
                int ok = input_line_run(&il, term_rows - 1, "Save all to: ");
                path_complete_reset();
                /* Clear the edited line and the completion row */
                printf("\033[%d;1H\033[2K\033[%d;1H\033[2K\033[?25l",
                       term_rows - 1, term_rows);
                if (ok == 1) {
                    int saved = 0;
                    for (int i = 0; i < att_count; i++) {
                        char *fname = safe_filename_for_path(atts[i].filename);
                        char fpath[4096];
                        snprintf(fpath, sizeof(fpath), "%s/%s",
                                 dest_dir, fname ? fname : "attachment");
                        free(fname);
                        if (mime_save_attachment(&atts[i], fpath) == 0)
                            saved++;
                    }
                    snprintf(info_msg, sizeof(info_msg),
                             saved == att_count
                             ? "  Saved %d/%d files to: %.1900s"
                             : "  Saved %d/%d (errors) to: %.1900s",
                             saved, att_count, dest_dir);
                }
            }
            break;
        }
        }
    }
show_int_done:
#undef SHOW_HDR_LINES_INT
    mime_free_attachments(atts, att_count);
    free(body); free(body_wrapped); free(from); free(subject); free(date); free(show_labels); free(raw);
    return result;
}

/* ── List helpers ────────────────────────────────────────────────────── */

typedef struct { char uid[17]; int flags; time_t epoch; } MsgEntry;

/* Parse "YYYY-MM-DD HH:MM" (manifest date format) to time_t in local time.
 * Returns 0 on failure. */
static time_t parse_manifest_date(const char *d) {
    if (!d || !*d) return 0;
    struct tm tm = {0};
    if (sscanf(d, "%d-%d-%d %d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min) != 5) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* Return 1 if a background sync process is currently running. */
static int sync_is_running(void) {
    const char *cache_base = platform_cache_dir();
    if (!cache_base) return 0;
    char pid_path[2048];
    snprintf(pid_path, sizeof(pid_path), "%s/email-cli/sync.pid", cache_base);
    RAII_FILE FILE *pf = fopen(pid_path, "r");
    if (!pf) return 0;
    int pid = 0;
    if (fscanf(pf, "%d", &pid) != 1) pid = 0;
    if (pid <= 0) return 0;
    /* Accept any of the binary names that may be running sync */
    return platform_pid_is_program((pid_t)pid, "email-cli") ||
           platform_pid_is_program((pid_t)pid, "email-sync") ||
           platform_pid_is_program((pid_t)pid, "email-tui");
}

/* Build path to email-sync binary (same directory as the running binary). */
static void get_sync_bin_path(char *buf, size_t size) {
    snprintf(buf, size, "email-sync"); /* fallback: PATH lookup */
    char self[1024] = {0};
    if (platform_executable_path(self, sizeof(self)) == 0) {
        char *slash = strrchr(self, '/');
        if (slash)
            snprintf(buf, size, "%.*s/email-sync", (int)(slash - self), self);
    }
}

/* Set by the SIGCHLD handler when the background sync child exits. */
static volatile sig_atomic_t bg_sync_done = 0;
static pid_t bg_sync_pid = -1;

static void bg_sync_sigchld(int sig) {
    (void)sig;
    int status;
    pid_t p;
    /* Reap all children; only bg_sync_pid triggers bg_sync_done. */
    while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
        if (p == bg_sync_pid) {
            bg_sync_pid = -1;
            bg_sync_done = 1;
        }
    }
}

/**
 * Fork a minimal child to push a single flag change to the mail server.
 * The parent returns immediately; the child connects, sets the flag, and exits.
 * The pending queue already records the change so failures are retried on sync.
 */
static void flag_push_background(const Config *cfg, const char *uid,
                                  const char *flag_name, int add_flag) {
    /* Ensure SIGCHLD is handled so the child is reaped without polling. */
    struct sigaction sa = {0};
    sa.sa_handler = bg_sync_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGCHLD, &sa, NULL);

    pid_t pid = fork();
    if (pid < 0) return; /* fork failed; pending queue retries on next sync */
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        MailClient *mc = make_mail(cfg);
        if (mc) {
            mail_client_set_flag(mc, uid, flag_name, add_flag);
            mail_client_free(mc);
        }
        _exit(0);
    }
    /* Parent continues; child reaped by bg_sync_sigchld. */
}

/**
 * Fork and exec email-sync in the background.
 * Installs a SIGCHLD handler (without SA_RESTART) so the blocked read() in
 * terminal_read_key() is interrupted when the child exits — this lets the TUI
 * react immediately without any polling.
 * Returns 1 if the child was spawned, 0 if already running, -1 on error.
 */
static int sync_start_background(void) {
    if (sync_is_running()) return 0;

    struct sigaction sa = {0};
    sa.sa_handler = bg_sync_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: read() must be interrupted on SIGCHLD */
    sigaction(SIGCHLD, &sa, NULL);

    char sync_bin[1024];
    get_sync_bin_path(sync_bin, sizeof(sync_bin));

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child: detach from the TUI session */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        char *args[] = {sync_bin, NULL};
        execvp(sync_bin, args);
        _exit(1); /* exec failed */
    }
    bg_sync_pid = pid;
    return 1;
}

/* Sort group: 0=unseen, 1=flagged (read), 2=rest */
static int msg_group(int flags) {
    if (flags & MSG_FLAG_UNSEEN)  return 0;
    if (flags & MSG_FLAG_FLAGGED) return 1;
    return 2;
}

static int cmp_uid_entry(const void *a, const void *b) {
    const MsgEntry *ea = a, *eb = b;
    int ga = msg_group(ea->flags);
    int gb = msg_group(eb->flags);
    if (ga != gb) return ga - gb;         /* group order: unseen, flagged, rest */
    /* Within group: newer date first; fall back to UID if date unavailable */
    if (eb->epoch != ea->epoch) return (eb->epoch > ea->epoch) ? 1 : -1;
    return strcmp(eb->uid, ea->uid);
}

/* ── Folder list helpers ─────────────────────────────────────────────── */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ── Folder tree renderer ────────────────────────────────────────────── */

/**
 * Returns 1 if names[i] is the last child of its parent in the sorted list.
 * Skips descendants of names[i] before checking for siblings.
 */
static int is_last_sibling(char **names, int count, int i, char sep) {
    const char *name = names[i];
    size_t name_len  = strlen(name);
    const char *lsep = strrchr(name, sep);
    size_t parent_len = lsep ? (size_t)(lsep - name) : 0;

    /* Find the last position that belongs to names[i]'s subtree */
    int last = i;
    for (int j = i + 1; j < count; j++) {
        if (strncmp(names[j], name, name_len) == 0 &&
            (names[j][name_len] == sep || names[j][name_len] == '\0'))
            last = j;
        else
            break;
    }

    /* After the subtree, look for a sibling */
    for (int j = last + 1; j < count; j++) {
        if (parent_len == 0)
            return 0; /* any following item is a root-level sibling */
        if (strlen(names[j]) > parent_len &&
            strncmp(names[j], name, parent_len) == 0 &&
            names[j][parent_len] == sep)
            return 0;
        return 1; /* jumped to a different parent subtree */
    }
    return 1;
}

/**
 * Returns 1 if the ancestor of names[i] at indent-level 'level'
 * (0 = root component) is the last child of its own parent.
 */
static int ancestor_is_last(char **names, int count, int i,
                             int level, char sep) {
    const char *name = names[i];

    /* ancestor prefix length: (level+1) components */
    size_t anc_len = 0;
    int sep_cnt = 0;
    while (name[anc_len]) {
        if (name[anc_len] == sep && sep_cnt++ == level) break;
        anc_len++;
    }

    /* parent prefix length: level components */
    size_t parent_len = 0;
    sep_cnt = 0;
    for (size_t k = 0; k < anc_len; k++) {
        if (name[k] == sep) {
            if (sep_cnt++ == level - 1) { parent_len = k; break; }
        }
    }
    if (level == 0) parent_len = 0;

    /* Last item in ancestor's subtree */
    int last = i;
    for (int j = i + 1; j < count; j++) {
        if (strncmp(names[j], name, anc_len) == 0 &&
            (names[j][anc_len] == sep || names[j][anc_len] == '\0'))
            last = j;
        else
            break;
    }

    /* After subtree, look for sibling of ancestor */
    for (int j = last + 1; j < count; j++) {
        if (parent_len == 0)
            return 0; /* another root-level item */
        if (strlen(names[j]) > parent_len &&
            strncmp(names[j], name, parent_len) == 0 &&
            names[j][parent_len] == sep)
            return 0;
        return 1;
    }
    return 1;
}

/** Returns 1 if folder `name` has any direct or indirect children. */
static int folder_has_children(char **names, int count, const char *name, char sep) {
    size_t len = strlen(name);
    for (int i = 0; i < count; i++)
        if (strncmp(names[i], name, len) == 0 && names[i][len] == sep)
            return 1;
    return 0;
}

/** Sum unseen/flagged/messages for a folder and all its descendants. */
static void sum_subtree(char **names, int count, char sep,
                        const char *prefix, const FolderStatus *statuses,
                        int *msgs_out, int *unseen_out, int *flagged_out) {
    size_t plen = strlen(prefix);
    int msgs = 0, unseen = 0, flagged = 0;
    for (int i = 0; i < count; i++) {
        const char *n = names[i];
        if (strcmp(n, prefix) == 0 ||
            (strncmp(n, prefix, plen) == 0 && n[plen] == sep)) {
            msgs   += statuses ? statuses[i].messages : 0;
            unseen += statuses ? statuses[i].unseen   : 0;
            flagged+= statuses ? statuses[i].flagged  : 0;
        }
    }
    *msgs_out   = msgs;
    *unseen_out = unseen;
    *flagged_out= flagged;
}

/**
 * Build filtered index (into names[]) of direct children of `prefix`.
 * prefix="" means root level (folders with no sep in their name).
 * Returns number of visible entries written into vis_out[].
 */
static int build_flat_view(char **names, int count, char sep,
                           const char *prefix, int *vis_out) {
    int vcount = 0;
    size_t plen = strlen(prefix);
    for (int i = 0; i < count; i++) {
        const char *name = names[i];
        if (plen == 0) {
            if (strchr(name, sep) == NULL)
                vis_out[vcount++] = i;
        } else {
            if (strncmp(name, prefix, plen) == 0 && name[plen] == sep &&
                strchr(name + plen + 1, sep) == NULL)
                vis_out[vcount++] = i;
        }
    }
    return vcount;
}

/** Print one folder item with its tree/flat prefix and optional selection highlight. */
/* Flat mode column layout: Unread | Flagged | Folder | Total
 * name_w: width of the folder name column (ignored in tree mode).
 * flagged: number of flagged messages (0 = blank cell). */
static void print_folder_item(char **names, int count, int i, char sep,
                               int tree_mode, int selected, int has_kids,
                               int messages, int unseen, int flagged, int name_w) {
    if (selected)
        printf("\033[7m");
    else if (messages == 0)
        printf("\033[2m");          /* dim: empty folder */

    if (tree_mode) {
        /* Build "tree-prefix + component-name" into name_buf for column layout */
        char name_buf[512];
        int pos = 0;
        int depth = 0;
        for (const char *p = names[i]; *p; p++)
            if (*p == sep) depth++;
        for (int lv = 0; lv < depth; lv++) {
            int anc_last = ancestor_is_last(names, count, i, lv, sep);
            const char *branch = anc_last ? "    " : "\u2502   ";
            int blen = (int)strlen(branch);
            if (pos + blen < (int)sizeof(name_buf) - 1) {
                memcpy(name_buf + pos, branch, blen);
                pos += blen;
            }
        }
        int last = is_last_sibling(names, count, i, sep);
        const char *conn = last ? "\u2514\u2500\u2500 " : "\u251c\u2500\u2500 ";
        int clen = (int)strlen(conn);
        if (pos + clen < (int)sizeof(name_buf) - 1) {
            memcpy(name_buf + pos, conn, clen);
            pos += clen;
        }
        const char *comp = strrchr(names[i], sep);
        snprintf(name_buf + pos, sizeof(name_buf) - pos, "%s",
                 comp ? comp + 1 : names[i]);
        char u[16], f[16], t[16];
        fmt_thou(u, sizeof(u), unseen);
        fmt_thou(f, sizeof(f), flagged);
        fmt_thou(t, sizeof(t), messages);
        printf("  %6s  %7s  %-*s  %7s", u, f,
               name_w + utf8_extra_bytes(name_buf), name_buf, t);
    } else {
        /* Flat mode: Unread | Flagged | Folder | Total */
        const char *comp    = strrchr(names[i], sep);
        const char *display = comp ? comp + 1 : names[i];
        char name_buf[256];
        snprintf(name_buf, sizeof(name_buf), "%s%s", display, has_kids ? "/" : "");
        char u[16], f[16], t[16];
        fmt_thou(u, sizeof(u), unseen);
        fmt_thou(f, sizeof(f), flagged);
        fmt_thou(t, sizeof(t), messages);
        printf("  %6s  %7s  %-*s  %7s", u, f,
               name_w + utf8_extra_bytes(name_buf), name_buf, t);
    }

    if (selected) printf("\033[K\033[0m");
    else if (messages == 0) printf("\033[0m");
    printf("\n");
}

static void render_folder_tree(char **names, int count, char sep,
                                const FolderStatus *statuses) {
    int name_w = 40;
    printf("  %6s  %7s  %-*s  %7s\n", "Unread", "Flagged", name_w, "Folder", "Total");
    printf("  \u2550\u2550\u2550\u2550\u2550\u2550  \u2550\u2550\u2550\u2550\u2550\u2550\u2550  ");
    print_dbar(name_w);
    printf("  \u2550\u2550\u2550\u2550\u2550\u2550\u2550\n");

    for (int i = 0; i < count; i++) {
        int unseen   = statuses ? statuses[i].unseen   : 0;
        int flagged  = statuses ? statuses[i].flagged  : 0;
        int messages = statuses ? statuses[i].messages : 0;

        /* Build "tree-prefix + component-name" */
        char name_buf[512];
        int pos = 0;
        int depth = 0;
        for (const char *p = names[i]; *p; p++)
            if (*p == sep) depth++;
        for (int lv = 0; lv < depth; lv++) {
            int anc_last = ancestor_is_last(names, count, i, lv, sep);
            const char *branch = anc_last ? "    " : "\u2502   ";
            int blen = (int)strlen(branch);
            if (pos + blen < (int)sizeof(name_buf) - 1) {
                memcpy(name_buf + pos, branch, blen);
                pos += blen;
            }
        }
        int last = is_last_sibling(names, count, i, sep);
        const char *conn = last ? "\u2514\u2500\u2500 " : "\u251c\u2500\u2500 ";
        int clen = (int)strlen(conn);
        if (pos + clen < (int)sizeof(name_buf) - 1) {
            memcpy(name_buf + pos, conn, clen);
            pos += clen;
        }
        const char *comp = strrchr(names[i], sep);
        snprintf(name_buf + pos, sizeof(name_buf) - pos, "%s",
                 comp ? comp + 1 : names[i]);

        char u[16], f[16], t[16];
        fmt_thou(u, sizeof(u), unseen);
        fmt_thou(f, sizeof(f), flagged);
        fmt_thou(t, sizeof(t), messages);
        int nw = name_w + utf8_extra_bytes(name_buf);
        if (messages == 0)
            printf("\033[2m  %6s  %7s  %-*s  %7s\033[0m\n", u, f, nw, name_buf, t);
        else
            printf("  %6s  %7s  %-*s  %7s\n", u, f, nw, name_buf, t);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

/**
 * Case-insensitive match of `name` against the cached server folder list.
 * Returns a heap-allocated canonical name if the case differs, or NULL if
 * the name is already canonical (or the cache is unavailable).
 * Caller must free() the returned string.
 */
static char *resolve_folder_name_dup(const char *name) {
    int fcount = 0;
    char **fl = local_folder_list_load(&fcount, NULL);
    if (!fl) return NULL;
    char *result = NULL;
    for (int i = 0; i < fcount; i++) {
        if (strcasecmp(fl[i], name) == 0 && strcmp(fl[i], name) != 0) {
            result = strdup(fl[i]);   /* canonical differs from input */
            break;
        }
    }
    for (int i = 0; i < fcount; i++) free(fl[i]);
    free(fl);
    return result;
}

int email_service_list(const Config *cfg, EmailListOpts *opts) {
    /* Always re-initialise the local store so the correct account's manifests
     * and header cache are used, regardless of which account was active before. */
    local_store_init(cfg->host, cfg->user);

    const char *raw_folder = opts->folder ? opts->folder : cfg->folder;

    /* Normalise to the server-canonical name so the manifest key matches
     * what sync stored (e.g. config "Inbox" → server "INBOX"). */
    RAII_STRING char *folder_canonical = resolve_folder_name_dup(raw_folder);
    const char *folder = folder_canonical ? folder_canonical : raw_folder;

    /* Gmail: if the folder value looks like a display name rather than a label ID,
     * try to resolve it to the underlying ID via the local label name cache. */
    RAII_STRING char *gmail_resolved_id = NULL;
    if (cfg->gmail_mode && folder) {
        gmail_resolved_id = local_gmail_label_id_lookup(folder);
        if (gmail_resolved_id)
            folder = gmail_resolved_id;
    }

    /* Friendly display name for the folder/label (used in status bar).
     * For Gmail user labels: look up display name from ID.
     * For IMAP or system labels: use the ID as-is. */
    RAII_STRING char *folder_display_alloc = NULL;
    if (cfg->gmail_mode && folder)
        folder_display_alloc = local_gmail_label_name_lookup(folder);
    const char *folder_display = folder_display_alloc ? folder_display_alloc : folder;

    int list_result = 0;

    logger_log(LOG_INFO, "Listing %s @ %s/%s", cfg->user, cfg->host, folder);

    /* Load manifest (needed in both online and cron modes) */
    Manifest *manifest = manifest_load(folder);
    if (!manifest) {
        manifest = calloc(1, sizeof(Manifest));
        if (!manifest) { return -1; }
    }

    int show_count = 0;
    int unseen_count = 0;
    MsgEntry *entries = NULL;

    /* Shared mail client — populated in online mode, NULL in cron mode.
     * Kept alive for the full rendering loop so header fetches reuse it. */
    RAII_MAIL MailClient *list_mc = NULL;

    if (cfg->sync_interval > 0) {
        /* ── Cron / cache-only mode: serve entirely from manifest ──────── */
        if (manifest->count == 0) {
            manifest_free(manifest);
            if (!opts->pager) {
                printf("No cached data for %s. Run 'email-cli sync' first.\n", folder);
                return 0;
            }
            RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();
            {
                int tcols = terminal_cols(); int trows = terminal_rows();
                if (tcols <= 0) tcols = 80;
                if (trows <= 0) trows = 24;
                int avail = tcols - 28; if (avail < 40) avail = 40;
                int subj_w = avail * 3 / 5, from_w = avail - subj_w;
                printf("\033[H\033[2J");
                char cl[512];
                snprintf(cl, sizeof(cl),
                         "  0 of 0 message(s) in %s (0 unread) [%s].  \u26a0 No cached data \u2014 run 'email-sync' or 's=sync'",
                         folder_display, cfg->user ? cfg->user : "?");
                printf("\033[7m%s", cl);
                int used = visible_line_cols(cl, cl + strlen(cl));
                for (int p = used; p < tcols; p++) putchar(' ');
                printf("\033[0m\n\n");
                printf("  %-16s  %-4s  %-*s  %s\n",
                       "Date", "Sts", subj_w, "Subject", "From");
                printf("  ");
                print_dbar(16); printf("  \u2550\u2550\u2550\u2550  ");
                print_dbar(subj_w); printf("  "); print_dbar(from_w); printf("\n");
                printf("\n  \033[2m(empty)\033[0m\n");
                fflush(stdout);
                char sb[256];
                snprintf(sb, sizeof(sb),
                         "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                         "  Backspace=%s  ESC=quit"
                         "  s=sync  R=refresh  [0/0]",
                         cfg->gmail_mode ? "labels" : "folders");
                print_statusbar(trows, tcols, sb);
            }
            for (;;) {
                TermKey key = terminal_read_key();
                if (key == TERM_KEY_BACK) return 1;
                if (key == TERM_KEY_QUIT || key == TERM_KEY_ESC) return 0;
                int ch = terminal_last_printable();
                if (ch == 's') { sync_start_background(); }
                if (ch == 'R') return 4; /* refresh: re-list */
            }
        }
        show_count = manifest->count;
        entries = malloc((size_t)show_count * sizeof(MsgEntry));
        if (!entries) { manifest_free(manifest); return -1; }
        for (int i = 0; i < show_count; i++) {
            memcpy(entries[i].uid, manifest->entries[i].uid, 17);
            entries[i].flags = manifest->entries[i].flags;
            entries[i].epoch = parse_manifest_date(manifest->entries[i].date);
        }
        for (int i = 0; i < show_count; i++)
            if (entries[i].flags & MSG_FLAG_UNSEEN) unseen_count++;
    } else if (cfg->gmail_mode) {
        /* ── Gmail offline mode: load from local .idx + .hdr cache ─────── */
        /* SPAM and TRASH are stored under underscore-prefixed local names */
        const char *idx_folder = folder;
        if (strcmp(folder, "TRASH") == 0) idx_folder = "_trash";
        else if (strcmp(folder, "SPAM") == 0) idx_folder = "_spam";

        char (*idx_uids)[17] = NULL;
        int idx_count = 0;
        label_idx_load(idx_folder, &idx_uids, &idx_count);

        /* Only include entries that have a cached .hdr file.
         * UIDs without a .hdr were never fully synced (e.g. sync was
         * interrupted, or they are stale data from a previous format).
         * Skip them silently; a run of email-sync will fetch them. */
        entries = malloc((size_t)(idx_count > 0 ? idx_count : 1) * sizeof(MsgEntry));
        if (!entries) { free(idx_uids); manifest_free(manifest); return -1; }
        show_count = 0;

        for (int i = 0; i < idx_count; i++) {
            /* .hdr format: from\tsubject\tdate\tlabels\tflags */
            char *hdr = local_hdr_load("", idx_uids[i]);
            if (!hdr) continue;   /* not yet synced — skip */

            MsgEntry *e = &entries[show_count];
            memcpy(e->uid, idx_uids[i], 17);
            e->flags = 0;
            e->epoch = 0;

            /* Split tab-separated fields */
            char *fields[5] = {0};
            fields[0] = hdr;
            int f = 1;
            for (char *p = hdr; *p && f < 5; p++) {
                if (*p == '\t') { *p = '\0'; fields[f++] = p + 1; }
            }
            const char *from = fields[0] ? fields[0] : "";
            const char *subj = fields[1] ? fields[1] : "";
            const char *date = fields[2] ? fields[2] : "";
            int flags = fields[4] ? atoi(fields[4]) : 0;
            e->flags = flags;
            e->epoch = parse_manifest_date(date);
            /* Populate manifest so the renderer can find from/subject/date.
             * manifest_upsert takes ownership of the strings, so strdup
             * them — the originals point into the hdr buffer freed below. */
            manifest_upsert(manifest, idx_uids[i], strdup(from), strdup(subj), strdup(date), flags);
            free(hdr);

            if (e->flags & MSG_FLAG_UNSEEN) unseen_count++;
            show_count++;
        }
        free(idx_uids);
    } else {
        /* ── IMAP online mode: contact the server ──────────────────────── */

        /* Fetch UNSEEN and ALL UID sets via a shared mail client connection. */
        list_mc = make_mail(cfg);
        if (!list_mc) {
            manifest_free(manifest);
            fprintf(stderr, "Failed to connect.\n");
            return -1;
        }
        if (mail_client_select(list_mc, folder) != 0) {
            manifest_free(manifest);
            fprintf(stderr, "Failed to select folder %s.\n", folder);
            return -1;
        }

        char (*unseen_uids)[17] = NULL;
        int  unseen_uid_count = 0;
        if (mail_client_search(list_mc, MAIL_SEARCH_UNREAD, &unseen_uids, &unseen_uid_count) != 0) {
            manifest_free(manifest);
            fprintf(stderr, "Failed to search mailbox.\n");
            return -1;
        }

        char (*flagged_uids)[17] = NULL;
        int flagged_count = 0;
        mail_client_search(list_mc, MAIL_SEARCH_FLAGGED, &flagged_uids, &flagged_count);
        /* ignore errors — treat as 0 flagged */

        char (*done_uids)[17] = NULL;
        int done_count = 0;
        mail_client_search(list_mc, MAIL_SEARCH_DONE, &done_uids, &done_count);
        /* ignore errors — treat as 0 done */

        char (*all_uids)[17] = NULL;
        int  all_count = 0;
        if (mail_client_search(list_mc, MAIL_SEARCH_ALL, &all_uids, &all_count) != 0) {
            free(unseen_uids);
            free(flagged_uids);
            free(done_uids);
            manifest_free(manifest);
            fprintf(stderr, "Failed to search mailbox.\n");
            return -1;
        }
        /* Evict headers for messages deleted from the server */
        if (all_count > 0)
            local_hdr_evict_stale(folder, (const char (*)[17])all_uids, all_count);

        /* Remove entries for UIDs deleted from the server */
        if (all_count > 0)
            manifest_retain(manifest, (const char (*)[17])all_uids, all_count);

        show_count = all_count;

        if (show_count == 0) {
            manifest_free(manifest);
            free(unseen_uids);
            free(flagged_uids);
            free(done_uids);
            free(all_uids);
            if (!opts->pager) {
                printf("No messages in %s.\n", folder_display);
                return 0;
            }
            RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();
            {
                int tcols = terminal_cols(); int trows = terminal_rows();
                if (tcols <= 0) tcols = 80;
                if (trows <= 0) trows = 24;
                int avail = tcols - 28; if (avail < 40) avail = 40;
                int subj_w = avail * 3 / 5, from_w = avail - subj_w;
                printf("\033[H\033[2J");
                char cl[512];
                snprintf(cl, sizeof(cl),
                         "  0 of 0 message(s) in %s (0 unread) [%s].",
                         folder_display, cfg->user ? cfg->user : "?");
                printf("\033[7m%s", cl);
                int used = visible_line_cols(cl, cl + strlen(cl));
                for (int p = used; p < tcols; p++) putchar(' ');
                printf("\033[0m\n\n");
                printf("  %-16s  %-4s  %-*s  %s\n",
                       "Date", "Sts", subj_w, "Subject", "From");
                printf("  ");
                print_dbar(16); printf("  \u2550\u2550\u2550\u2550  ");
                print_dbar(subj_w); printf("  "); print_dbar(from_w); printf("\n");
                printf("\n  \033[2m(empty)\033[0m\n");
                fflush(stdout);
                char sb[256];
                if (cfg->gmail_mode) {
                    snprintf(sb, sizeof(sb),
                             "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                             "  Backspace=labels  ESC=quit"
                             "  c=compose  r=reply  n=unread  f=star"
                             "  s=sync  R=refresh  [0/0]");
                } else {
                    snprintf(sb, sizeof(sb),
                             "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                             "  Backspace=folders  ESC=quit"
                             "  c=compose  r=reply  n=new  f=flag  d=done"
                             "  s=sync  R=refresh  [0/0]");
                }
                print_statusbar(trows, tcols, sb);
            }
            for (;;) {
                TermKey key = terminal_read_key();
                if (key == TERM_KEY_BACK)  return 1;
                if (key == TERM_KEY_QUIT || key == TERM_KEY_ESC) return 0;
                int ch = terminal_last_printable();
                if (ch == 'c') return 2; /* compose */
                if (ch == 's') { sync_start_background(); }
                if (ch == 'R') return 4; /* refresh */
            }
        }

        /* Build tagged entry array */
        entries = malloc((size_t)show_count * sizeof(MsgEntry));
        if (!entries) { free(unseen_uids); free(flagged_uids); free(done_uids); free(all_uids); manifest_free(manifest); return -1; }

        for (int i = 0; i < show_count; i++) {
            memcpy(entries[i].uid, all_uids[i], 17);
            entries[i].flags = 0;
            for (int j = 0; j < unseen_uid_count;  j++)
                if (strcmp(unseen_uids[j],  all_uids[i]) == 0) { entries[i].flags |= MSG_FLAG_UNSEEN;  break; }
            for (int j = 0; j < flagged_count; j++)
                if (strcmp(flagged_uids[j], all_uids[i]) == 0) { entries[i].flags |= MSG_FLAG_FLAGGED; break; }
            for (int j = 0; j < done_count;    j++)
                if (strcmp(done_uids[j],    all_uids[i]) == 0) { entries[i].flags |= MSG_FLAG_DONE;    break; }
            /* Try to get date from cached manifest (may be 0 if not yet fetched) */
            ManifestEntry *me = manifest_find(manifest, all_uids[i]);
            entries[i].epoch = me ? parse_manifest_date(me->date) : 0;
        }
        /* Compute unseen_count for the status line */
        for (int i = 0; i < show_count; i++)
            if (entries[i].flags & MSG_FLAG_UNSEEN) unseen_count++;
        free(unseen_uids);
        free(flagged_uids);
        free(done_uids);
        free(all_uids);
    }

    if (show_count == 0) {
        manifest_free(manifest);
        free(entries);
        if (!opts->pager) {
            printf("No messages in %s.\n", folder_display);
            return 0;
        }
        RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();
        {
            int tcols = terminal_cols(); int trows = terminal_rows();
            if (tcols <= 0) tcols = 80;
            if (trows <= 0) trows = 24;
            int avail = tcols - 28; if (avail < 40) avail = 40;
            int subj_w = avail * 3 / 5, from_w = avail - subj_w;
            printf("\033[H\033[2J");
            char cl[512];
            snprintf(cl, sizeof(cl),
                     "  0 of 0 message(s) in %s (0 unread) [%s].",
                     folder_display, cfg->user ? cfg->user : "?");
            printf("\033[7m%s", cl);
            int used = visible_line_cols(cl, cl + strlen(cl));
            for (int p = used; p < tcols; p++) putchar(' ');
            printf("\033[0m\n\n");
            printf("  %-16s  %-4s  %-*s  %s\n",
                   "Date", "Sts", subj_w, "Subject", "From");
            printf("  ");
            print_dbar(16); printf("  \u2550\u2550\u2550\u2550  ");
            print_dbar(subj_w); printf("  "); print_dbar(from_w); printf("\n");
            printf("\n  \033[2m(empty)\033[0m\n");
            fflush(stdout);
            char sb[256];
            if (cfg->gmail_mode) {
                snprintf(sb, sizeof(sb),
                         "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                         "  Backspace=labels  ESC=quit"
                         "  c=compose  r=reply  n=unread  f=star"
                         "  s=sync  R=refresh  [0/0]");
            } else {
                snprintf(sb, sizeof(sb),
                         "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                         "  Backspace=folders  ESC=quit"
                         "  c=compose  r=reply  n=new  f=flag  d=done"
                         "  s=sync  R=refresh  [0/0]");
            }
            print_statusbar(trows, tcols, sb);
        }
        for (;;) {
            TermKey key = terminal_read_key();
            if (key == TERM_KEY_BACK)  return 1;
            if (key == TERM_KEY_QUIT || key == TERM_KEY_ESC) return 0;
            int ch = terminal_last_printable();
            if (ch == 'c') return 2; /* compose */
            if (ch == 's') { sync_start_background(); }
            if (ch == 'R') return 4; /* refresh */
        }
    }

    /* Sort: unseen → flagged → rest, within each group newest (highest UID) first */
    qsort(entries, (size_t)show_count, sizeof(MsgEntry), cmp_uid_entry);

    int limit  = (opts->limit > 0) ? opts->limit : show_count;
    int cursor = (opts->offset > 1) ? opts->offset - 1 : 0;
    if (cursor >= show_count) cursor = 0;
    int wstart = cursor;   /* top of the visible window */

    /* Track entries that have been locally label-removed ('d' key) but not
     * yet purged from the entries array.  TUI renders them with red
     * strikethrough so the user sees immediate feedback.  Only allocated in
     * TUI (pager) mode; always NULL in CLI/RO mode. */
    int *pending_remove = opts->pager
                          ? calloc((size_t)show_count, sizeof(int))
                          : NULL;

    /* Keep the terminal in raw mode for the entire interactive TUI.
     * Without this, each terminal_read_key() call would need to briefly enter
     * and exit raw mode per keystroke, which causes escape sequence echo and
     * ICANON buffering artefacts.  terminal_read_key() requires raw mode to
     * already be active — we enter it once here and exit at list_done. */
    RAII_TERM_RAW TermRawState *tui_raw = opts->pager
                                          ? terminal_raw_enter()
                                          : NULL;

    for (;;) {
        /* Scroll window to keep cursor visible */
        if (cursor < wstart)             wstart = cursor;
        if (cursor >= wstart + limit)    wstart = cursor - limit + 1;
        if (wstart < 0)                  wstart = 0;
        int wend = wstart + limit;
        if (wend > show_count)           wend = show_count;

        /* Compute adaptive column widths.
         * email-tui (opts->pager==1): date+sts+subject+from, overhead=28
         * email-cli/ro (opts->pager==0): uid+date+sts+subject+from, overhead=46
         * Non-TTY CLI: two-pass — pre-load all entries, use max Subject width. */
        int is_tty   = isatty(STDOUT_FILENO);
        int tcols    = is_tty ? terminal_cols() : 0;
        int is_gmail = cfg->gmail_mode;
        int show_uid = !opts->pager;   /* UID column in CLI/RO mode, not TUI */
        int overhead = show_uid ? 46 : 28;  /* 46 = 28 + uid(16) + sep(2) */
        int subj_w, from_w;
        if (is_tty) {
            int avail = tcols - overhead;
            if (avail < 40) avail = 40;
            subj_w = avail * 3 / 5;
            from_w = avail - subj_w;
        } else {
            subj_w = 0;
            from_w = 0;
        }

        /* Non-TTY CLI mode: pre-load all entries to determine exact Subject column width.
         * This two-pass approach ensures Subject is padded consistently so From starts
         * at a predictable column — required for reliable batch/script processing. */
        int manifest_dirty = 0;
        if (!opts->pager && !is_tty) {
            for (int i = wstart; i < wend; i++) {
                if (manifest_find(manifest, entries[i].uid)) continue;
                char *hdrs   = list_mc
                               ? fetch_uid_headers_via(list_mc, folder, entries[i].uid)
                               : fetch_uid_headers_cached(cfg, folder, entries[i].uid);
                char *fr_raw = hdrs ? mime_get_header(hdrs, "From")    : NULL;
                char *fr     = fr_raw ? mime_decode_words(fr_raw)      : strdup("");
                free(fr_raw);
                char *su_raw = hdrs ? mime_get_header(hdrs, "Subject") : NULL;
                char *su     = su_raw ? mime_decode_words(su_raw)      : strdup("");
                free(su_raw);
                char *dt_raw = hdrs ? mime_get_header(hdrs, "Date")    : NULL;
                char *dt     = dt_raw ? mime_format_date(dt_raw)       : strdup("");
                free(dt_raw);
                char *ct_raw = hdrs ? mime_get_header(hdrs, "Content-Type") : NULL;
                if (ct_raw && strcasestr(ct_raw, "multipart/mixed"))
                    entries[i].flags |= MSG_FLAG_ATTACH;
                free(ct_raw);
                free(hdrs);
                manifest_upsert(manifest, entries[i].uid, fr, su, dt, entries[i].flags);
                manifest_dirty = 1;
            }
            /* Compute max Subject display width across all visible entries */
            for (int i = wstart; i < wend; i++) {
                ManifestEntry *me = manifest_find(manifest, entries[i].uid);
                const char *sub = (me && me->subject) ? me->subject : "";
                int w = (int)visible_line_cols(sub, sub + strlen(sub));
                if (w > subj_w) subj_w = w;
            }
            from_w = 0; /* From is the last column — no right-padding needed */
        }

        if (opts->pager) printf("\033[H\033[2J");

        /* Count / status line */
        {
            char cl[512];
            int sync = sync_is_running();
            const char *suffix;
            if (bg_sync_done)
                suffix = "  \u2709 New mail may have arrived!  R=refresh";
            else if (sync)
                suffix = "  \u21bb syncing...";
            else if (is_gmail && strcmp(folder, "_trash") == 0)
                suffix = "  \u26a0 auto-delete: 30 days";
            else
                suffix = "";
            snprintf(cl, sizeof(cl),
                     "  %d-%d of %d message(s) in %s (%d unread) [%s].%s",
                     wstart + 1, wend, show_count, folder_display, unseen_count,
                     cfg->user ? cfg->user : "?", suffix);
            if (opts->pager) {
                /* TUI mode: reverse-video status bar padded to full terminal width */
                printf("\033[7m%s", cl);
                int used = visible_line_cols(cl, cl + strlen(cl));
                for (int p = used; p < tcols; p++) putchar(' ');
                printf("\033[0m\n\n");
            } else {
                printf("%s\n\n", cl);
            }
        }
        if (show_uid)
            printf("  %-16s  %-16s  %-4s  %-*s  %s\n",
                   "UID", "Date", "Sts", subj_w, "Subject", "From");
        else
            printf("  %-16s  %-4s  %-*s  %s\n",
                   "Date", "Sts", subj_w, "Subject", "From");
        printf("  ");
        if (show_uid) { print_dbar(16); printf("  "); }
        print_dbar(16); printf("  ");
        printf("\u2550\u2550\u2550\u2550  ");
        print_dbar(subj_w > 0 ? subj_w : 30); printf("  ");
        print_dbar(from_w > 0 ? from_w : 40); printf("\n");

        /* Data rows: fetch-on-demand + immediate render per row */
        int load_interrupted = 0;
        for (int i = wstart; i < wend; i++) {
            /* Fetch into manifest if missing; always sync unseen flag */
            ManifestEntry *cached_me = manifest_find(manifest, entries[i].uid);
            if (!cached_me) {
                /* Check for user interrupt before slow network fetch */
                if (opts->pager) {
                    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
                    if (poll(&pfd, 1, 0) > 0) {
                        TermKey key = terminal_read_key();
                        if (key == TERM_KEY_BACK) {
                            list_result = 1; load_interrupted = 1; break;
                        }
                        if (key == TERM_KEY_QUIT || key == TERM_KEY_ESC) {
                            list_result = 0; load_interrupted = 1; break;
                        }
                    }
                }
                char *hdrs     = list_mc
                                 ? fetch_uid_headers_via(list_mc, folder, entries[i].uid)
                                 : fetch_uid_headers_cached(cfg, folder, entries[i].uid);
                char *fr_raw   = hdrs ? mime_get_header(hdrs, "From")    : NULL;
                char *fr       = fr_raw ? mime_decode_words(fr_raw)      : strdup("");
                free(fr_raw);
                char *su_raw   = hdrs ? mime_get_header(hdrs, "Subject") : NULL;
                char *su       = su_raw ? mime_decode_words(su_raw)      : strdup("");
                free(su_raw);
                char *dt_raw   = hdrs ? mime_get_header(hdrs, "Date")    : NULL;
                char *dt       = dt_raw ? mime_format_date(dt_raw)       : strdup("");
                free(dt_raw);
                /* Detect attachment: Content-Type: multipart/mixed */
                char *ct_raw = hdrs ? mime_get_header(hdrs, "Content-Type") : NULL;
                if (ct_raw && strcasestr(ct_raw, "multipart/mixed"))
                    entries[i].flags |= MSG_FLAG_ATTACH;
                free(ct_raw);
                free(hdrs);
                manifest_upsert(manifest, entries[i].uid, fr, su, dt, entries[i].flags);
                manifest_dirty = 1;
            } else if (cached_me->flags != entries[i].flags) {
                /* Keep manifest flags in sync (relevant in online mode) */
                cached_me->flags = entries[i].flags;
                manifest_dirty = 1;
            }

            /* Render this row immediately */
            ManifestEntry *me = manifest_find(manifest, entries[i].uid);
            const char *from    = (me && me->from    && me->from[0])    ? me->from    : "(no from)";
            const char *subject = (me && me->subject && me->subject[0]) ? me->subject : "(no subject)";
            const char *date    = (me && me->date)                       ? me->date    : "";

            int sel     = opts->pager && (i == cursor);
            int pending = (pending_remove != NULL) && pending_remove[i];

            /* Pending-remove rows (TUI only): red + strikethrough.
             * When the cursor is also on the row, add inverse-video so the
             * cursor position remains visible.  The status column uses plain
             * ASCII (no embedded colour escapes) for pending rows to avoid
             * \033[0m conflicts with the combined attribute set. */
            if (opts->pager && pending && sel) {
                printf("\033[7m\033[31m\033[9m"); /* inverse + red + strikethrough */
            } else if (opts->pager && pending) {
                printf("\033[31m\033[9m");         /* red + strikethrough */
            } else if (sel) {
                printf("\033[7m");
            }

            /* Status column: coloured in TUI (except pending-remove rows).
             * In sel (reverse-video) rows: temporarily exit reverse-video
             * for the coloured character, then re-enter (\033[7m) so the
             * rest of the row stays highlighted. Plain CLI/RO stays ASCII. */
            char sts[64];
            if (opts->pager && !pending) {
                /* Non-sel: plain colour reset after char.
                 * Sel: \033[0m exits rev-video → colour → \033[7m re-enters. */
                const char *n_s = (entries[i].flags & MSG_FLAG_UNSEEN)
                    ? (sel ? "\033[0m\033[32mN\033[7m" : "\033[32mN\033[0m")
                    : "-";
                const char *f_s = (entries[i].flags & MSG_FLAG_FLAGGED)
                    ? (sel ? "\033[0m\033[33m\xe2\x98\x85\033[7m"
                           : "\033[33m\xe2\x98\x85\033[0m")
                    : "-";
                snprintf(sts, sizeof(sts), "%s%s%c%c", n_s, f_s,
                    (entries[i].flags & MSG_FLAG_DONE)   ? 'D' : '-',
                    (entries[i].flags & MSG_FLAG_ATTACH) ? 'A' : '-');
            } else {
                sts[0] = (entries[i].flags & MSG_FLAG_UNSEEN)  ? 'N' : '-';
                sts[1] = (entries[i].flags & MSG_FLAG_FLAGGED) ? '*' : '-';
                sts[2] = (entries[i].flags & MSG_FLAG_DONE)    ? 'D' : '-';
                sts[3] = (entries[i].flags & MSG_FLAG_ATTACH)  ? 'A' : '-';
                sts[4] = '\0';
            }
            if (show_uid)
                printf("  %-16.16s  %-16.16s  %s  ", entries[i].uid, date, sts);
            else
                printf("  %-16.16s  %s  ", date, sts);
            print_padded_col(subject, subj_w);
            printf("  ");
            print_padded_col(from,    from_w);

            if (opts->pager && (sel || pending)) printf("\033[K\033[0m");
            printf("\n");
            fflush(stdout); /* show row immediately as it arrives */
        }
        if (manifest_dirty) manifest_save(folder, manifest);
        if (load_interrupted) goto list_done;

        if (!opts->pager) {
            if (wend < show_count)
                printf("\n  -- %d more message(s) --  use --offset %d for next page\n",
                       show_count - wend, wend + 1);
            break;
        }

        /* Navigation hint (status bar) — anchored at last terminal row */
        fflush(stdout);
        {
            int trows = terminal_rows();
            if (trows <= 0) trows = limit + 6;
            char sb[256];
            if (is_gmail) {
                snprintf(sb, sizeof(sb),
                         "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                         "  Backspace=labels  ESC=quit"
                         "  c=compose  r=reply  n=unread  f=star  a=archive"
                         "  d=rm-label  D=trash  s=sync  R=refresh  [%d/%d]",
                         cursor + 1, show_count);
            } else {
                snprintf(sb, sizeof(sb),
                         "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                         "  Backspace=folders  ESC=quit"
                         "  c=compose  r=reply  n=new  f=flag  d=done"
                         "  s=sync  R=refresh  [%d/%d]",
                         cursor + 1, show_count);
            }
            print_statusbar(trows, tcols, sb);
        }

        /* terminal_read_key() blocks in read().  When the background sync child
         * exits, SIGCHLD fires (SA_RESTART not set) and interrupts read() with
         * EINTR — terminal_read_key() returns TERM_KEY_IGNORE (last_printable=0).
         * We detect this by checking whether bg_sync_done changed, and if so
         * jump back here to wait for the next real keypress without re-rendering.
         * The notification will appear the next time the user presses a key. */
read_key_again: ;
        int prev_sync_done = bg_sync_done;
        TermKey key = terminal_read_key();
        fprintf(stderr, "\r\033[K"); fflush(stderr);
        if (bg_sync_done && !prev_sync_done) {
            goto read_key_again; /* SIGCHLD woke us — wait for real keypress */
        }

        switch (key) {
        case TERM_KEY_BACK:
            list_result = 1;
            goto list_done;
        case TERM_KEY_QUIT:
        case TERM_KEY_ESC:
            goto list_done;
        case TERM_KEY_ENTER:
            {
                int ret = show_uid_interactive(cfg, folder, entries[cursor].uid, opts->limit);
                if (ret == 1) goto list_done;  /* user quit from show */
                if (ret == 2) {
                    /* 'r' pressed in reader → reply to this message */
                    memcpy(opts->action_uid, entries[cursor].uid, 17);
                    list_result = 3;
                    goto list_done;
                }
                /* ret == 0: Backspace → back to list; ret == -1: error → stay */
            }
            break;
        case TERM_KEY_HOME:
            cursor = 0;
            break;
        case TERM_KEY_END:
            cursor = show_count > 0 ? show_count - 1 : 0;
            break;
        case TERM_KEY_LEFT:
        case TERM_KEY_RIGHT:
        case TERM_KEY_DELETE:
        case TERM_KEY_TAB:
        case TERM_KEY_SHIFT_TAB:
        case TERM_KEY_IGNORE: {
            int ch = terminal_last_printable();
            if (ch == 'c') {
                list_result = 2;
                goto list_done;
            }
            if (ch == 'r') {
                memcpy(opts->action_uid, entries[cursor].uid, 17);
                list_result = 3;
                goto list_done;
            }
            if (ch == 's') {
                sync_start_background();
                break; /* re-render: shows ⟳ syncing... indicator */
            }
            if (ch == 'R') {
                /* Explicit refresh after sync notification */
                bg_sync_done = 0;
                list_result = 4;
                goto list_done;
            }
            if (ch == 'h' || ch == '?') {
                if (is_gmail) {
                    static const char *ghelp[][2] = {
                        { "\u2191 / \u2193",   "Move cursor up / down"           },
                        { "PgUp / PgDn",        "Page up / down"                  },
                        { "Enter",             "Open selected message"           },
                        { "r",                 "Reply to selected message"       },
                        { "c",                 "Compose new message"             },
                        { "n",                 "Toggle Unread label"             },
                        { "f",                 "Toggle Starred label"            },
                        { "a",                 "Archive (remove INBOX label)"    },
                        { "d",                 "Remove current label"            },
                        { "D",                 "Move to Trash"                   },
                        { "u",                 "Untrash (restore labels)"        },
                        { "t",                 "Toggle labels (picker)"          },
                        { "s",                 "Start background sync"           },
                        { "R",                 "Refresh after sync"              },
                        { "Backspace",         "Open label browser"              },
                        { "ESC / q",           "Quit"                            },
                        { "h / ?",             "Show this help"                  },
                    };
                    show_help_popup("Message list shortcuts (Gmail)",
                                    ghelp, (int)(sizeof(ghelp)/sizeof(ghelp[0])));
                } else {
                    static const char *help[][2] = {
                        { "\u2191 / \u2193",   "Move cursor up / down"           },
                        { "PgUp / PgDn",        "Page up / down"                  },
                        { "Enter",             "Open selected message"           },
                        { "r",                 "Reply to selected message"       },
                        { "c",                 "Compose new message"             },
                        { "n",                 "Toggle New (unread) flag"        },
                        { "f",                 "Toggle Flagged (starred) flag"   },
                        { "d",                 "Toggle Done flag"                },
                        { "s",                 "Start background sync"           },
                        { "R",                 "Refresh after sync"              },
                        { "Backspace",         "Open folder browser"             },
                        { "ESC / q",           "Quit"                            },
                        { "h / ?",             "Show this help"                  },
                    };
                    show_help_popup("Message list shortcuts",
                                    help, (int)(sizeof(help)/sizeof(help[0])));
                }
                break;
            }
            if (ch == 'a' && is_gmail) {
                /* Archive: remove INBOX label from current message */
                const char *uid = entries[cursor].uid;
                if (list_mc) {
                    /* Mark as read and remove INBOX label via Gmail API */
                    mail_client_set_flag(list_mc, uid, "\\Seen", 1);
                    mail_client_modify_label(list_mc, uid, "INBOX", 0);
                }
                /* Update local index and .hdr: remove INBOX */
                label_idx_remove("INBOX", uid);
                { const char *_inbox = "INBOX";
                  local_hdr_update_labels("", uid, NULL, 0, &_inbox, 1); }
                /* If no other labels remain, add to _nolabel */
                {
                    char *lbl = local_hdr_get_labels("", uid);
                    int has_real = 0;
                    if (lbl) {
                        char *tok = lbl, *s;
                        while (tok && *tok) {
                            s = strchr(tok, ',');
                            size_t tl = s ? (size_t)(s - tok) : strlen(tok);
                            char lb[64];
                            if (tl >= sizeof(lb)) tl = sizeof(lb) - 1;
                            memcpy(lb, tok, tl); lb[tl] = '\0';
                            if (strcmp(lb, "INBOX") != 0 &&
                                strcmp(lb, "UNREAD") != 0 &&
                                strcmp(lb, "IMPORTANT") != 0 &&
                                strncmp(lb, "CATEGORY_", 9) != 0)
                                has_real = 1;
                            tok = s ? s + 1 : NULL;
                        }
                        free(lbl);
                    }
                    if (!has_real) label_idx_add("_nolabel", uid);
                }
                break;
            }
            if (ch == 'D' && is_gmail) {
                /* Trash: Gmail compound trash operation */
                const char *uid = entries[cursor].uid;
                /* Save current labels for potential untrash */
                char *pre_trash = local_hdr_get_labels("", uid);
                if (pre_trash) {
                    local_trash_labels_save(uid, pre_trash);
                    free(pre_trash);
                }
                if (list_mc) mail_client_trash(list_mc, uid);
                /* Remove from all local label indexes */
                {
                    char **all_labels = NULL;
                    int all_count = 0;
                    label_idx_list(&all_labels, &all_count);
                    for (int j = 0; j < all_count; j++) {
                        label_idx_remove(all_labels[j], uid);
                        free(all_labels[j]);
                    }
                    free(all_labels);
                }
                label_idx_add("_trash", uid);
                break;
            }
            if (ch == 'u' && is_gmail) {
                /* Untrash: restore from Trash + restore saved labels */
                const char *uid = entries[cursor].uid;
                if (list_mc) {
                    /* Gmail API: untrash removes TRASH label */
                    mail_client_modify_label(list_mc, uid, "TRASH", 0);
                }
                label_idx_remove("_trash", uid);
                /* Restore saved labels */
                char *saved = local_trash_labels_load(uid);
                if (saved) {
                    int restored_real = 0;
                    char *tok = saved, *sep;
                    while (tok && *tok) {
                        sep = strchr(tok, ',');
                        size_t tl = sep ? (size_t)(sep - tok) : strlen(tok);
                        char lb[64];
                        if (tl >= sizeof(lb)) tl = sizeof(lb) - 1;
                        memcpy(lb, tok, tl); lb[tl] = '\0';
                        if (lb[0] && strcmp(lb, "UNREAD") != 0 &&
                            strcmp(lb, "IMPORTANT") != 0 &&
                            strncmp(lb, "CATEGORY_", 9) != 0) {
                            label_idx_add(lb, uid);
                            if (list_mc)
                                mail_client_modify_label(list_mc, uid, lb, 1);
                            restored_real = 1;
                        }
                        tok = sep ? sep + 1 : NULL;
                    }
                    free(saved);
                    local_trash_labels_remove(uid);
                    /* If only CATEGORY_* labels were saved, also restore to Archive */
                    if (!restored_real)
                        label_idx_add("_nolabel", uid);
                } else {
                    /* No saved labels — put in Archive (_nolabel) */
                    label_idx_add("_nolabel", uid);
                }
                break;
            }
            if (ch == 't' && is_gmail) {
                show_label_picker(list_mc, entries[cursor].uid);
                break;
            }
            if (ch == 'd' && is_gmail) {
                /* Toggle: first 'd' removes the label (pending_remove);
                 * second 'd' on the same row restores it (undo).
                 * Restricted to non-meta labels (no underscore prefix). */
                if (folder[0] != '_') {
                    const char *uid = entries[cursor].uid;
                    if (pending_remove && pending_remove[cursor]) {
                        /* Undo: restore the label that was removed */
                        if (list_mc)
                            mail_client_modify_label(list_mc, uid, folder, 1);
                        label_idx_add(folder, uid);
                        local_hdr_update_labels("", uid, &folder, 1, NULL, 0);
                        /* Remove from archive fallback if it was added */
                        label_idx_remove("_nolabel", uid);
                        pending_remove[cursor] = 0;
                    } else {
                        /* Remove label from this message */
                        if (list_mc)
                            mail_client_modify_label(list_mc, uid, folder, 0);
                        label_idx_remove(folder, uid);
                        local_hdr_update_labels("", uid, NULL, 0, &folder, 1);
                        /* If no other real labels remain, put in archive */
                        char *lbl = local_hdr_get_labels("", uid);
                        int has_real = 0;
                        if (lbl) {
                            char *tok = lbl, *s;
                            while (tok && *tok) {
                                s = strchr(tok, ',');
                                size_t tl = s ? (size_t)(s - tok) : strlen(tok);
                                char lb[64];
                                if (tl >= sizeof(lb)) tl = sizeof(lb) - 1;
                                memcpy(lb, tok, tl); lb[tl] = '\0';
                                if (strcmp(lb, folder) != 0 &&
                                    strcmp(lb, "UNREAD") != 0 &&
                                    strcmp(lb, "IMPORTANT") != 0 &&
                                    strncmp(lb, "CATEGORY_", 9) != 0)
                                    has_real = 1;
                                tok = s ? s + 1 : NULL;
                            }
                            free(lbl);
                        }
                        if (!has_real) label_idx_add("_nolabel", uid);
                        /* Mark row for immediate visual feedback (red strikethrough) */
                        if (pending_remove) pending_remove[cursor] = 1;
                    }
                }
                break;
            }
            if (ch == 'n' || ch == 'f' || ch == 'd') {
                const char *uid = entries[cursor].uid;
                int bit;
                const char *flag_name;
                if (ch == 'n') {
                    bit = MSG_FLAG_UNSEEN;  flag_name = "\\Seen";
                } else if (ch == 'f') {
                    bit = MSG_FLAG_FLAGGED; flag_name = "\\Flagged";
                } else {
                    bit = MSG_FLAG_DONE;    flag_name = "$Done";
                }
                int currently = entries[cursor].flags & bit;
                /* Determine the IMAP add/remove direction */
                int add_flag = (ch == 'n') ? (currently ? 1 : 0) : (!currently ? 1 : 0);

                /* Local update first — instant UI response regardless of network */
                local_pending_flag_add(folder, uid, flag_name, add_flag);
                entries[cursor].flags ^= bit;
                ManifestEntry *me = manifest_find(manifest, uid);
                if (me) me->flags = entries[cursor].flags;
                manifest_save(folder, manifest);

                /* Gmail: update local label indexes and .hdr (both labels CSV
                 * and flags integer), then kick background sync. */
                if (is_gmail) {
                    const char *lbl = (ch == 'n') ? "UNREAD"
                                    : (ch == 'f') ? "STARRED" : NULL;
                    if (lbl) {
                        /* n/f: label-backed flag — keep .idx and .hdr in sync */
                        if (currently) {
                            label_idx_remove(lbl, uid);
                            local_hdr_update_labels("", uid, NULL, 0, &lbl, 1);
                        } else {
                            label_idx_add(lbl, uid);
                            local_hdr_update_labels("", uid, &lbl, 1, NULL, 0);
                        }
                    } else {
                        /* d: $Done is an IMAP keyword, not a Gmail label */
                        local_hdr_update_flags("", uid, entries[cursor].flags);
                    }
                    flag_push_background(cfg, uid, flag_name, add_flag);
                } else if (list_mc) {
                    /* IMAP online mode: connection already open, push immediately */
                    mail_client_set_flag(list_mc, uid, flag_name, add_flag);
                }
            }
            break;
        }
        case TERM_KEY_NEXT_LINE:
            if (cursor < show_count - 1) cursor++;
            break;
        case TERM_KEY_PREV_LINE:
            if (cursor > 0) cursor--;
            break;
        case TERM_KEY_NEXT_PAGE:
            cursor += limit;
            if (cursor >= show_count) cursor = show_count - 1;
            break;
        case TERM_KEY_PREV_PAGE:
            cursor -= limit;
            if (cursor < 0) cursor = 0;
            break;
        }
    }
list_done:
    free(pending_remove);
    /* tui_raw / folder_canonical cleaned up automatically via RAII macros */
    manifest_free(manifest);
    free(entries);
    return list_result;
}

/** Fetch the folder list into a heap-allocated array; caller owns entries and array. */
static char **fetch_folder_list_from_server(const Config *cfg,
                                             int *count_out, char *sep_out) {
    RAII_MAIL MailClient *mc = make_mail(cfg);
    if (!mc) return NULL;

    char **folders = NULL;
    int count = 0;
    char sep = '.';
    if (mail_client_list(mc, &folders, &count, &sep) != 0) return NULL;

    *count_out = count;
    if (sep_out) *sep_out = sep;
    return folders;
}

static char **fetch_folder_list(const Config *cfg, int *count_out, char *sep_out) {
    /* Try local cache first (populated by sync). */
    char **cached = local_folder_list_load(count_out, sep_out);
    if (cached && *count_out > 0) return cached;
    if (cached) { free(cached); }

    /* Fall back to server. */
    char sep = '.';
    char **folders = fetch_folder_list_from_server(cfg, count_out, &sep);
    if (folders && *count_out > 0) {
        local_folder_list_save((const char **)folders, *count_out, sep);
        if (sep_out) *sep_out = sep;
    }
    return folders;
}

int email_service_list_folders(const Config *cfg, int tree) {
    int count = 0;
    char sep = '.';
    char **folders = fetch_folder_list(cfg, &count, &sep);

    if (!folders || count == 0) {
        printf("No folders found.\n");
        if (folders) free(folders);
        return folders ? 0 : -1;
    }

    qsort(folders, (size_t)count, sizeof(char *), cmp_str);

    FolderStatus *statuses = fetch_all_folder_statuses(cfg, folders, count);

    if (tree) {
        render_folder_tree(folders, count, sep, statuses);
    } else {
        /* Batch flat view: Unread | Flagged | Folder | Total */
        int name_w = 40;
        printf("  %6s  %7s  %-*s  %7s\n",
               "Unread", "Flagged", name_w, "Folder", "Total");
        printf("  \u2550\u2550\u2550\u2550\u2550\u2550  \u2550\u2550\u2550\u2550\u2550\u2550\u2550  ");
        print_dbar(name_w);
        printf("  \u2550\u2550\u2550\u2550\u2550\u2550\u2550\n");
        for (int i = 0; i < count; i++) {
            int unseen   = statuses ? statuses[i].unseen   : 0;
            int flagged  = statuses ? statuses[i].flagged  : 0;
            int messages = statuses ? statuses[i].messages : 0;
            char u[16], f[16], t[16];
            fmt_thou(u, sizeof(u), unseen);
            fmt_thou(f, sizeof(f), flagged);
            fmt_thou(t, sizeof(t), messages);
            int nw = name_w + utf8_extra_bytes(folders[i]);
            if (messages == 0)
                printf("\033[2m  %6s  %7s  %-*s  %7s\033[0m\n",
                       u, f, nw, folders[i], t);
            else
                printf("  %6s  %7s  %-*s  %7s\n",
                       u, f, nw, folders[i], t);
        }
    }

    free(statuses);
    for (int i = 0; i < count; i++) free(folders[i]);
    free(folders);
    return 0;
}

char *email_service_list_folders_interactive(const Config *cfg,
                                             const char *current_folder,
                                             int *go_up) {
    local_store_init(cfg->host, cfg->user);
    if (go_up) *go_up = 0;
    int count = 0;
    char sep = '.';
    char **folders = fetch_folder_list(cfg, &count, &sep);
    if (!folders || count == 0) {
        if (folders) free(folders);
        return NULL;
    }

    qsort(folders, (size_t)count, sizeof(char *), cmp_str);

    FolderStatus *statuses = fetch_all_folder_statuses(cfg, folders, count);

    int *vis = malloc((size_t)count * sizeof(int));
    if (!vis) {
        free(statuses);
        for (int i = 0; i < count; i++) free(folders[i]);
        free(folders);
        return NULL;
    }

    int cursor = 0, wstart = 0;
    int tree_mode = ui_pref_get_int("folder_view_mode", 1);
    char current_prefix[512] = "";   /* flat mode: current navigation level */

    /* Pre-position cursor on current_folder.
     * INBOX is case-insensitive per RFC 3501 — use strcasecmp so that a
     * config value of "inbox" still matches the server's "INBOX". */
    if (current_folder && *current_folder) {
        if (tree_mode) {
            /* In tree mode the flat view is folders[0..count-1] directly */
            for (int i = 0; i < count; i++) {
                if (strcasecmp(folders[i], current_folder) == 0) {
                    cursor = i; break;
                }
            }
        } else {
            /* In flat mode, navigate to the level that contains current_folder */
            const char *last = strrchr(current_folder, sep);
            if (last) {
                size_t plen = (size_t)(last - current_folder);
                if (plen < sizeof(current_prefix)) {
                    memcpy(current_prefix, current_folder, plen);
                    current_prefix[plen] = '\0';
                }
            }
            int tmp_vis[1024];
            int tv = build_flat_view(folders, count, sep, current_prefix, tmp_vis);
            for (int i = 0; i < tv; i++) {
                if (strcasecmp(folders[tmp_vis[i]], current_folder) == 0) {
                    cursor = i; break;
                }
            }
        }
    }
    int vcount = 0;                  /* flat view: number of visible entries */
    char *selected = NULL;

    RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();

    for (;;) {
        int rows  = terminal_rows();
        int limit = (rows > 4) ? rows - 3 : 10;

        /* Rebuild flat view on each iteration (alphabetical order) */
        int display_count;
        if (tree_mode) {
            display_count = count;
        } else {
            vcount = build_flat_view(folders, count, sep, current_prefix, vis);
            display_count = vcount;
        }
        if (cursor >= display_count && display_count > 0)
            cursor = display_count - 1;

        if (cursor < wstart) wstart = cursor;
        if (cursor >= wstart + limit) wstart = cursor - limit + 1;
        int wend = wstart + limit;
        if (wend > display_count) wend = display_count;

        /* Compute name column width for flat mode */
        int tcols_f = terminal_cols();
        /* Fixed: "  " + 6 (unread) + "  " + 7 (flagged) + "  " + name_w + "  " + 7 (total) = name_w + 28 */
        int name_w = tcols_f - 28;
        if (name_w < 20) name_w = 20;

        printf("\033[H\033[2J");
        {
            char cl[1024];
            if (!tree_mode && current_prefix[0])
                snprintf(cl, sizeof(cl), "  Folders \u2014 %s  \u203a %s/  (%d)",
                         cfg->user ? cfg->user : "?",
                         current_prefix, display_count);
            else
                snprintf(cl, sizeof(cl), "  Folders \u2014 %s  (%d)",
                         cfg->user ? cfg->user : "?",
                         display_count);
            printf("\033[7m%s", cl);
            int used = visible_line_cols(cl, cl + strlen(cl));
            for (int p = used; p < tcols_f; p++) putchar(' ');
            printf("\033[0m\n\n");
        }

        /* Column header and separator (both flat and tree mode) */
        printf("  %6s  %7s  %-*s  %7s\n", "Unread", "Flagged", name_w, "Folder", "Total");
        printf("  \u2550\u2550\u2550\u2550\u2550\u2550  \u2550\u2550\u2550\u2550\u2550\u2550\u2550  ");
        print_dbar(name_w);
        printf("  \u2550\u2550\u2550\u2550\u2550\u2550\u2550\n");

        for (int i = wstart; i < wend; i++) {
            if (tree_mode) {
                int msgs = statuses ? statuses[i].messages : 0;
                int unsn = statuses ? statuses[i].unseen   : 0;
                int flgd = statuses ? statuses[i].flagged  : 0;
                print_folder_item(folders, count, i, sep, 1, i == cursor, 0,
                                  msgs, unsn, flgd, name_w);
            } else {
                int fi = vis[i];
                int hk = folder_has_children(folders, count, folders[fi], sep);
                int msgs, unsn, flgd;
                if (hk) {
                    /* Aggregate own + all descendant counts so the user can see
                     * total unread/flagged even when children are not expanded. */
                    sum_subtree(folders, count, sep, folders[fi], statuses,
                                &msgs, &unsn, &flgd);
                } else {
                    msgs = statuses ? statuses[fi].messages : 0;
                    unsn = statuses ? statuses[fi].unseen   : 0;
                    flgd = statuses ? statuses[fi].flagged  : 0;
                }
                print_folder_item(folders, count, fi, sep, 0, i == cursor, hk,
                                  msgs, unsn, flgd, name_w);
            }
        }

        fflush(stdout);
        {
            int trows_f = terminal_rows();
            if (trows_f <= 0) trows_f = limit + 4;
            int tcols_f = terminal_cols();
            char sb[256];
            if (!tree_mode && current_prefix[0])
                snprintf(sb, sizeof(sb),
                         "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open/select"
                         "  t=tree  Backspace=up  ESC=quit  [%d/%d]",
                         display_count > 0 ? cursor + 1 : 0, display_count);
            else
                snprintf(sb, sizeof(sb),
                         "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open/select"
                         "  t=%s  Backspace=back  ESC=quit  [%d/%d]",
                         tree_mode ? "flat" : "tree",
                         display_count > 0 ? cursor + 1 : 0, display_count);
            print_statusbar(trows_f, tcols_f, sb);
        }

        TermKey key = terminal_read_key();

        switch (key) {
        case TERM_KEY_QUIT:
        case TERM_KEY_ESC:
            goto folders_int_done;
        case TERM_KEY_BACK:
            if (!tree_mode && current_prefix[0]) {
                /* navigate up one level */
                char *last_sep = strrchr(current_prefix, sep);
                if (last_sep) *last_sep = '\0';
                else          current_prefix[0] = '\0';
                cursor = 0; wstart = 0;
            } else {
                /* at root: go up to accounts screen (if caller supports it),
                 * or return unchanged current folder (legacy behaviour). */
                if (go_up) {
                    *go_up = 1; /* selected stays NULL */
                } else {
                    if (current_folder && *current_folder)
                        selected = strdup(current_folder);
                }
                goto folders_int_done;
            }
            break;
        case TERM_KEY_ENTER:
            if (tree_mode) {
                selected = strdup(folders[cursor]);
                goto folders_int_done;
            } else if (display_count > 0) {
                int fi = vis[cursor];
                if (folder_has_children(folders, count, folders[fi], sep)) {
                    /* navigate into subfolder */
                    strncpy(current_prefix, folders[fi], sizeof(current_prefix) - 1);
                    current_prefix[sizeof(current_prefix) - 1] = '\0';
                    cursor = 0; wstart = 0;
                } else {
                    selected = strdup(folders[fi]);
                    goto folders_int_done;
                }
            }
            break;
        case TERM_KEY_NEXT_LINE:
            if (cursor < display_count - 1) cursor++;
            break;
        case TERM_KEY_PREV_LINE:
            if (cursor > 0) cursor--;
            break;
        case TERM_KEY_NEXT_PAGE:
            cursor += limit;
            if (cursor >= display_count) cursor = display_count > 0 ? display_count - 1 : 0;
            break;
        case TERM_KEY_PREV_PAGE:
            cursor -= limit;
            if (cursor < 0) cursor = 0;
            break;
        case TERM_KEY_HOME:
            cursor = 0; wstart = 0;
            break;
        case TERM_KEY_END:
            cursor = display_count > 0 ? display_count - 1 : 0;
            break;
        case TERM_KEY_LEFT:
        case TERM_KEY_RIGHT:
        case TERM_KEY_DELETE:
        case TERM_KEY_TAB:
        case TERM_KEY_SHIFT_TAB:
        case TERM_KEY_IGNORE: {
            int ch = terminal_last_printable();
            if (ch == 't') {
                tree_mode = !tree_mode;
                ui_pref_set_int("folder_view_mode", tree_mode);
                cursor = 0; wstart = 0;
                if (!tree_mode) current_prefix[0] = '\0';
            } else if (ch == 'h' || ch == '?') {
                static const char *help[][2] = {
                    { "\u2191 / \u2193",   "Move cursor up / down"                   },
                    { "PgUp / PgDn",        "Move cursor one page up / down"          },
                    { "Enter",             "Open folder / navigate into subfolder"   },
                    { "t",                 "Toggle tree / flat view"                 },
                    { "Backspace",         "Go up one level (or back to accounts)"   },
                    { "ESC / q",           "Quit"                                    },
                    { "h / ?",             "Show this help"                          },
                };
                show_help_popup("Folder browser shortcuts",
                                help, (int)(sizeof(help)/sizeof(help[0])));
            }
            break;
        }
        }
    }
folders_int_done:
    free(statuses);
    free(vis);
    for (int i = 0; i < count; i++) free(folders[i]);
    free(folders);
    return selected;
}

/* ── Gmail Label Picker Popup ────────────────────────────────────────── */

/**
 * Show a popup overlay with checkboxes for each label.
 * The user can toggle labels on/off with Enter/Space, navigate with arrows.
 * Applies changes via mail_client_set_flag and updates local .idx.
 * Returns when the user presses ESC/Backspace/q.
 */
static void show_label_picker(MailClient *mc,
                               const char *uid) {
    /* Collect available labels from local .idx files */
    char **all_labels = NULL;
    int all_count = 0;
    label_idx_list(&all_labels, &all_count);

    /* Build display: only user labels + key system labels (INBOX, STARRED, etc.)
     * Skip UNREAD (use 'n' key), _nolabel, _spam, _trash (system-managed) */
    char **pick_ids   = NULL;
    char **pick_names = NULL;
    int  *pick_on     = NULL;
    int  pick_count = 0, pick_cap = 0;

    /* Add system labels first */
    static const char *sys_pick[] = {"INBOX", "STARRED", "SENT", "DRAFTS"};
    for (int s = 0; s < (int)(sizeof(sys_pick)/sizeof(sys_pick[0])); s++) {
        if (pick_count == pick_cap) {
            int nc = pick_cap ? pick_cap * 2 : 16;
            pick_ids   = realloc(pick_ids,   (size_t)nc * sizeof(char *));
            pick_names = realloc(pick_names, (size_t)nc * sizeof(char *));
            pick_on    = realloc(pick_on,    (size_t)nc * sizeof(int));
            pick_cap   = nc;
        }
        pick_ids[pick_count]   = strdup(sys_pick[s]);
        pick_names[pick_count] = strdup(sys_pick[s]);
        pick_on[pick_count]    = label_idx_contains(sys_pick[s], uid);
        pick_count++;
    }
    /* Add user labels */
    for (int i = 0; i < all_count; i++) {
        if (is_system_or_special_label(all_labels[i])) {
            free(all_labels[i]);
            continue;
        }
        if (strcmp(all_labels[i], "UNREAD") == 0) { free(all_labels[i]); continue; }
        if (pick_count == pick_cap) {
            int nc = pick_cap ? pick_cap * 2 : 16;
            pick_ids   = realloc(pick_ids,   (size_t)nc * sizeof(char *));
            pick_names = realloc(pick_names, (size_t)nc * sizeof(char *));
            pick_on    = realloc(pick_on,    (size_t)nc * sizeof(int));
            pick_cap   = nc;
        }
        pick_ids[pick_count]   = all_labels[i]; /* transfer ownership */
        pick_names[pick_count] = strdup(all_labels[i]);
        pick_on[pick_count]    = label_idx_contains(all_labels[i], uid);
        pick_count++;
    }
    free(all_labels);

    if (pick_count == 0) {
        free(pick_ids); free(pick_names); free(pick_on);
        return;
    }

    int pcursor = 0;
    int tcols = terminal_cols();
    int trows = terminal_rows();
    if (tcols <= 0) tcols = 80;
    if (trows <= 0) trows = 24;

    int inner_w = 30;
    int box_w = inner_w + 4;
    int box_h = pick_count + 4;
    int col0 = (tcols - box_w) / 2;
    int row0 = (trows - box_h) / 2;
    if (col0 < 1) col0 = 1;
    if (row0 < 1) row0 = 1;

    for (;;) {
        /* Draw popup */
        fprintf(stderr, "\033[%d;%dH\033[7m\u250c", row0, col0);
        for (int i = 0; i < box_w - 2; i++) fprintf(stderr, "\u2500");
        fprintf(stderr, "\u2510\033[0m");

        const char *title = "Toggle Labels";
        int tlen = (int)strlen(title);
        fprintf(stderr, "\033[%d;%dH\033[7m\u2502 ", row0 + 1, col0);
        int pl = (box_w - 4 - tlen) / 2;
        int pr = (box_w - 4 - tlen) - pl;
        for (int i = 0; i < pl; i++) fputc(' ', stderr);
        fprintf(stderr, "%s", title);
        for (int i = 0; i < pr; i++) fputc(' ', stderr);
        fprintf(stderr, " \u2502\033[0m");

        fprintf(stderr, "\033[%d;%dH\033[7m\u251c", row0 + 2, col0);
        for (int i = 0; i < box_w - 2; i++) fprintf(stderr, "\u2500");
        fprintf(stderr, "\u2524\033[0m");

        for (int i = 0; i < pick_count; i++) {
            int sel = (i == pcursor);
            fprintf(stderr, "\033[%d;%dH", row0 + 3 + i, col0);
            if (sel) fprintf(stderr, "\033[7m\033[1m");
            else     fprintf(stderr, "\033[7m");
            fprintf(stderr, "\u2502 [%c] %-*.*s \u2502",
                    pick_on[i] ? 'x' : ' ',
                    inner_w - 5, inner_w - 5,
                    pick_names[i]);
            fprintf(stderr, "\033[0m");
        }

        fprintf(stderr, "\033[%d;%dH\033[7m\u2514", row0 + 3 + pick_count, col0);
        for (int i = 0; i < box_w - 2; i++) fprintf(stderr, "\u2500");
        fprintf(stderr, "\u2518\033[0m");

        const char *foot = " \u2191\u2193=move  Enter=toggle  ESC=done ";
        int flen = (int)strlen(foot);
        if (flen < box_w) {
            int fc = col0 + (box_w - flen) / 2;
            fprintf(stderr, "\033[%d;%dH\033[2m%s\033[0m",
                    row0 + 4 + pick_count, fc, foot);
        }
        fflush(stderr);

        TermKey key = terminal_read_key();
        if (key == TERM_KEY_QUIT || key == TERM_KEY_ESC || key == TERM_KEY_BACK)
            break;
        if (key == TERM_KEY_NEXT_LINE && pcursor < pick_count - 1) pcursor++;
        if (key == TERM_KEY_PREV_LINE && pcursor > 0) pcursor--;
        if (key == TERM_KEY_ENTER || terminal_last_printable() == ' ') {
            /* Toggle the label */
            pick_on[pcursor] = !pick_on[pcursor];
            const char *lid = pick_ids[pcursor];
            int adding = pick_on[pcursor];
            if (adding) {
                label_idx_add(lid, uid);
                local_hdr_update_labels("", uid, &lid, 1, NULL, 0);
            } else {
                label_idx_remove(lid, uid);
                local_hdr_update_labels("", uid, NULL, 0, &lid, 1);
            }
            if (mc) {
                if (strcmp(lid, "STARRED") == 0)
                    mail_client_set_flag(mc, uid, "\\Flagged", adding);
                else
                    mail_client_modify_label(mc, uid, lid, adding);
            }
        }
    }

    for (int i = 0; i < pick_count; i++) { free(pick_ids[i]); free(pick_names[i]); }
    free(pick_ids); free(pick_names); free(pick_on);
}

/* ── Gmail Label List (interactive) ──────────────────────────────────── */

/* System labels in display order.  id = .idx filename, name = display name. */
static const struct { const char *id; const char *name; } gmail_system_labels[] = {
    { "UNREAD",   "Unread"  },
    { "STARRED",  "Starred" },
    { "INBOX",    "Inbox"   },
    { "SENT",     "Sent"    },
    { "DRAFTS",   "Drafts"  },
};
#define GMAIL_SYS_COUNT ((int)(sizeof(gmail_system_labels)/sizeof(gmail_system_labels[0])))

/* Gmail automatic inbox category labels (shown as a separate section) */
static const struct { const char *id; const char *name; } gmail_cat_labels[] = {
    { "CATEGORY_PERSONAL",   "Personal"   },
    { "CATEGORY_SOCIAL",     "Social"     },
    { "CATEGORY_PROMOTIONS", "Promotions" },
    { "CATEGORY_UPDATES",    "Updates"    },
    { "CATEGORY_FORUMS",     "Forums"     },
};
#define GMAIL_CAT_COUNT ((int)(sizeof(gmail_cat_labels)/sizeof(gmail_cat_labels[0])))

static const struct { const char *id; const char *name; } gmail_special_labels[] = {
    { "_nolabel", "Archive" },
    { "_spam",    "Spam"    },
    { "_trash",   "Trash (auto-delete: 30 days)" },
};
#define GMAIL_SPECIAL_COUNT ((int)(sizeof(gmail_special_labels)/sizeof(gmail_special_labels[0])))

/* Build a flat label display list.  Returns count.
 * Each entry has id (for .idx lookup / selection) and display name.
 * section_sep[i] = 1 means a separator line should appear before row i. */
static int build_label_display(
    char ***ids_out, char ***names_out, int **sep_out,
    char **user_labels, int user_count,
    char **cat_labels,  int cat_count)
{
    /* total = system + user + categories + special */
    int total = GMAIL_SYS_COUNT + user_count + cat_count + GMAIL_SPECIAL_COUNT;
    char **ids   = calloc((size_t)total, sizeof(char *));
    char **names = calloc((size_t)total, sizeof(char *));
    int  *seps   = calloc((size_t)total, sizeof(int));
    if (!ids || !names || !seps) { free(ids); free(names); free(seps); return 0; }

    int n = 0;
    /* Section 1: system labels */
    for (int i = 0; i < GMAIL_SYS_COUNT; i++) {
        ids[n]   = strdup(gmail_system_labels[i].id);
        names[n] = strdup(gmail_system_labels[i].name);
        n++;
    }
    /* Section 2: user labels (separator before first) */
    if (user_count > 0) seps[n] = 1;
    for (int i = 0; i < user_count; i++) {
        ids[n]   = strdup(user_labels[i]);
        char *disp = local_gmail_label_name_lookup(user_labels[i]);
        names[n] = disp ? disp : strdup(user_labels[i]);
        n++;
    }
    /* Section 3: inbox category labels (separator before first) */
    if (cat_count > 0) seps[n] = 1;
    for (int i = 0; i < cat_count; i++) {
        ids[n] = strdup(cat_labels[i]);
        /* Map ID to display name */
        const char *disp = cat_labels[i];
        for (int k = 0; k < GMAIL_CAT_COUNT; k++) {
            if (strcmp(cat_labels[i], gmail_cat_labels[k].id) == 0) {
                disp = gmail_cat_labels[k].name;
                break;
            }
        }
        names[n] = strdup(disp);
        n++;
    }
    /* Section 4: special labels (separator before first) */
    seps[n] = 1;
    for (int i = 0; i < GMAIL_SPECIAL_COUNT; i++) {
        ids[n]   = strdup(gmail_special_labels[i].id);
        names[n] = strdup(gmail_special_labels[i].name);
        n++;
    }

    *ids_out   = ids;
    *names_out = names;
    *sep_out   = seps;
    return n;
}

static void free_label_display(char **ids, char **names, int *seps, int count) {
    for (int i = 0; i < count; i++) { free(ids[i]); free(names[i]); }
    free(ids); free(names); free(seps);
}

/* Check if a label name is a system or special label (skip for user list). */
static int is_system_or_special_label(const char *name) {
    for (int i = 0; i < GMAIL_SYS_COUNT; i++)
        if (strcmp(name, gmail_system_labels[i].id) == 0) return 1;
    for (int i = 0; i < GMAIL_SPECIAL_COUNT; i++)
        if (strcmp(name, gmail_special_labels[i].id) == 0) return 1;
    /* Also filter out IMPORTANT and CATEGORY_* which gmail_sync already filters */
    if (strcmp(name, "IMPORTANT") == 0) return 1;
    if (strncmp(name, "CATEGORY_", 9) == 0) return 1;
    if (strcmp(name, "TRASH") == 0 || strcmp(name, "SPAM") == 0) return 1;
    return 0;
}

char *email_service_list_labels_interactive(const Config *cfg,
                                            const char *current_label,
                                            int *go_up) {
    local_store_init(cfg->host, cfg->user);
    if (go_up) *go_up = 0;

    /* Collect user and category labels from locally synced .idx files. */
    char **user_labels = NULL;
    int user_count = 0;
    char **cat_labels = NULL;
    int cat_count = 0;
    {
        char **all_labels = NULL;
        int all_count = 0;
        label_idx_list(&all_labels, &all_count);
        user_labels = calloc(all_count > 0 ? (size_t)all_count : 1, sizeof(char *));
        cat_labels  = calloc(all_count > 0 ? (size_t)all_count : 1, sizeof(char *));
        for (int i = 0; i < all_count; i++) {
            if (strncmp(all_labels[i], "CATEGORY_", 9) == 0)
                cat_labels[cat_count++] = strdup(all_labels[i]);
            else if (!is_system_or_special_label(all_labels[i]))
                user_labels[user_count++] = strdup(all_labels[i]);
            free(all_labels[i]);
        }
        free(all_labels);
    }

    /* Sort user labels alphabetically */
    if (user_count > 1)
        qsort(user_labels, (size_t)user_count, sizeof(char *), cmp_str);

    char **lbl_ids = NULL, **lbl_names = NULL;
    int *lbl_seps = NULL;
    int lbl_count = build_label_display(&lbl_ids, &lbl_names, &lbl_seps,
                                        user_labels, user_count,
                                        cat_labels, cat_count);
    for (int i = 0; i < user_count; i++) free(user_labels[i]);
    free(user_labels);
    for (int i = 0; i < cat_count; i++) free(cat_labels[i]);
    free(cat_labels);

    if (lbl_count == 0) {
        free_label_display(lbl_ids, lbl_names, lbl_seps, 0);
        return NULL;
    }

    int cursor = 0, wstart = 0;
    char *selected = NULL;

    /* Pre-position cursor on current_label */
    if (current_label && *current_label) {
        for (int i = 0; i < lbl_count; i++) {
            if (strcmp(lbl_ids[i], current_label) == 0) {
                cursor = i; break;
            }
        }
    }

    RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();

    for (;;) {
        int trows = terminal_rows();
        int tcols = terminal_cols();
        if (trows <= 0) trows = 24;
        if (tcols <= 0) tcols = 80;
        int wend = 0;  /* computed below, after avail is known */
        /* Name column width: "  " + 6(count) + "  " + name_w = name_w + 10 */
        int name_w = tcols - 12;
        if (name_w < 20) name_w = 20;

        /* Fixed overhead: 1 title + 1 blank + 1 col-header + 1 separator + 1 statusbar = 5.
         * Remaining rows are shared between items and section-separator lines. */
        int avail = (trows > 6) ? trows - 5 : 5;

        if (cursor >= lbl_count) cursor = lbl_count - 1;
        if (cursor < 0) cursor = 0;
        if (cursor < wstart) wstart = cursor;
        /* Advance wstart until cursor is within the visible window, counting
         * separator rows so the window never overflows the terminal. */
        for (;;) {
            int rows = 0, found = 0;
            for (int i = wstart; i < lbl_count && rows < avail; i++) {
                if (lbl_seps[i] && i > wstart) rows++;   /* separator line */
                rows++;                                    /* item line */
                if (i == cursor) { found = 1; break; }
            }
            if (found) break;
            wstart++;
        }

        /* Compute actual wend given avail rows */
        int rows_counted = 0;
        wend = wstart;
        for (int i = wstart; i < lbl_count; i++) {
            int extra = (lbl_seps[i] && i > wstart) ? 1 : 0;
            if (rows_counted + extra + 1 > avail) break;
            rows_counted += extra + 1;
            wend = i + 1;
        }

        printf("\033[H\033[2J");
        {
            char cl[512];
            snprintf(cl, sizeof(cl), "  Labels \u2014 %s  (%d)",
                     cfg->user ? cfg->user : "?", lbl_count);
            printf("\033[7m%s", cl);
            int used = visible_line_cols(cl, cl + strlen(cl));
            for (int p = used; p < tcols; p++) putchar(' ');
            printf("\033[0m\n\n");
        }

        printf("  %6s  %-*s\n", "Count", name_w, "Label");
        printf("  \u2550\u2550\u2550\u2550\u2550\u2550  ");
        print_dbar(name_w);
        printf("\n");

        for (int i = wstart; i < wend; i++) {
            /* Section separator (not before the very first visible item) */
            if (lbl_seps[i] && i > wstart) {
                printf("  \033[2m");
                for (int s = 0; s < tcols - 2; s++) fputs("\u2504", stdout);
                printf("\033[0m\n");
            }

            int cnt = label_idx_count(lbl_ids[i]);
            char cnt_buf[16];
            fmt_thou(cnt_buf, sizeof(cnt_buf), cnt);

            int sel = (i == cursor);
            if (sel) printf("\033[7m");
            printf("  %6s  ", cnt_buf);
            print_padded_col(lbl_names[i], name_w);
            if (sel) printf("\033[K\033[0m");
            printf("\n");
        }
        fflush(stdout);

        {
            char sb[256];
            snprintf(sb, sizeof(sb),
                     "  \u2191\u2193=select  Enter=open  c=create  d=delete  Backspace=accounts  ESC=quit"
                     "  h=help  [%d/%d]",
                     cursor + 1, lbl_count);
            print_statusbar(trows, tcols, sb);
        }

        TermKey key = terminal_read_key();
        fprintf(stderr, "\r\033[K"); fflush(stderr);

        switch (key) {
        case TERM_KEY_BACK:
            if (go_up) *go_up = 1;
            goto labels_done;
        case TERM_KEY_QUIT:
        case TERM_KEY_ESC:
            goto labels_done;
        case TERM_KEY_HOME:
            cursor = 0; wstart = 0;
            break;
        case TERM_KEY_END:
            cursor = lbl_count > 0 ? lbl_count - 1 : 0;
            break;
        case TERM_KEY_NEXT_LINE:
            if (cursor < lbl_count - 1) cursor++;
            break;
        case TERM_KEY_PREV_LINE:
            if (cursor > 0) cursor--;
            break;
        case TERM_KEY_NEXT_PAGE:
            cursor += avail;
            if (cursor >= lbl_count) cursor = lbl_count - 1;
            break;
        case TERM_KEY_PREV_PAGE:
            cursor -= avail;
            if (cursor < 0) cursor = 0;
            break;
        case TERM_KEY_ENTER:
            selected = strdup(lbl_ids[cursor]);
            goto labels_done;
        default: {
            int ch = terminal_last_printable();
            if (ch == 'h' || ch == '?') {
                static const char *help[][2] = {
                    { "\u2191 / \u2193",   "Move cursor up / down"      },
                    { "PgUp / PgDn",       "Page up / down"             },
                    { "Enter",             "Open selected label"        },
                    { "c",                 "Create new label"           },
                    { "d",                 "Delete selected label"      },
                    { "Backspace",         "Back to accounts"           },
                    { "ESC / q",           "Quit"                       },
                    { "h / ?",             "Show this help"             },
                };
                show_help_popup("Label browser shortcuts",
                                help, (int)(sizeof(help)/sizeof(help[0])));
            }
            if (ch == 'c') {
                /* Create new label */
                char new_name[256] = "";
                InputLine il;
                input_line_init(&il, new_name, sizeof(new_name), "");
                int confirmed = input_line_run(&il, trows - 2, "New label name: ");
                if (confirmed && new_name[0]) {
                    if (email_service_create_label(cfg, new_name) == 0) {
                        /* Reload label list on next iteration */
                        free_label_display(lbl_ids, lbl_names, lbl_seps, lbl_count);
                        /* Rebuild label list */
                        char **ul2 = NULL, **cl2 = NULL;
                        int uc2 = 0, cc2 = 0;
                        {
                            char **al2 = NULL;
                            int ac2 = 0;
                            label_idx_list(&al2, &ac2);
                            ul2 = calloc(ac2 > 0 ? (size_t)ac2 : 1, sizeof(char *));
                            cl2 = calloc(ac2 > 0 ? (size_t)ac2 : 1, sizeof(char *));
                            for (int i = 0; i < ac2; i++) {
                                if (strncmp(al2[i], "CATEGORY_", 9) == 0)
                                    cl2[cc2++] = strdup(al2[i]);
                                else if (!is_system_or_special_label(al2[i]))
                                    ul2[uc2++] = strdup(al2[i]);
                                free(al2[i]);
                            }
                            free(al2);
                        }
                        if (uc2 > 1)
                            qsort(ul2, (size_t)uc2, sizeof(char *), cmp_str);
                        lbl_count = build_label_display(&lbl_ids, &lbl_names, &lbl_seps,
                                                        ul2, uc2, cl2, cc2);
                        for (int i = 0; i < uc2; i++) free(ul2[i]);
                        free(ul2);
                        for (int i = 0; i < cc2; i++) free(cl2[i]);
                        free(cl2);
                        if (lbl_count == 0) {
                            free_label_display(lbl_ids, lbl_names, lbl_seps, 0);
                            goto labels_done;
                        }
                    }
                }
            }
            if (ch == 'd' && lbl_count > 0) {
                /* Delete selected label — use the label name as ID (best effort).
                 * TODO: use label ID instead of name for Gmail (ID != name for
                 *       user-defined labels). For IMAP this is correct (name == ID). */
                const char *del_id = lbl_names[cursor];
                if (del_id) {
                    email_service_delete_label(cfg, del_id);
                    /* Rebuild display after deletion */
                    free_label_display(lbl_ids, lbl_names, lbl_seps, lbl_count);
                    char **ul3 = NULL, **cl3 = NULL;
                    int uc3 = 0, cc3 = 0;
                    {
                        char **al3 = NULL;
                        int ac3 = 0;
                        label_idx_list(&al3, &ac3);
                        ul3 = calloc(ac3 > 0 ? (size_t)ac3 : 1, sizeof(char *));
                        cl3 = calloc(ac3 > 0 ? (size_t)ac3 : 1, sizeof(char *));
                        for (int i = 0; i < ac3; i++) {
                            if (strncmp(al3[i], "CATEGORY_", 9) == 0)
                                cl3[cc3++] = strdup(al3[i]);
                            else if (!is_system_or_special_label(al3[i]))
                                ul3[uc3++] = strdup(al3[i]);
                            free(al3[i]);
                        }
                        free(al3);
                    }
                    if (uc3 > 1)
                        qsort(ul3, (size_t)uc3, sizeof(char *), cmp_str);
                    lbl_count = build_label_display(&lbl_ids, &lbl_names, &lbl_seps,
                                                    ul3, uc3, cl3, cc3);
                    for (int i = 0; i < uc3; i++) free(ul3[i]);
                    free(ul3);
                    for (int i = 0; i < cc3; i++) free(cl3[i]);
                    free(cl3);
                    if (cursor >= lbl_count) cursor = lbl_count - 1;
                    if (cursor < 0) cursor = 0;
                    if (lbl_count == 0) {
                        free_label_display(lbl_ids, lbl_names, lbl_seps, 0);
                        goto labels_done;
                    }
                }
            }
            break;
        }
        }
    }
labels_done:
    free_label_display(lbl_ids, lbl_names, lbl_seps, lbl_count);
    return selected;
}

/**
 * Sum unread and flagged counts across all locally-cached folders for one
 * account.  Temporarily switches g_account_base via local_store_init; the
 * caller must restore the correct account after iterating all accounts.
 */
static void get_account_totals(const Config *cfg, int *unseen_out, int *flagged_out) {
    *unseen_out = 0; *flagged_out = 0;
    if (!cfg) return;
    local_store_init(cfg->host, cfg->user);
    if (cfg->gmail_mode) {
        /* Gmail: count from local label index files */
        *unseen_out  = label_idx_count("UNREAD");
        *flagged_out = label_idx_count("STARRED");
        return;
    }
    if (!cfg->host) return;
    int fcount = 0;
    char **flist = local_folder_list_load(&fcount, NULL);
    if (!flist) return;
    for (int i = 0; i < fcount; i++) {
        int total = 0, unseen = 0, flagged = 0;
        manifest_count_folder(flist[i], &total, &unseen, &flagged);
        *unseen_out  += unseen;
        *flagged_out += flagged;
        free(flist[i]);
    }
    free(flist);
}

/** Print one account row; cursor=1 draws the selection arrow. */
/**
 * Format a URL for display, appending the default port if none is present.
 * defport is used when the URL has no ":port" after the host.
 */
static void fmt_url_with_port(const char *url, int defport, char *out, size_t size) {
    if (!url || !url[0]) { out[0] = '\0'; return; }
    const char *proto_end = strstr(url, "://");
    const char *host = proto_end ? proto_end + 3 : url;
    if (strchr(host, ':')) {
        /* Port already present in URL */
        snprintf(out, size, "%s", url);
    } else {
        snprintf(out, size, "%s:%d", url, defport);
    }
}

static void print_account_row(const Config *cfg, int cursor,
                               int unseen, int flagged,
                               int imap_w, int smtp_w) {
    const char *user = cfg->user ? cfg->user : "(unknown)";
    const char *type = cfg->gmail_mode ? "Gmail" : "IMAP";

    /* Server: Gmail shows "Gmail API", IMAP shows host:port */
    char server_buf[256];
    if (cfg->gmail_mode) {
        snprintf(server_buf, sizeof(server_buf), "Gmail API");
    } else {
        fmt_url_with_port(cfg->host, 993, server_buf, sizeof(server_buf));
    }

    /* Build SMTP display string (no ANSI — safe to truncate with %.*s) */
    char smtp_buf[256];
    int smtp_configured;
    if (cfg->gmail_mode) {
        snprintf(smtp_buf, sizeof(smtp_buf), "Gmail API");
        smtp_configured = 1;
    } else {
        smtp_configured = cfg->smtp_host && cfg->smtp_host[0];
        if (smtp_configured) {
            if (cfg->smtp_port) {
                const char *proto_end = strstr(cfg->smtp_host, "://");
                const char *smtp_host_part = proto_end ? proto_end + 3 : cfg->smtp_host;
                if (strchr(smtp_host_part, ':'))
                    snprintf(smtp_buf, sizeof(smtp_buf), "%s", cfg->smtp_host);
                else
                    snprintf(smtp_buf, sizeof(smtp_buf), "%s:%d",
                             cfg->smtp_host, cfg->smtp_port);
            } else {
                int defport = (strncmp(cfg->smtp_host, "smtps://", 8) == 0) ? 465 : 587;
                fmt_url_with_port(cfg->smtp_host, defport, smtp_buf, sizeof(smtp_buf));
            }
        } else {
            snprintf(smtp_buf, sizeof(smtp_buf), "\u2014");
        }
    }

    char u[16], f[16];
    fmt_thou(u, sizeof(u), unseen);
    fmt_thou(f, sizeof(f), flagged);

    if (cursor) {
        printf("  \033[1m\u2192 %6s  %7s  %-32.32s  %-5s  %-*.*s  %.*s\033[0m\n",
               u, f, user, type, imap_w, imap_w, server_buf, smtp_w, smtp_buf);
    } else if (!smtp_configured) {
        printf("    %6s  %7s  %-32.32s  %-5s  %-*.*s  \033[2m%.*s\033[0m\n",
               u, f, user, type, imap_w, imap_w, server_buf, smtp_w, smtp_buf);
    } else {
        printf("    %6s  %7s  %-32.32s  %-5s  %-*.*s  %.*s\n",
               u, f, user, type, imap_w, imap_w, server_buf, smtp_w, smtp_buf);
    }
}

int email_service_account_interactive(Config **cfg_out, int *cursor_inout) {
    *cfg_out = NULL;
    RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();
    (void)tui_raw;

    int cursor = (cursor_inout && *cursor_inout > 0) ? *cursor_inout : 0;

    for (;;) {
        /* Reload account list on every iteration (list may change after add/delete) */
        int count = 0;
        AccountEntry *accounts = config_list_accounts(&count);

        int trows = terminal_rows();
        int tcols = terminal_cols();
        if (trows <= 0) trows = 24;
        if (tcols <= 0) tcols = 80;
        if (cursor >= count) cursor = count > 0 ? count - 1 : 0;

        /* Compute unread/flagged totals for each account (local manifests only) */
        int *acc_unseen  = calloc(count > 0 ? (size_t)count : 1, sizeof(int));
        int *acc_flagged = calloc(count > 0 ? (size_t)count : 1, sizeof(int));
        for (int i = 0; i < count; i++)
            get_account_totals(accounts[i].cfg, &acc_unseen[i], &acc_flagged[i]);

        /* Column widths:
         * Fixed overhead: 4(indent) + 6(unread) + 2 + 7(flagged) + 2
         *                 + 32(account) + 2 + 5(type) + 2 + 2(sep) = 64
         * Remaining split evenly between Server and SMTP columns. */
        int avail = tcols - 64;
        if (avail < 0) avail = 0;
        int imap_w = avail / 2;
        int smtp_w = avail - imap_w;
        if (imap_w < 10) imap_w = 10;
        if (smtp_w <  8) smtp_w =  8;

        printf("\033[H\033[2J");
        {
            char cl[128];
            snprintf(cl, sizeof(cl), "  Email Accounts (%d)", count);
            printf("\033[7m%s", cl);
            int used = visible_line_cols(cl, cl + strlen(cl));
            for (int p = used; p < tcols; p++) putchar(' ');
            printf("\033[0m\n\n");
        }

        if (count == 0) {
            printf("  No accounts configured.\n");
        } else {
            printf("    %6s  %7s  %-32s  %-5s  %-*s  %s\n",
                   "Unread", "Flagged", "Account", "Type", imap_w, "Server", "Send via");
            printf("    \u2550\u2550\u2550\u2550\u2550\u2550  \u2550\u2550\u2550\u2550\u2550\u2550\u2550  ");
            print_dbar(32);
            printf("  \u2550\u2550\u2550\u2550\u2550  ");
            print_dbar(imap_w);
            printf("  ");
            print_dbar(smtp_w);
            printf("\n");
            for (int i = 0; i < count; i++)
                print_account_row(accounts[i].cfg, i == cursor,
                                  acc_unseen[i], acc_flagged[i],
                                  imap_w, smtp_w);
        }
        fflush(stdout);

        char sb[256];
        snprintf(sb, sizeof(sb),
                 "  \u2191\u2193=select  Enter=open  n=add  d=delete*  i=IMAP  e=SMTP  ESC=quit  (*keeps local data)");
        print_statusbar(trows, tcols, sb);

        TermKey key = terminal_read_key();
        fprintf(stderr, "\r\033[K"); fflush(stderr);

        int ch = terminal_last_printable();

#define ACC_FREE() do { free(acc_unseen); free(acc_flagged); \
                        config_free_account_list(accounts, count); } while(0)

        if (key == TERM_KEY_QUIT || key == TERM_KEY_ESC) {
            if (cursor_inout) *cursor_inout = cursor;
            ACC_FREE(); return 0;
        }
        if (key == TERM_KEY_BACK) {
            /* Backspace has no meaning at the top-level accounts screen; ignore. */
            ACC_FREE(); continue;
        }
        if (key == TERM_KEY_HOME) {
            cursor = 0;
            ACC_FREE(); continue;
        }
        if (key == TERM_KEY_END) {
            cursor = count > 0 ? count - 1 : 0;
            ACC_FREE(); continue;
        }
        if (key == TERM_KEY_NEXT_LINE || key == TERM_KEY_NEXT_PAGE) {
            if (cursor < count - 1) cursor++;
            ACC_FREE(); continue;
        }
        if (key == TERM_KEY_PREV_LINE || key == TERM_KEY_PREV_PAGE) {
            if (cursor > 0) cursor--;
            ACC_FREE(); continue;
        }
        if (key == TERM_KEY_ENTER && count > 0) {
            if (cursor_inout) *cursor_inout = cursor;
            *cfg_out = accounts[cursor].cfg;
            accounts[cursor].cfg = NULL; /* transfer ownership */
            ACC_FREE(); return 1;
        }

        /* Printable keys */
        if (ch == 'h' || ch == '?') {
            static const char *help[][2] = {
                { "\u2191 / \u2193",   "Move cursor up / down"      },
                { "Enter",            "Open selected account"       },
                { "n",               "Add new account"             },
                { "d",               "Delete selected account"     },
                { "i",               "Edit IMAP for account"       },
                { "e",               "Edit SMTP for account"       },
                { "ESC / q",         "Quit"                        },
                { "h / ?",           "Show this help"              },
            };
            show_help_popup("Accounts shortcuts",
                            help, (int)(sizeof(help)/sizeof(help[0])));
            ACC_FREE(); continue;
        }
        if (ch == 'i' && count > 0) {
            if (cursor_inout) *cursor_inout = cursor;
            *cfg_out = accounts[cursor].cfg;
            accounts[cursor].cfg = NULL;
            ACC_FREE(); return 4;
        }
        if (ch == 'e' && count > 0) {
            if (cursor_inout) *cursor_inout = cursor;
            *cfg_out = accounts[cursor].cfg;
            accounts[cursor].cfg = NULL;
            ACC_FREE(); return 2;
        }
        if (ch == 'n') {
            ACC_FREE(); return 3;  /* caller runs setup wizard */
        }
        if (ch == 'd' && count > 0) {
            const char *name = accounts[cursor].name;

            /* Compute local data directory (NOT deleted) */
            const char *data_base = platform_data_dir();
            char data_path[2048] = "";
            if (data_base && name && name[0])
                snprintf(data_path, sizeof(data_path),
                         "%s/email-cli/accounts/%s", data_base, name);

            config_delete_account(name);
            ACC_FREE();
            if (cursor > 0) cursor--;

            /* Show preservation notice */
            if (data_path[0]) {
                int trows2 = terminal_rows();
                int tcols2 = terminal_cols();
                char notice[2200];
                snprintf(notice, sizeof(notice),
                         "Account removed. Local messages preserved: %s", data_path);
                print_infoline(trows2, tcols2, notice);
            }
            continue;  /* re-render */
        }

        ACC_FREE();
    }
#undef ACC_FREE
}

int email_service_read(const Config *cfg, const char *uid, int pager, int page_size) {
    char *raw = NULL;

    if (local_msg_exists(cfg->folder, uid)) {
        logger_log(LOG_DEBUG, "Cache hit for UID %s in %s", uid, cfg->folder);
        raw = local_msg_load(cfg->folder, uid);
    } else if (cfg->sync_interval > 0) {
        /* cron/offline mode: serve only from local cache; do not connect */
        fprintf(stderr, "Could not load message UID %s.\n", uid);
        return -1;
    } else {
        raw = fetch_uid_content_in(cfg, cfg->folder, uid, 0);
        if (raw) {
            local_msg_save(cfg->folder, uid, raw, strlen(raw));
            local_index_update(cfg->folder, uid, raw);
        }
    }

    if (!raw) { fprintf(stderr, "Could not load message UID %s.\n", uid); return -1; }

    char *from_raw = mime_get_header(raw, "From");
    char *from     = from_raw ? mime_decode_words(from_raw) : NULL;
    free(from_raw);
    char *subj_raw = mime_get_header(raw, "Subject");
    char *subject  = subj_raw ? mime_decode_words(subj_raw) : NULL;
    free(subj_raw);
    char *date_raw = mime_get_header(raw, "Date");
    char *date     = date_raw ? mime_format_date(date_raw) : NULL;
    free(date_raw);
    char *ro_labels = cfg->gmail_mode ? local_hdr_get_labels("", uid) : NULL;

    print_show_headers(from, subject, date, ro_labels);

    int term_cols_show = pager ? terminal_cols() : SHOW_WIDTH;
    int wrap_cols = term_cols_show > SHOW_WIDTH ? SHOW_WIDTH : term_cols_show;

    char *body = NULL;
    char *html_raw = mime_get_html_part(raw);
    if (html_raw) {
        body = html_render(html_raw, wrap_cols, pager ? 1 : 0);
        free(html_raw);
    }
    if (!body) {
        char *plain = mime_get_text_body(raw);
        if (plain) {
            body = word_wrap(plain, wrap_cols);
            if (!body) body = plain;
            else free(plain);
        }
    }
    const char *body_text = body ? body : "(no readable text body)";

#define SHOW_HDR_LINES 5
    if (!pager || page_size <= SHOW_HDR_LINES) {
        printf("%s\n", body_text);
    } else {
        int body_vrows  = count_visual_rows(body_text, term_cols_show);
        int rows_avail  = page_size - SHOW_HDR_LINES;
        int total_pages = (body_vrows + rows_avail - 1) / rows_avail;
        if (total_pages < 1) total_pages = 1;

        /* Enter raw mode for the pager loop; pager_prompt calls terminal_read_key
         * which requires raw mode to be already active. */
        RAII_TERM_RAW TermRawState *show_raw = terminal_raw_enter();

        for (int cur_line = 0, show_displayed = 0; ; ) {
            if (show_displayed) {
                printf("\033[0m\033[H\033[2J");   /* reset attrs + clear screen */
                print_show_headers(from, subject, date, ro_labels);
            }
            show_displayed = 1;
            print_body_page(body_text, cur_line, rows_avail, term_cols_show);
            printf("\033[0m");                     /* close any open ANSI from body */
            fflush(stdout);

            if (cur_line == 0 && cur_line + rows_avail >= body_vrows) break;

            int cur_page = cur_line / rows_avail + 1;
            int delta = pager_prompt(cur_page, total_pages, rows_avail, page_size, wrap_cols);
            if (delta == 0) break;
            cur_line += delta;
            if (cur_line < 0) cur_line = 0;
            if (cur_line >= body_vrows) break;
        }
        (void)show_raw; /* cleaned up automatically via RAII_TERM_RAW */
    }
#undef SHOW_HDR_LINES

    free(body); free(from); free(subject); free(date); free(ro_labels); free(raw);
    return 0;
}

/* ── Sync progress callback ──────────────────────────────────────────────── */

typedef struct {
    int    loop_i;     /* 1-based index of current UID in the loop */
    int    loop_total; /* total UIDs in this folder */
    char   uid[17];
} SyncProgressCtx;

static void fmt_size(char *buf, size_t bufsz, size_t bytes) {
    if (bytes >= 1024 * 1024)
        snprintf(buf, bufsz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(buf, bufsz, "%zu KB", bytes / 1024);
}

static void sync_progress_cb(size_t received, size_t total, void *ctx) {
    SyncProgressCtx *p = ctx;
    char recv_s[32], total_s[32];
    fmt_size(recv_s,  sizeof(recv_s),  received);
    fmt_size(total_s, sizeof(total_s), total);
    printf("  [%d/%d] UID %s  %s / %s ...\r",
           p->loop_i, p->loop_total, p->uid, recv_s, total_s);
    fflush(stdout);
}

int email_service_sync(const Config *cfg) {
    /* ── PID-file lock: exit immediately if another sync is running ──────── */
    char pid_path[2048] = {0};
    const char *cache_base = platform_cache_dir();
    if (cache_base)
        snprintf(pid_path, sizeof(pid_path),
                 "%s/email-cli/sync.pid", cache_base);

    if (pid_path[0]) {
        {
            RAII_FILE FILE *pf = fopen(pid_path, "r");
            if (pf) {
                int other = 0;
                if (fscanf(pf, "%d", &other) != 1) other = 0;
                if (other > 0 && (pid_t)other != platform_getpid() &&
                    platform_pid_is_program((pid_t)other, "email-cli")) {
                    fprintf(stderr,
                            "email-cli sync is already running (PID %d). Skipping.\n",
                            other);
                    return 0;
                }
            }
        }
        /* Write our own PID */
        RAII_FILE FILE *pf = fopen(pid_path, "w");
        if (pf) fprintf(pf, "%d\n", (int)platform_getpid());
    }

    /* ── Gmail: delegate to gmail_sync (flat store + label indexes) ────── */
    if (cfg->gmail_mode) {
        GmailClient *gc = gmail_connect((Config *)cfg);
        if (!gc) {
            fprintf(stderr, "sync: could not connect to Gmail API.\n");
            if (pid_path[0]) unlink(pid_path);
            return -1;
        }
        int rc = gmail_sync(gc);
        gmail_disconnect(gc);
        if (pid_path[0]) unlink(pid_path);
        return rc;
    }

    /* ── IMAP: sync all folders individually ─────────────────────────── */
    int folder_count = 0;
    char sep = '.';
    /* Always fetch from server during sync to get the latest folder list */
    char **folders = fetch_folder_list_from_server(cfg, &folder_count, &sep);
    if (!folders || folder_count == 0) {
        fprintf(stderr, "sync: could not retrieve folder list.\n");
        if (folders) free(folders);
        if (pid_path[0]) unlink(pid_path);
        return -1;
    }
    qsort(folders, (size_t)folder_count, sizeof(char *), cmp_str);

    /* Persist folder list so the next 'folders' command is instant */
    local_folder_list_save((const char **)folders, folder_count, sep);

    int total_fetched = 0, total_skipped = 0, errors = 0;

    /* One shared mail client connection for all folder operations */
    RAII_MAIL MailClient *sync_mc = make_mail(cfg);
    if (!sync_mc) {
        fprintf(stderr, "sync: could not connect to mail server.\n");
        for (int i = 0; i < folder_count; i++) free(folders[i]);
        free(folders);
        if (pid_path[0]) unlink(pid_path);
        return -1;
    }

    for (int fi = 0; fi < folder_count; fi++) {
        const char *folder = folders[fi];
        printf("Syncing %s ...\n", folder);
        fflush(stdout);

        if (mail_client_select(sync_mc, folder) != 0) {
            fprintf(stderr, "  WARN: SELECT failed for %s\n", folder);
            errors++;
            continue;
        }

        char (*uids)[17] = NULL;
        int  uid_count = 0;
        if (mail_client_search(sync_mc, MAIL_SEARCH_ALL, &uids, &uid_count) != 0) {
            fprintf(stderr, "  WARN: SEARCH ALL failed for %s\n", folder);
            errors++;
            continue;
        }
        if (uid_count == 0) {
            printf("  (empty)\n");
            free(uids);
            continue;
        }

        /* Load or create manifest for this folder */
        Manifest *manifest = manifest_load(folder);
        if (!manifest) {
            manifest = calloc(1, sizeof(Manifest));
            if (!manifest) {
                fprintf(stderr, "  WARN: out of memory for manifest %s\n", folder);
                free(uids);
                errors++;
                continue;
            }
        }

        /* Flush pending local flag changes to the server before reading state */
        {
            int pcount = 0;
            PendingFlag *pending = local_pending_flag_load(folder, &pcount);
            if (pending && pcount > 0) {
                for (int pi = 0; pi < pcount; pi++)
                    mail_client_set_flag(sync_mc, pending[pi].uid,
                                        pending[pi].flag_name, pending[pi].add);
                local_pending_flag_clear(folder);
            }
            free(pending);
        }

        /* Get UNSEEN set to mark entries */
        char (*unseen_uids)[17] = NULL;
        int  unseen_count = 0;
        if (mail_client_search(sync_mc, MAIL_SEARCH_UNREAD, &unseen_uids, &unseen_count) != 0)
            unseen_count = 0;

        char (*flagged_uids)[17] = NULL;
        int flagged_count = 0;
        mail_client_search(sync_mc, MAIL_SEARCH_FLAGGED, &flagged_uids, &flagged_count);

        char (*done_uids)[17] = NULL;
        int done_count = 0;
        mail_client_search(sync_mc, MAIL_SEARCH_DONE, &done_uids, &done_count);

        /* Evict deleted messages from manifest */
        manifest_retain(manifest, (const char (*)[17])uids, uid_count);

        int fetched = 0, skipped = 0;
        for (int i = 0; i < uid_count; i++) {
            const char *uid = uids[i];
            int uid_flags = 0;
            for (int j = 0; j < unseen_count;  j++)
                if (strcmp(unseen_uids[j],  uid) == 0) { uid_flags |= MSG_FLAG_UNSEEN;  break; }
            for (int j = 0; j < flagged_count; j++)
                if (strcmp(flagged_uids[j], uid) == 0) { uid_flags |= MSG_FLAG_FLAGGED; break; }
            for (int j = 0; j < done_count;    j++)
                if (strcmp(done_uids[j],    uid) == 0) { uid_flags |= MSG_FLAG_DONE;    break; }

            /* Show progress BEFORE the potentially slow network fetch */
            printf("  [%d/%d] UID %s...\r", i + 1, uid_count, uid);
            fflush(stdout);

            /* Fetch full body if not cached */
            if (!local_msg_exists(folder, uid)) {
                SyncProgressCtx pctx = { i + 1, uid_count, {0} };
                memcpy(pctx.uid, uid, 17);
                mail_client_set_progress(sync_mc, sync_progress_cb, &pctx);
                char *raw = mail_client_fetch_body(sync_mc, uid);
                mail_client_set_progress(sync_mc, NULL, NULL);
                if (raw) {
                    /* Cache the header section extracted from the full body so
                     * the subsequent manifest update needs no extra IMAP round-trip. */
                    if (!local_hdr_exists(folder, uid)) {
                        const char *sep4 = strstr(raw, "\r\n\r\n");
                        size_t hlen = sep4 ? (size_t)(sep4 - raw + 4) : strlen(raw);
                        local_hdr_save(folder, uid, raw, hlen);
                    }
                    local_msg_save(folder, uid, raw, strlen(raw));
                    local_index_update(folder, uid, raw);
                    free(raw);
                    fetched++;
                } else {
                    fprintf(stderr, "  WARN: failed to fetch UID %s in %s\n", uid, folder);
                    errors++;
                    continue;
                }
            } else {
                skipped++;
            }

            /* Update manifest entry (headers from local cache — now always warm) */
            ManifestEntry *me = manifest_find(manifest, uid);
            if (!me) {
                char *hdrs   = fetch_uid_headers_via(sync_mc, folder, uid);
                char *fr_raw = hdrs ? mime_get_header(hdrs, "From")    : NULL;
                char *fr     = fr_raw ? mime_decode_words(fr_raw)      : strdup("");
                free(fr_raw);
                char *su_raw = hdrs ? mime_get_header(hdrs, "Subject") : NULL;
                char *su     = su_raw ? mime_decode_words(su_raw)      : strdup("");
                free(su_raw);
                char *dt_raw = hdrs ? mime_get_header(hdrs, "Date")    : NULL;
                char *dt     = dt_raw ? mime_format_date(dt_raw)       : strdup("");
                free(dt_raw);
                free(hdrs);
                manifest_upsert(manifest, uid, fr, su, dt, uid_flags);
            } else {
                /* update flags on existing entry */
                me->flags = uid_flags;
            }

            printf("  [%d/%d] UID %s   \r", i + 1, uid_count, uid);
            fflush(stdout);
        }
        free(unseen_uids);
        free(flagged_uids);
        free(done_uids);
        manifest_save(folder, manifest);
        manifest_free(manifest);
        free(uids);

        printf("\r\033[K  %d fetched, %d already stored%s\n",
               fetched, skipped, errors ? " (some errors)" : "");
        total_fetched += fetched;
        total_skipped += skipped;
    }

    for (int i = 0; i < folder_count; i++) free(folders[i]);
    free(folders);

    printf("\nSync complete: %d fetched, %d already stored", total_fetched, total_skipped);
    if (errors) printf(", %d errors", errors);
    printf("\n");

    /* Release PID lock */
    if (pid_path[0]) unlink(pid_path);

    return errors ? -1 : 0;
}

int email_service_sync_all(const char *only_account) {
    int count = 0;
    AccountEntry *accounts = config_list_accounts(&count);
    if (!accounts || count == 0) {
        fprintf(stderr, "No accounts configured.\n");
        config_free_account_list(accounts, count);
        return -1;
    }

    int errors = 0;
    int synced = 0;
    for (int i = 0; i < count; i++) {
        if (only_account && only_account[0] &&
            strcmp(accounts[i].name, only_account) != 0)
            continue;
        if (count > 1)
            printf("\n=== Syncing account: %s ===\n", accounts[i].name);
        local_store_init(accounts[i].cfg->host, accounts[i].cfg->user);
        if (email_service_sync(accounts[i].cfg) < 0)
            errors++;
        synced++;
    }
    config_free_account_list(accounts, count);

    if (synced == 0) {
        fprintf(stderr, "Account '%s' not found.\n",
                only_account ? only_account : "");
        return -1;
    }
    return errors > 0 ? -1 : 0;
}

int email_service_rebuild_indexes(const char *only_account) {
    int count = 0;
    AccountEntry *accounts = config_list_accounts(&count);
    if (!accounts || count == 0) {
        fprintf(stderr, "No accounts configured.\n");
        config_free_account_list(accounts, count);
        return -1;
    }

    int errors = 0, done = 0;
    for (int i = 0; i < count; i++) {
        if (only_account && only_account[0] &&
            strcmp(accounts[i].name, only_account) != 0)
            continue;
        if (!accounts[i].cfg->gmail_mode) {
            printf("Account %s: IMAP accounts do not use label indexes — skipping.\n",
                   accounts[i].name);
            done++;
            continue;
        }
        printf("=== Rebuilding indexes: %s ===\n", accounts[i].name);
        local_store_init(accounts[i].cfg->host, accounts[i].cfg->user);
        if (gmail_sync_rebuild_indexes() < 0)
            errors++;
        done++;
    }
    config_free_account_list(accounts, count);

    if (done == 0) {
        fprintf(stderr, "Account '%s' not found.\n",
                only_account ? only_account : "");
        return -1;
    }
    return errors > 0 ? -1 : 0;
}

int email_service_cron_setup(const Config *cfg) {

    /* Find the path to this binary */
    char self_path[1024] = {0};
    if (platform_executable_path(self_path, sizeof(self_path)) != 0) {
        fprintf(stderr, "Cannot determine binary path.\n");
        return -1;
    }

    /* Build path to email-sync (same directory as current binary) */
    char sync_bin[1024] = "email-sync";
    char *last_slash = strrchr(self_path, '/');
    if (last_slash)
        snprintf(sync_bin, sizeof(sync_bin), "%.*s/email-sync",
                 (int)(last_slash - self_path), self_path);

    /* Build the cron line */
    char cron_line[2048];
    snprintf(cron_line, sizeof(cron_line),
             "*/%d * * * * %s >> ~/.cache/email-cli/sync.log 2>&1",
             cfg->sync_interval, sync_bin);

    /* Read existing crontab */
    char existing[65536] = {0};
    size_t total = 0;
    {
        RAII_PFILE FILE *fp = popen("crontab -l 2>/dev/null", "r");
        if (fp) {
            size_t n2;
            while ((n2 = fread(existing + total, 1, sizeof(existing) - total - 1, fp)) > 0)
                total += n2;
        }
    }
    existing[total] = '\0';

    /* Check if already present (email-sync or legacy email-cli sync) */
    if (strstr(existing, "email-sync") ||
        (strstr(existing, "email-cli") && strstr(existing, " sync"))) {
        printf("Cron job already installed. "
               "Run 'email-sync cron remove' first to change the interval.\n");
        return 0;
    }

    /* Append our line (ensure existing ends with newline) */
    if (total > 0 && existing[total - 1] != '\n')
        strncat(existing, "\n", sizeof(existing) - total - 1);
    strncat(existing, cron_line, sizeof(existing) - strlen(existing) - 1);
    strncat(existing, "\n", sizeof(existing) - strlen(existing) - 1);

    RAII_PFILE FILE *cp = popen("crontab -", "w");
    if (!cp) {
        fprintf(stderr, "Failed to update crontab.\n");
        return -1;
    }
    fputs(existing, cp);
    int rc = pclose(cp);
    cp = NULL; /* prevent RAII double-close */
    if (rc != 0) {
        fprintf(stderr, "crontab update failed (exit %d).\n", rc);
        return -1;
    }

    printf("Cron job installed: %s\n", cron_line);
    return 0;
}

int email_service_cron_remove(void) {
    char existing[65536] = {0};
    size_t total = 0;
    {
        RAII_PFILE FILE *fp = popen("crontab -l 2>/dev/null", "r");
        if (fp) {
            size_t n;
            while ((n = fread(existing + total, 1, sizeof(existing) - total - 1, fp)) > 0)
                total += n;
        }
    }
    existing[total] = '\0';

#define IS_SYNC_LINE(s) \
    (strstr((s), "email-sync") || \
     (strstr((s), "email-cli") && strstr((s), " sync")))

    if (!IS_SYNC_LINE(existing)) {
        printf("No email-sync cron entry found.\n");
        return 0;
    }

    /* Filter out sync cron lines */
    char filtered[65536] = {0};
    size_t flen = 0;
    char *p = existing;
    while (*p) {
        char *nl = strchr(p, '\n');
        char *end = nl ? nl : p + strlen(p);
        char saved = *end; *end = '\0';
        if (!IS_SYNC_LINE(p)) {
            size_t llen = strlen(p);
            if (flen + llen + 2 < sizeof(filtered)) {
                memcpy(filtered + flen, p, llen);
                flen += llen;
                filtered[flen++] = '\n';
                filtered[flen]   = '\0';
            }
        }
        *end = saved;
        p = nl ? nl + 1 : end;
    }

    RAII_PFILE FILE *cp = popen("crontab -", "w");
    if (!cp) {
        fprintf(stderr, "Failed to update crontab.\n");
        return -1;
    }
    fputs(filtered, cp);
    int rc = pclose(cp);
    cp = NULL; /* prevent RAII double-close */
    if (rc != 0) {
        fprintf(stderr, "crontab update failed.\n");
        return -1;
    }

    printf("Cron job removed.\n");
    return 0;
}

int email_service_cron_status(void) {
    RAII_PFILE FILE *fp = popen("crontab -l 2>/dev/null", "r");
    if (!fp) {
        printf("No crontab found for this user.\n");
        return 0;
    }
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (IS_SYNC_LINE(line)) {
            if (!found) printf("Cron entry found:\n");
            printf("  %s", line);
            found = 1;
        }
    }
    if (!found)
        printf("No email-sync cron entry found.\n");
#undef IS_SYNC_LINE
    return 0;
}

/* ── Attachment service functions ───────────────────────────────────── */

/* Load raw message for uid (cache or fetch). Returns heap string or NULL. */
static char *load_raw_message(const Config *cfg, const char *uid) {
    if (local_msg_exists(cfg->folder, uid)) {
        return local_msg_load(cfg->folder, uid);
    }
    char *raw = fetch_uid_content_in(cfg, cfg->folder, uid, 0);
    if (raw) {
        local_msg_save(cfg->folder, uid, raw, strlen(raw));
        local_index_update(cfg->folder, uid, raw);
    }
    return raw;
}

char *email_service_fetch_raw(const Config *cfg, const char *uid) {
    return load_raw_message(cfg, uid);
}

int email_service_list_attachments(const Config *cfg, const char *uid) {
    char *raw = load_raw_message(cfg, uid);
    if (!raw) {
        fprintf(stderr, "Could not load message UID %s.\n", uid);
        return -1;
    }
    int count = 0;
    MimeAttachment *atts = mime_list_attachments(raw, &count);
    free(raw);
    if (count == 0) {
        printf("No attachments.\n");
        mime_free_attachments(atts, count);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        const char *name = atts[i].filename ? atts[i].filename : "(no name)";
        size_t sz = atts[i].size;
        if (sz >= 1024 * 1024)
            printf("%-40s  %.1f MB\n", name, (double)sz / (1024.0 * 1024.0));
        else if (sz >= 1024)
            printf("%-40s  %.0f KB\n", name, (double)sz / 1024.0);
        else
            printf("%-40s  %zu B\n", name, sz);
    }
    mime_free_attachments(atts, count);
    return 0;
}

int email_service_save_attachment(const Config *cfg, const char *uid,
                                  const char *name, const char *outdir) {
    char *raw = load_raw_message(cfg, uid);
    if (!raw) {
        fprintf(stderr, "Could not load message UID %s.\n", uid);
        return -1;
    }
    int count = 0;
    MimeAttachment *atts = mime_list_attachments(raw, &count);
    free(raw);
    if (count == 0) {
        fprintf(stderr, "Message UID %s has no attachments.\n", uid);
        mime_free_attachments(atts, count);
        return -1;
    }

    /* Find attachment by filename (case-sensitive). */
    int idx = -1;
    for (int i = 0; i < count; i++) {
        const char *fn = atts[i].filename ? atts[i].filename : "";
        if (strcmp(fn, name) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        fprintf(stderr, "Attachment '%s' not found in message UID %s.\n", name, uid);
        mime_free_attachments(atts, count);
        return -1;
    }

    /* Build destination path. */
    const char *dir = outdir ? outdir : attachment_save_dir();
    char *dir_heap = NULL;
    if (!outdir) dir_heap = (char *)dir; /* attachment_save_dir returns heap */

    char *safe = safe_filename_for_path(name);
    char dest[2048];
    snprintf(dest, sizeof(dest), "%s/%s", dir, safe ? safe : "attachment");
    free(safe);

    int rc = mime_save_attachment(&atts[idx], dest);
    if (rc == 0)
        printf("Saved: %s\n", dest);
    else
        fprintf(stderr, "Failed to save attachment to %s\n", dest);

    mime_free_attachments(atts, count);
    free(dir_heap);
    return rc;
}

/* ── Flag / label service functions ─────────────────────────────────── */

int email_service_set_flag(const Config *cfg, const char *uid,
                           const char *folder, int flag_bit, int add) {
    const char *use_folder = folder ? folder : (cfg->folder ? cfg->folder : "INBOX");

    /* Determine IMAP flag name and effective add direction */
    const char *flag_name;
    int imap_add;
    if (flag_bit == MSG_FLAG_UNSEEN) {
        flag_name = "\\Seen";
        imap_add  = !add;  /* add UNSEEN = remove \Seen */
    } else if (flag_bit == MSG_FLAG_FLAGGED) {
        flag_name = "\\Flagged";
        imap_add  = add;
    } else if (flag_bit == MSG_FLAG_DONE) {
        flag_name = "$Done";
        imap_add  = add;
    } else {
        fprintf(stderr, "Error: Unknown flag bit %d.\n", flag_bit);
        return -1;
    }

    /* Update local manifest */
    Manifest *m = manifest_load(use_folder);
    if (m) {
        ManifestEntry *me = manifest_find(m, uid);
        if (me) {
            if (add)
                me->flags |= flag_bit;
            else
                me->flags &= ~flag_bit;
        }
        manifest_save(use_folder, m);
        manifest_free(m);
    }

    /* Update Gmail label indexes and .hdr labels CSV if in Gmail mode.
     * Both .idx AND the labels field in .hdr must be kept in sync so that
     * rebuild_label_indexes() (run during every full sync) does not undo
     * locally applied flag changes. */
    if (cfg->gmail_mode) {
        const char *lbl = NULL;
        if (flag_bit == MSG_FLAG_UNSEEN)  lbl = "UNREAD";
        else if (flag_bit == MSG_FLAG_FLAGGED) lbl = "STARRED";

        if (lbl) {
            if (add) {
                label_idx_add(lbl, uid);
                local_hdr_update_labels("", uid, &lbl, 1, NULL, 0);
            } else {
                label_idx_remove(lbl, uid);
                local_hdr_update_labels("", uid, NULL, 0, &lbl, 1);
            }
        }

        /* Also update the flags integer field in .hdr */
        Manifest *m2 = manifest_load(use_folder);
        if (m2) {
            ManifestEntry *me2 = manifest_find(m2, uid);
            if (me2)
                local_hdr_update_flags("", uid, me2->flags);
            manifest_free(m2);
        }
    }

    /* Enqueue to pending flag queue */
    local_pending_flag_add(use_folder, uid, flag_name, imap_add);

    /* Synchronous server push */
    MailClient *mc = make_mail(cfg);
    if (mc) {
        if (mail_client_select(mc, use_folder) == 0)
            mail_client_set_flag(mc, uid, flag_name, imap_add);
        mail_client_free(mc);
    } else {
        fprintf(stderr, "Warning: Could not connect. Change queued for next sync.\n");
    }

    return 0;
}

int email_service_set_label(const Config *cfg, const char *uid,
                            const char *label, int add) {
    if (!cfg->gmail_mode) {
        fprintf(stderr, "Error: label operations require Gmail mode.\n");
        return -1;
    }

    MailClient *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect to server.\n");
        return -1;
    }
    int rc = mail_client_modify_label(mc, uid, label, add);
    mail_client_free(mc);

    if (add) {
        label_idx_add(label, uid);
        local_hdr_update_labels("", uid, &label, 1, NULL, 0);
    } else {
        label_idx_remove(label, uid);
        local_hdr_update_labels("", uid, NULL, 0, &label, 1);
    }

    return rc;
}

int email_service_list_labels(const Config *cfg) {
    MailClient *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect.\n");
        return -1;
    }

    char **names = NULL, **ids = NULL;
    int count = 0;
    int rc = mail_client_list_with_ids(mc, &names, &ids, &count);
    mail_client_free(mc);

    if (rc != 0 || count == 0) {
        if (rc == 0) printf("No labels/folders found.\n");
        /* free any partial results */
        for (int i = 0; i < count; i++) {
            if (names) free(names[i]);
            if (ids)   free(ids[i]);
        }
        free(names);
        free(ids);
        return rc;
    }

    if (cfg->gmail_mode) {
        printf("%-30s  %s\n", "Label", "ID");
        printf("%-30s  %s\n", "------------------------------",
               "------------------------------");
        for (int i = 0; i < count; i++) {
            printf("%-30s  %s\n",
                   names[i] ? names[i] : "",
                   ids[i]   ? ids[i]   : "");
        }
    } else {
        for (int i = 0; i < count; i++)
            printf("%s\n", names[i] ? names[i] : "");
    }

    for (int i = 0; i < count; i++) {
        free(names[i]);
        free(ids[i]);
    }
    free(names);
    free(ids);
    return 0;
}

int email_service_create_label(const Config *cfg, const char *name) {
    MailClient *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect.\n");
        return -1;
    }
    char *new_id = NULL;
    int rc = mail_client_create_label(mc, name, &new_id);
    mail_client_free(mc);

    if (rc == 0)
        printf("Label '%s' created (ID: %s).\n", name, new_id ? new_id : name);
    free(new_id);
    return rc;
}

int email_service_delete_label(const Config *cfg, const char *label_id) {
    MailClient *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect.\n");
        return -1;
    }
    int rc = mail_client_delete_label(mc, label_id);
    mail_client_free(mc);

    if (rc == 0)
        printf("Label '%s' deleted.\n", label_id);
    return rc;
}

int email_service_save_sent(const Config *cfg, const char *msg, size_t msg_len) {
    RAII_MAIL MailClient *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Warning: could not connect to save message to Sent folder.\n");
        return -1;
    }
    const char *sent_folder = cfg->sent_folder ? cfg->sent_folder : "Sent";
    int rc = mail_client_append(mc, sent_folder, msg, msg_len);
    if (rc != 0)
        fprintf(stderr, "Warning: could not save message to '%s' folder.\n", sent_folder);
    return rc;
}
