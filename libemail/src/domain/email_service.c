#include "email_service.h"
#include "config_store.h"
#include "input_line.h"
#include "path_complete.h"
#include "imap_client.h"
#include "mail_client.h"
#include "gmail_sync.h"
#include "local_store.h"
#include "mail_rules.h"
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
#include <ctype.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

/* ── Verbose mode ────────────────────────────────────────────────────── */

static int g_verbose = 0;

/** @brief Set verbose mode for sync and apply-rules operations. */
void email_service_set_verbose(int v) { g_verbose = v; }

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
                /* No space found: never hard-break a word — emit the whole
                 * token and let the terminal handle visual wrapping. */
                const char *word_end = (const char *)p;
                while (word_end < line_end && !isspace((unsigned char)*word_end))
                    word_end++;
                size_t n = (size_t)(word_end - seg);
                if (n == 0) {
                    /* Single wide char exceeds width: emit it anyway. */
                    const unsigned char *u = (const unsigned char *)seg;
                    int sl = (*u < 0x80) ? 1
                           : (*u < 0xE0) ? 2
                           : (*u < 0xF0) ? 3 : 4;
                    memcpy(wp, seg, (size_t)sl); wp += sl; seg += sl;
                } else {
                    memcpy(wp, seg, n); wp += n;
                    seg = word_end;
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
static void show_label_picker(MailClient *mc, const char *uid,
                               char *feedback_out, int feedback_cap);
static int is_system_or_special_label(const char *name);
static void flag_push_background(const Config *cfg, const char *uid,
                                  const char *flag_name, int add_flag);

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
                                const char *date, const char *uid,
                                const char *labels) {
    /* label = 9 chars ("From:    "), remaining = SHOW_WIDTH - 9 = 71 */
    printf("From:    "); print_clean(from,    "(none)", SHOW_WIDTH - 9); putchar('\n');
    printf("Subject: "); print_clean(subject, "(none)", SHOW_WIDTH - 9); putchar('\n');
    printf("Date:    "); print_clean(date,    "(none)", SHOW_WIDTH - 9); putchar('\n');
    printf("UID:     %s\n", uid ? uid : "(none)");
    if (labels && labels[0])
        printf("Labels:  %s\n", labels);
    printf(SHOW_SEPARATOR);
}

/* Find the line number (0-based) in body containing term.
 * Searches forward (dir >= 0) or backward (dir < 0) from from_line.
 * Wraps around. Returns -1 if no match. */
static int find_match_line(const char *body, const char *term, int from_line, int dir) {
    if (!term || !*term || !body) return -1;
    int matches[8192]; int nm = 0;
    const char *p = body; int ln = 0;
    while (*p && nm < 8192) {
        const char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        char tmp[512];
        size_t cp = llen < sizeof(tmp) - 1 ? llen : sizeof(tmp) - 1;
        memcpy(tmp, p, cp); tmp[cp] = '\0';
        if (strcasestr(tmp, term)) matches[nm++] = ln;
        p = nl ? nl + 1 : p + strlen(p);
        ln++;
    }
    if (nm == 0) return -1;
    if (dir >= 0) {
        for (int i = 0; i < nm; i++) if (matches[i] > from_line) return matches[i];
        return matches[0]; /* wrap */
    } else {
        for (int i = nm - 1; i >= 0; i--) if (matches[i] < from_line) return matches[i];
        return matches[nm - 1]; /* wrap */
    }
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
 * Returns 0 = back to list (Backspace/ESC/q), 2 = reply, -1 = error.
 * mc may be NULL (operations then queue for background sync).
 * initial_flags: caller-supplied MSG_FLAG_* bitmask (used for IMAP where .hdr
 *   does not carry a flags field).
 * flags_out: if non-NULL, receives the final flag state on exit.
 */
static int show_uid_interactive(const Config *cfg, MailClient *mc,
                                const char *folder,
                                const char *uid, int page_size,
                                int initial_flags, int *flags_out) {
    char *raw = NULL;
    if (local_msg_exists(folder, uid)) {
        raw = local_msg_load(folder, uid);
    } else if (mc) {
        if (mail_client_select(mc, folder) == 0)
            raw = mail_client_fetch_body(mc, uid);
        if (raw) {
            local_msg_save(folder, uid, raw, strlen(raw));
            local_index_update(folder, uid, raw);
        }
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
    /* Load current flags for 'f' / 'n' / 'd' toggle operations.
     * For Gmail: .hdr contains a flags field — use it if available.
     * For IMAP:  .hdr contains raw RFC 2822 headers without flags — use
     *            the caller-supplied initial_flags instead. */
    int reader_flags = initial_flags;
    if (cfg->gmail_mode) {
        char *hdr = local_hdr_load("", uid);
        if (hdr) {
            char *last_tab = strrchr(hdr, '\t');
            if (last_tab) reader_flags = atoi(last_tab + 1);
            free(hdr);
        }
    }
    int term_cols = terminal_cols();
    int term_rows = terminal_rows();
    if (term_cols <= 0) term_cols = 80;
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
    int hdr_rows    = (show_labels && show_labels[0]) ? 6 : 5;
    int rows_avail  = (term_rows > hdr_rows + 2) ? term_rows - hdr_rows - 2 : 1;
    int view_raw    = 0; /* 0=rendered, 1=raw source */
    int body_vrows  = count_visual_rows(body_text, term_cols);
    int total_pages = (body_vrows + rows_avail - 1) / rows_avail;
    if (total_pages < 1) total_pages = 1;

    char search_buf[256] = ""; /* last search term; empty = none */

    /* Persistent info message — stays until replaced by a newer one */
    char info_msg[2048] = "";

    int result = 0;
    for (int cur_line = 0;;) {
        /* Recompute active body text and pagination for current view mode */
        body_text  = view_raw ? raw : (body ? body : "(no readable text body)");
        body_vrows = count_visual_rows(body_text, term_cols);
        total_pages = (body_vrows + rows_avail - 1) / rows_avail;
        if (total_pages < 1) total_pages = 1;

        printf("\033[0m\033[H\033[2J");     /* reset attrs + clear screen */
        print_show_headers(from, subject, date, uid, show_labels);
        print_body_page(body_text, cur_line, rows_avail, term_cols);
        printf("\033[0m");                  /* close any open ANSI from body */
        fflush(stdout);

        int cur_page = cur_line / rows_avail + 1;

        /* Info line (second from bottom) — persistent until overwritten */
        print_infoline(term_rows, wrap_cols, info_msg);

        /* Shortcut hints (bottom row) */
        {
            char sb[256];
            int is_gmail = cfg->gmail_mode;
            const char *vtog = view_raw ? "v=rendered" : "v=source";
            if (is_gmail) {
                if (att_count > 0) {
                    snprintf(sb, sizeof(sb),
                             "-- [%d/%d] \u2191\u2193=scroll  r=rm-label  d=rm  D=trash"
                             "  f=star  n=unread  a=arch  t=labels  A=save(%d)"
                             "  /=search  %s  q=back  ESC=quit --",
                             cur_page, total_pages, att_count, vtog);
                } else {
                    snprintf(sb, sizeof(sb),
                             "-- [%d/%d] \u2191\u2193=scroll  r=rm-label  d=rm  D=trash"
                             "  f=star  n=unread  a=arch  t=labels"
                             "  /=search  %s  q=back  ESC=quit --",
                             cur_page, total_pages, vtog);
                }
            } else if (att_count > 0) {
                snprintf(sb, sizeof(sb),
                         "-- [%d/%d] \u2191\u2193=scroll  r=reply  f=star  n=unread"
                         "  d=done  a=save  A=save-all(%d)"
                         "  /=search  %s  BS=list  ESC=quit --",
                         cur_page, total_pages, att_count, vtog);
            } else {
                snprintf(sb, sizeof(sb),
                         "-- [%d/%d] \u2191\u2193=scroll  r=reply  f=star  n=unread"
                         "  d=done  /=search  %s  BS=list  ESC=quit --",
                         cur_page, total_pages, vtog);
            }
            print_statusbar(term_rows, term_cols, sb);
        }

        TermKey key = terminal_read_key();
        fprintf(stderr, "\r\033[K");
        fflush(stderr);

        switch (key) {
        case TERM_KEY_BACK:
        case TERM_KEY_QUIT:
            result = 0;          /* back to list */
            goto show_int_done;
        case TERM_KEY_ESC:
            result = 1;          /* exit program */
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
            int is_gmail = cfg->gmail_mode;
            if (ch == 'q') {
                result = 0;      /* back to list */
                goto show_int_done;
            } else if (ch == 'v') {
                view_raw = !view_raw;
                cur_line = 0;
                break;
            } else if (ch == '/') {
                /* Inline search prompt on the status row */
                printf("\033[%d;1H\033[2K/", term_rows);
                fflush(stdout);
                size_t slen = 0; search_buf[0] = '\0';
                int srch_cancel = 0;
                for (;;) {
                    TermKey sk = terminal_read_key();
                    if (sk == TERM_KEY_ESC) { srch_cancel = 1; break; }
                    if (sk == TERM_KEY_ENTER) break;
                    if (sk == TERM_KEY_BACK) {
                        if (slen > 0) search_buf[--slen] = '\0';
                    } else if (sk == TERM_KEY_IGNORE) {
                        int sc = terminal_last_printable();
                        if (sc == 127 || sc == 8) {
                            if (slen > 0) search_buf[--slen] = '\0';
                        } else if (sc >= 32 && sc < 127 && slen + 1 < sizeof(search_buf)) {
                            search_buf[slen++] = (char)sc; search_buf[slen] = '\0';
                        }
                    }
                    printf("\033[%d;1H\033[2K/%s_", term_rows, search_buf);
                    fflush(stdout);
                }
                if (!srch_cancel && search_buf[0]) {
                    int ml = find_match_line(body_text, search_buf, cur_line - 1, 1);
                    if (ml >= 0) { cur_line = ml; info_msg[0] = '\0'; }
                    else snprintf(info_msg, sizeof(info_msg), "No match: %s", search_buf);
                } else if (srch_cancel) {
                    search_buf[0] = '\0';
                }
                break;
            } else if (ch == 'n' && search_buf[0]) {
                int ml = find_match_line(body_text, search_buf, cur_line, 1);
                if (ml >= 0) { cur_line = ml; info_msg[0] = '\0'; }
                else snprintf(info_msg, sizeof(info_msg), "No more matches");
                break;
            } else if (ch == 'N' && search_buf[0]) {
                int ml = find_match_line(body_text, search_buf, cur_line, -1);
                if (ml >= 0) { cur_line = ml; info_msg[0] = '\0'; }
                else snprintf(info_msg, sizeof(info_msg), "No more matches");
                break;
            } else if (ch == 'h' || ch == '?') {
                if (is_gmail) {
                    static const char *ghelp[][2] = {
                        { "PgDn / \u2193",   "Scroll down one page / one line"          },
                        { "PgUp / \u2191",   "Scroll up one page / one line"            },
                        { "Home / End",      "Jump to top / bottom of message"          },
                        { "r",              "Remove current label"                      },
                        { "d",              "Remove current label"                      },
                        { "D",              "Move to Trash"                             },
                        { "f",              "Toggle Starred label"                      },
                        { "n",              "Toggle Unread label"                       },
                        { "a",              "Archive (remove all labels)"               },
                        { "t",              "Toggle labels (picker)"                    },
                        { "A",              "Save attachment"                           },
                        { "v",              "Toggle rendered / raw source view"         },
                        { "/",              "Search in message body"                    },
                        { "n / N",          "Next / previous search match"             },
                        { "q / Backspace",  "Back to message list"                     },
                        { "ESC",            "Exit program"                              },
                        { "h / ?",          "Show this help"                            },
                    };
                    show_help_popup("Message reader shortcuts (Gmail)",
                                    ghelp, (int)(sizeof(ghelp)/sizeof(ghelp[0])));
                } else {
                    static const char *help[][2] = {
                        { "PgDn / \u2193",   "Scroll down one page / one line"          },
                        { "PgUp / \u2191",   "Scroll up one page / one line"            },
                        { "Home / End",      "Jump to top / bottom of message"          },
                        { "r",              "Reply to this message"                     },
                        { "f",              "Toggle Flagged (starred)"                  },
                        { "n",              "Toggle Unread flag"                        },
                        { "d",              "Toggle Done flag"                          },
                        { "a",              "Save an attachment"                        },
                        { "A",              "Save all attachments"                      },
                        { "v",              "Toggle rendered / raw source view"         },
                        { "/",              "Search in message body"                    },
                        { "n / N",          "Next / previous search match"             },
                        { "Backspace / q",  "Back to message list"                     },
                        { "ESC",            "Exit program"                              },
                        { "h / ?",          "Show this help"                            },
                    };
                    show_help_popup("Message reader shortcuts",
                                    help, (int)(sizeof(help)/sizeof(help[0])));
                }
                break;
            } else if (is_gmail && (ch == 'r' || ch == 'd') && folder[0] != '_') {
                /* Remove current label from this message */
                const char *lbl = folder;
                label_idx_remove(lbl, uid);
                local_hdr_update_labels("", uid, NULL, 0, &lbl, 1);
                if (mc) mail_client_modify_label(mc, uid, lbl, 0);
                snprintf(info_msg, sizeof(info_msg), "Label removed: %s", lbl);
                /* Reload show_labels to reflect the change */
                free(show_labels);
                show_labels = local_hdr_get_labels("", uid);
                break;
            } else if (is_gmail && ch == 'D') {
                /* Trash: Gmail compound trash operation */
                if (mc) mail_client_trash(mc, uid);
                char **all_labels = NULL; int all_count = 0;
                label_idx_list(&all_labels, &all_count);
                for (int j = 0; j < all_count; j++) {
                    label_idx_remove(all_labels[j], uid);
                    free(all_labels[j]);
                }
                free(all_labels);
                label_idx_add("_trash", uid);
                snprintf(info_msg, sizeof(info_msg), "Moved to Trash");
                break;
            } else if (is_gmail && ch == 'a') {
                /* Archive: remove all labels from this message */
                if (strcmp(folder, "_nolabel") == 0) {
                    snprintf(info_msg, sizeof(info_msg),
                             "Already in Archive \xe2\x80\x94 no change");
                    break;
                }
                char *lbl_str = local_hdr_get_labels("", uid);
                if (lbl_str) {
                    int n = 1;
                    for (const char *p = lbl_str; *p; p++) if (*p == ',') n++;
                    char **rm  = malloc((size_t)n * sizeof(char *));
                    char *copy = strdup(lbl_str);
                    int   rm_n = 0;
                    if (rm && copy) {
                        char *tok = copy, *sep;
                        while (tok && *tok) {
                            sep = strchr(tok, ',');
                            if (sep) *sep = '\0';
                            if (tok[0] && tok[0] != '_') {
                                label_idx_remove(tok, uid);
                                rm[rm_n++] = tok;
                                if (mc &&
                                    strcmp(tok, "IMPORTANT") != 0 &&
                                    strncmp(tok, "CATEGORY_", 9) != 0)
                                    mail_client_modify_label(mc, uid, tok, 0);
                            }
                            tok = sep ? sep + 1 : NULL;
                        }
                        local_hdr_update_labels("", uid, NULL, 0,
                                                (const char **)rm, rm_n);
                    }
                    free(copy); free(rm); free(lbl_str);
                }
                label_idx_remove("UNREAD", uid);
                int new_flags = reader_flags & ~MSG_FLAG_UNSEEN;
                local_hdr_update_flags("", uid, new_flags);
                reader_flags = new_flags;
                if (mc) mail_client_set_flag(mc, uid, "\\Seen", 1);
                label_idx_add("_nolabel", uid);
                free(show_labels);
                show_labels = local_hdr_get_labels("", uid);
                snprintf(info_msg, sizeof(info_msg), "Archived");
                break;
            } else if (is_gmail && ch == 't') {
                show_label_picker(mc, uid, info_msg, sizeof(info_msg));
                free(show_labels);
                show_labels = local_hdr_get_labels("", uid);
                break;
            } else if (ch == 'f') {
                /* Toggle starred / flagged */
                int currently = reader_flags & MSG_FLAG_FLAGGED;
                int add_flag  = currently ? 0 : 1;
                reader_flags ^= MSG_FLAG_FLAGGED;
                if (is_gmail) local_hdr_update_flags("", uid, reader_flags);
                local_pending_flag_add(folder, uid, "\\Flagged", add_flag);
                if (is_gmail) {
                    const char *lbl = "STARRED";
                    if (currently) {
                        label_idx_remove(lbl, uid);
                        local_hdr_update_labels("", uid, NULL, 0, &lbl, 1);
                    } else {
                        label_idx_add(lbl, uid);
                        local_hdr_update_labels("", uid, &lbl, 1, NULL, 0);
                    }
                    flag_push_background(cfg, uid, "\\Flagged", add_flag);
                    free(show_labels);
                    show_labels = local_hdr_get_labels("", uid);
                } else if (mc) {
                    mail_client_set_flag(mc, uid, "\\Flagged", add_flag);
                }
                snprintf(info_msg, sizeof(info_msg),
                         currently ? "Unstarred" : "Starred");
                break;
            } else if (ch == 'n') {
                /* Toggle unread / read */
                int currently = reader_flags & MSG_FLAG_UNSEEN;
                int add_flag  = currently ? 1 : 0; /* add \\Seen if currently unseen */
                reader_flags ^= MSG_FLAG_UNSEEN;
                if (is_gmail) local_hdr_update_flags("", uid, reader_flags);
                local_pending_flag_add(folder, uid, "\\Seen", add_flag);
                if (is_gmail) {
                    const char *lbl = "UNREAD";
                    if (currently) {
                        label_idx_remove(lbl, uid);
                        local_hdr_update_labels("", uid, NULL, 0, &lbl, 1);
                    } else {
                        label_idx_add(lbl, uid);
                        local_hdr_update_labels("", uid, &lbl, 1, NULL, 0);
                    }
                    flag_push_background(cfg, uid, "\\Seen", add_flag);
                    free(show_labels);
                    show_labels = local_hdr_get_labels("", uid);
                } else if (mc) {
                    mail_client_set_flag(mc, uid, "\\Seen", add_flag);
                }
                snprintf(info_msg, sizeof(info_msg),
                         currently ? "Marked as read" : "Marked as unread");
                break;
            } else if (!is_gmail && ch == 'd') {
                /* IMAP only: toggle Done flag */
                int currently = reader_flags & MSG_FLAG_DONE;
                int add_flag  = currently ? 0 : 1;
                reader_flags ^= MSG_FLAG_DONE;
                /* .hdr flags field is not used for IMAP; skip local_hdr_update_flags */
                local_pending_flag_add(folder, uid, "$Done", add_flag);
                if (mc) mail_client_set_flag(mc, uid, "$Done", add_flag);
                snprintf(info_msg, sizeof(info_msg),
                         currently ? "Marked not done" : "Marked done");
                break;
            } else if (!is_gmail && ch == 'r') {
                /* Ensure the raw message is in the local cache so cmd_reply
                 * can reload it without requiring a live IMAP connection. */
                if (!local_msg_exists(folder, uid))
                    local_msg_save(folder, uid, raw, strlen(raw));
                result = 2;      /* reply to this message */
                goto show_int_done;
            } else if (att_count > 0 && ((is_gmail && ch == 'A') || (!is_gmail && ch == 'a'))) {
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
    if (flags_out) *flags_out = reader_flags;
    return result;
}

/* ── List helpers ────────────────────────────────────────────────────── */

typedef struct { char uid[17]; int flags; time_t epoch; char folder[256]; } MsgEntry;

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

static void junk_push_background(const Config *cfg, const char *uid, int mark_junk) {
    struct sigaction sa = {0};
    sa.sa_handler = bg_sync_sigchld;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        MailClient *mc = make_mail(cfg);
        if (mc) {
            if (mark_junk) mail_client_mark_junk(mc, uid);
            else           mail_client_mark_notjunk(mc, uid);
            mail_client_free(mc);
        }
        _exit(0);
    }
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

/* Rebuild filtered entry index.
 * fentries[0..fcount-1] holds original indices from entries[] that match fbuf.
 * Empty fbuf = identity (all entries match). */
static void list_filter_rebuild(
    const MsgEntry *entries, int show_count,
    Manifest *manifest, const Config *cfg,
    const char *folder,
    const char *fbuf, int fscope,
    int *fentries, int *fcount_out)
{
    if (!fbuf || fbuf[0] == '\0') {
        for (int i = 0; i < show_count; i++) fentries[i] = i;
        *fcount_out = show_count;
        return;
    }
    int fc = 0;
    for (int i = 0; i < show_count; i++) {
        ManifestEntry *me = manifest_find(manifest, entries[i].uid);
        int match = 0;
        if (fscope == 0) {
            const char *s = (me && me->subject) ? me->subject : "";
            match = strcasestr(s, fbuf) != NULL;
        } else if (fscope == 1) {
            const char *s = (me && me->from) ? me->from : "";
            match = strcasestr(s, fbuf) != NULL;
        } else if (fscope == 2) {
            char *hdrs = fetch_uid_headers_cached(cfg, folder, entries[i].uid);
            if (hdrs) {
                char *to_raw = mime_get_header(hdrs, "To");
                if (to_raw) {
                    char *to_dec = mime_decode_words(to_raw);
                    if (to_dec) { match = strcasestr(to_dec, fbuf) != NULL; free(to_dec); }
                    free(to_raw);
                }
                free(hdrs);
            }
        } else {
            const char *lf = cfg->gmail_mode ? "" : folder;
            char *body = local_msg_load(lf, entries[i].uid);
            if (body) { match = strcasestr(body, fbuf) != NULL; free(body); }
        }
        if (match) fentries[fc++] = i;
    }
    *fcount_out = fc;
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
     * For virtual (underscore) labels: use a human-readable name.
     * For IMAP or system labels: use the ID as-is. */
    RAII_STRING char *folder_display_alloc = NULL;
    if (cfg->gmail_mode && folder)
        folder_display_alloc = local_gmail_label_name_lookup(folder);
    const char *folder_display = folder_display_alloc ? folder_display_alloc : folder;
    /* Virtual label display names (not in gmail_label_names) */
    if (cfg->gmail_mode && folder && !folder_display_alloc) {
        if (strcmp(folder, "_nolabel") == 0) folder_display = "Archive";
        else if (strcmp(folder, "_trash")   == 0) folder_display = "Trash";
        else if (strcmp(folder, "_spam")    == 0) folder_display = "Spam";
    }

    int list_result = 0;

    /* Virtual cross-folder views for IMAP (aggregate all manifests by flag) */
    int is_virtual_flags = 0;
    int virtual_flag_mask = 0;
    if (!cfg->gmail_mode && folder) {
        if (strcmp(folder, "__unread__")    == 0) { is_virtual_flags = 1; virtual_flag_mask = MSG_FLAG_UNSEEN;    folder_display = "Unread";    }
        if (strcmp(folder, "__flagged__")   == 0) { is_virtual_flags = 1; virtual_flag_mask = MSG_FLAG_FLAGGED;   folder_display = "Flagged";   }
        if (strcmp(folder, "__junk__")      == 0) { is_virtual_flags = 1; virtual_flag_mask = MSG_FLAG_JUNK;      folder_display = "Junk";      }
        if (strcmp(folder, "__phishing__")  == 0) { is_virtual_flags = 1; virtual_flag_mask = MSG_FLAG_PHISHING;  folder_display = "Phishing";  }
        if (strcmp(folder, "__answered__")  == 0) { is_virtual_flags = 1; virtual_flag_mask = MSG_FLAG_ANSWERED;  folder_display = "Answered";  }
        if (strcmp(folder, "__forwarded__") == 0) { is_virtual_flags = 1; virtual_flag_mask = MSG_FLAG_FORWARDED; folder_display = "Forwarded"; }
    }

    /* Cross-folder content search: folder = "__search__:<scope>:<query>" */
    int   is_virtual_search = 0;
    int   search_scope      = 0;
    char  search_query[256] = "";
    char  search_display[320] = "";
    if (folder && strncmp(folder, "__search__:", 11) == 0) {
        is_virtual_search = 1;
        search_scope = (folder[11] >= '0' && folder[11] <= '3') ? (folder[11] - '0') : 0;
        snprintf(search_query, sizeof(search_query), "%s",
                 (strlen(folder) > 13) ? folder + 13 : "");
        static const char *snames[] = {"Subject","From","To","Body"};
        snprintf(search_display, sizeof(search_display),
                 "Search: \"%s\" [%s]", search_query, snames[search_scope]);
        folder_display = search_display;
    }

    logger_log(LOG_INFO, "Listing %s @ %s/%s", cfg->user, cfg->host, folder);

    /* Load manifest (or build synthetic manifest for virtual views) */
    Manifest *manifest = NULL;
    if (is_virtual_flags) {
        manifest = manifest_load_all_with_flag(virtual_flag_mask);
        if (!manifest) return -1;
    } else if (is_virtual_search) {
        manifest = calloc(1, sizeof(Manifest));
        if (!manifest) return -1;
    } else {
        manifest = manifest_load(folder);
        if (!manifest) {
            manifest = calloc(1, sizeof(Manifest));
            if (!manifest) return -1;
        }
    }

    int show_count = 0;
    int unseen_count = 0;
    MsgEntry *entries = NULL;

    /* Shared mail client — populated in online mode, NULL in cron mode.
     * Kept alive for the full rendering loop so header fetches reuse it. */
    RAII_MAIL MailClient *list_mc = NULL;

    if (is_virtual_flags) {
        /* ── Virtual Unread/Flagged: local manifest aggregate (always cache-only).
         * Each entry carries its source folder so Enter, 'n', etc. can route
         * back to the correct per-folder manifest and IMAP SELECT. */
        SearchResult *fr = NULL;
        int fr_count = 0;
        local_flag_search(virtual_flag_mask, &fr, &fr_count);
        show_count = fr_count;
        entries = calloc((size_t)(fr_count > 0 ? fr_count : 1), sizeof(MsgEntry));
        if (!entries) { if (fr) free(fr); manifest_free(manifest); return -1; }
        for (int i = 0; i < fr_count; i++) {
            memcpy(entries[i].uid, fr[i].uid, 17);
            snprintf(entries[i].folder, sizeof(entries[i].folder), "%s", fr[i].folder);
            entries[i].flags = fr[i].flags;
            entries[i].epoch = fr[i].date ? parse_manifest_date(fr[i].date) : 0;
            manifest_upsert(manifest, fr[i].uid,
                            fr[i].from, fr[i].subject, fr[i].date, fr[i].flags);
            fr[i].from = fr[i].subject = fr[i].date = NULL;
            if (entries[i].flags & MSG_FLAG_UNSEEN) unseen_count++;
        }
        free(fr);
    } else if (is_virtual_search) {
        /* ── Cross-folder content search (always local data) ────────────── */
        SearchResult *sr = NULL;
        int sr_count = 0;
        local_search(search_query, search_scope, &sr, &sr_count);
        show_count = sr_count;
        entries = calloc((size_t)(sr_count > 0 ? sr_count : 1), sizeof(MsgEntry));
        if (!entries) { local_search_free(sr, sr_count); manifest_free(manifest); return -1; }
        for (int i = 0; i < sr_count; i++) {
            memcpy(entries[i].uid, sr[i].uid, 17);
            snprintf(entries[i].folder, sizeof(entries[i].folder), "%s", sr[i].folder);
            entries[i].flags = sr[i].flags;
            entries[i].epoch = sr[i].date ? parse_manifest_date(sr[i].date) : 0;
            /* Transfer ownership of strings to manifest; null them in sr so free() is safe */
            manifest_upsert(manifest, sr[i].uid,
                            sr[i].from, sr[i].subject, sr[i].date, sr[i].flags);
            sr[i].from = sr[i].subject = sr[i].date = NULL;
            if (entries[i].flags & MSG_FLAG_UNSEEN) unseen_count++;
        }
        local_search_free(sr, sr_count);
    } else if (cfg->sync_interval > 0) {
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
                int avail = tcols - 29; if (avail < 40) avail = 40;
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
                printf("  %-16s  %-5s  %-*s  %s\n",
                       "Date", "Sts", subj_w, "Subject", "From");
                printf("  ");
                print_dbar(16); printf("  \u2550\u2550\u2550\u2550\u2550  ");
                print_dbar(subj_w); printf("  "); print_dbar(from_w); printf("\n");
                printf("\n  \033[2m(empty)\033[0m\n");
                fflush(stdout);
                char sb[256];
                snprintf(sb, sizeof(sb),
                         "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                         "  Backspace=%s  ESC=quit"
                         "  s=sync  U=refresh  l=rules  [0/0]",
                         cfg->gmail_mode ? "labels" : "folders");
                print_statusbar(trows, tcols, sb);
            }
            for (;;) {
                TermKey key = terminal_read_key();
                if (key == TERM_KEY_BACK) return 1;
                if (key == TERM_KEY_QUIT || key == TERM_KEY_ESC) return 0;
                int ch = terminal_last_printable();
                if (ch == 's') { sync_start_background(); }
                if (ch == 'U') return 4; /* refresh: re-list */
                if (ch == 'l') return 7; /* rules editor */
            }
        }
        show_count = manifest->count;
        entries = malloc((size_t)show_count * sizeof(MsgEntry));
        if (!entries) { manifest_free(manifest); return -1; }
        for (int i = 0; i < show_count; i++) {
            memcpy(entries[i].uid, manifest->entries[i].uid, 17);
            entries[i].folder[0] = '\0';
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
            e->folder[0] = '\0';
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
                int avail = tcols - 29; if (avail < 40) avail = 40;
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
                printf("  %-16s  %-5s  %-*s  %s\n",
                       "Date", "Sts", subj_w, "Subject", "From");
                printf("  ");
                print_dbar(16); printf("  \u2550\u2550\u2550\u2550\u2550  ");
                print_dbar(subj_w); printf("  "); print_dbar(from_w); printf("\n");
                printf("\n  \033[2m(empty)\033[0m\n");
                fflush(stdout);
                char sb[256];
                if (cfg->gmail_mode) {
                    snprintf(sb, sizeof(sb),
                             "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                             "  Backspace=labels  ESC=quit"
                             "  c=compose  r=reply  F=fwd  A=r-all  n=unread  f=star"
                             "  s=sync  U=refresh  l=rules  [0/0]");
                } else {
                    snprintf(sb, sizeof(sb),
                             "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                             "  Backspace=folders  ESC=quit"
                             "  c=compose  r=reply  F=fwd  A=r-all  n=new  f=flag  d=done"
                             "  s=sync  U=refresh  l=rules  [0/0]");
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
                if (ch == 'U') return 4; /* refresh */
                if (ch == 'l') return 7; /* rules editor */
            }
        }

        /* Build tagged entry array */
        entries = malloc((size_t)show_count * sizeof(MsgEntry));
        if (!entries) { free(unseen_uids); free(flagged_uids); free(done_uids); free(all_uids); manifest_free(manifest); return -1; }

        for (int i = 0; i < show_count; i++) {
            memcpy(entries[i].uid, all_uids[i], 17);
            entries[i].folder[0] = '\0';
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
            int avail = tcols - 29; if (avail < 40) avail = 40;
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
            printf("  %-16s  %-5s  %-*s  %s\n",
                   "Date", "Sts", subj_w, "Subject", "From");
            printf("  ");
            print_dbar(16); printf("  \u2550\u2550\u2550\u2550\u2550  ");
            print_dbar(subj_w); printf("  "); print_dbar(from_w); printf("\n");
            printf("\n  \033[2m(empty)\033[0m\n");
            fflush(stdout);
            char sb[256];
            if (cfg->gmail_mode) {
                int in_trash_empty = (strcmp(folder, "_trash") == 0);
                if (in_trash_empty) {
                    snprintf(sb, sizeof(sb),
                             "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                             "  Backspace=labels  ESC=quit"
                             "  u=restore  t=labels  n=unread  f=star"
                             "  s=sync  U=refresh  l=rules  [0/0]");
                } else {
                    snprintf(sb, sizeof(sb),
                             "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                             "  Backspace=labels  ESC=quit"
                             "  c=compose  r=reply  F=fwd  A=r-all  n=unread  f=star"
                             "  s=sync  U=refresh  l=rules  [0/0]");
                }
            } else {
                snprintf(sb, sizeof(sb),
                         "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                         "  Backspace=folders  ESC=quit"
                         "  c=compose  r=reply  F=fwd  A=r-all  n=new  f=flag  d=done"
                         "  s=sync  U=refresh  l=rules  [0/0]");
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
            if (ch == 'U') return 4; /* refresh */
            if (ch == 'l') return 7; /* rules editor */
        }
    }

    /* Sort: unseen → flagged → rest, within each group newest (highest UID) first */
    qsort(entries, (size_t)show_count, sizeof(MsgEntry), cmp_uid_entry);

    int limit  = (opts->limit > 0) ? opts->limit : show_count;
    int cursor = (opts->offset > 1) ? opts->offset - 1 : 0;
    if (cursor >= show_count) cursor = 0;
    int wstart = cursor;   /* top of the visible window */

    /* Track entries with pending operations for immediate visual feedback.
     * pending_remove[i] = 1: row will be gone on next refresh (red strikethrough).
     *   Set by: 'D' (trash) only.
     * pending_label[i] = 1: label removed/archived, row may stay (yellow strikethrough).
     *   Set by: 'd' (remove current label), 'a' (archive).
     * pending_restore[i] = 1: row will leave this view on next refresh (green strikethrough).
     *   Set by: 'u' (untrash/restore), 't' label-picker unarchive.
     * Only allocated in TUI (pager) mode; always NULL in CLI/RO mode. */
    int *pending_remove  = opts->pager
                           ? calloc((size_t)(show_count > 0 ? show_count : 1), sizeof(int))
                           : NULL;
    int *pending_label   = opts->pager
                           ? calloc((size_t)(show_count > 0 ? show_count : 1), sizeof(int))
                           : NULL;
    int *pending_restore = opts->pager
                           ? calloc((size_t)(show_count > 0 ? show_count : 1), sizeof(int))
                           : NULL;
    /* Feedback message shown on the second-to-last row after each operation.
     * Cleared only when the list is re-opened (R/ESC/Backspace restart the view). */
    char feedback_msg[256] = "";

    /* ── Live filter state ──────────────────────────────────────────── */
    int   filter_active = 0;   /* filter bar is visible */
    int   filter_input  = 0;   /* typing mode (vs navigation mode) */
    int   filter_scope  = 0;   /* 0=Subject  1=From  2=To  3=Body */
    int   filter_dirty    = 0;   /* rebuild needed after next render */
    int   filter_scanning = 0;   /* body scan in progress — show progress bar */
    char  filter_buf[256] = "";
    int  *fentries = opts->pager
                     ? calloc((size_t)(show_count > 0 ? show_count : 1), sizeof(int))
                     : NULL;
    int   fcount = show_count;
    if (fentries) { for (int _fi = 0; _fi < show_count; _fi++) fentries[_fi] = _fi; }

    /* Keep the terminal in raw mode for the entire interactive TUI.
     * Without this, each terminal_read_key() call would need to briefly enter
     * and exit raw mode per keystroke, which causes escape sequence echo and
     * ICANON buffering artefacts.  terminal_read_key() requires raw mode to
     * already be active — we enter it once here and exit at list_done. */
    RAII_TERM_RAW TermRawState *tui_raw = opts->pager
                                          ? terminal_raw_enter()
                                          : NULL;

    for (;;) {
        /* Number of rows visible under current filter (= show_count when no filter) */
        int disp_count = filter_active ? fcount : show_count;

        /* Effective row budget: filter bar occupies 2 rows (separator + input) */
        int eff_limit = (opts->pager && filter_active) ? limit - 2 : limit;
        if (eff_limit < 1) eff_limit = 1;

        /* Scroll window to keep cursor visible */
        if (cursor < wstart)                 wstart = cursor;
        if (cursor >= wstart + eff_limit)    wstart = cursor - eff_limit + 1;
        if (wstart < 0)                      wstart = 0;
        int wend = wstart + eff_limit;
        if (wend > disp_count)               wend = disp_count;

        /* Compute adaptive column widths.
         * email-tui (opts->pager==1): date+sts+subject+from, overhead=29
         * email-cli/ro (opts->pager==0): uid+date+sts+subject+from, overhead=47
         * Non-TTY CLI: two-pass — pre-load all entries, use max Subject width. */
        int is_tty   = isatty(STDOUT_FILENO);
        int tcols    = is_tty ? terminal_cols() : 0;
        int is_gmail = cfg->gmail_mode;
        int show_uid = !opts->pager;   /* UID column in CLI/RO mode, not TUI */
        int overhead = show_uid ? 47 : 29;  /* 47 = 29 + uid(16) + sep(2) */
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
            int sync = (bg_sync_pid > 0) || sync_is_running();
            const char *suffix;
            if (bg_sync_done)
                suffix = "  \u2709 New mail may have arrived!  U=refresh";
            else if (sync)
                suffix = "  \u21bb syncing...";
            else if (is_gmail && strcmp(folder, "_trash") == 0)
                suffix = "  \u26a0 auto-delete: 30 days";
            else
                suffix = "";
            if (filter_active) {
                static const char *scn[] = {"Subject","From","To","Body"};
                snprintf(cl, sizeof(cl),
                         "  Showing %d of %d  [filter:%s]  %s (%d unread) [%s].%s",
                         fcount, show_count, scn[filter_scope],
                         folder_display, unseen_count,
                         cfg->user ? cfg->user : "?", suffix);
            } else {
                snprintf(cl, sizeof(cl),
                         "  %d-%d of %d message(s) in %s (%d unread) [%s].%s",
                         wstart + 1, wend, show_count, folder_display, unseen_count,
                         cfg->user ? cfg->user : "?", suffix);
            }
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
            printf("  %-16s  %-16s  %-5s  %-*s  %s\n",
                   "UID", "Date", "Sts", subj_w, "Subject", "From");
        else
            printf("  %-16s  %-5s  %-*s  %s\n",
                   "Date", "Sts", subj_w, "Subject", "From");
        printf("  ");
        if (show_uid) { print_dbar(16); printf("  "); }
        print_dbar(16); printf("  ");
        printf("\u2550\u2550\u2550\u2550\u2550  ");
        print_dbar(subj_w > 0 ? subj_w : 30); printf("  ");
        print_dbar(from_w > 0 ? from_w : 40); printf("\n");

        /* Data rows: fetch-on-demand + immediate render per row */
        int load_interrupted = 0;
        for (int i = wstart; i < wend; i++) {
            /* Map display index to original entries[] index */
            int ei = (filter_active && fentries) ? fentries[i] : i;
            /* Fetch into manifest if missing; always sync unseen flag */
            ManifestEntry *cached_me = manifest_find(manifest, entries[ei].uid);
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
                const char *hf = entries[ei].folder[0] ? entries[ei].folder : folder;
                char *hdrs     = list_mc
                                 ? fetch_uid_headers_via(list_mc, hf, entries[ei].uid)
                                 : fetch_uid_headers_cached(cfg, hf, entries[ei].uid);
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
                    entries[ei].flags |= MSG_FLAG_ATTACH;
                free(ct_raw);
                free(hdrs);
                manifest_upsert(manifest, entries[ei].uid, fr, su, dt, entries[ei].flags);
                manifest_dirty = 1;
            } else if (cached_me->flags != entries[ei].flags) {
                /* Keep manifest flags in sync (relevant in online mode) */
                cached_me->flags = entries[ei].flags;
                manifest_dirty = 1;
            }

            /* Render this row immediately */
            ManifestEntry *me = manifest_find(manifest, entries[ei].uid);
            const char *from    = (me && me->from    && me->from[0])    ? me->from    : "(no from)";
            const char *subject = (me && me->subject && me->subject[0]) ? me->subject : "(no subject)";
            const char *date    = (me && me->date)                       ? me->date    : "";

            int sel             = opts->pager && (i == cursor);
            int remove_pending  = (pending_remove  != NULL) && pending_remove[ei];
            int label_pending   = (pending_label   != NULL) && pending_label[ei];
            int restore_pending = (pending_restore != NULL) && pending_restore[ei];

            /* Pending rows: visible marker prefix + colour (no strikethrough).
             * The marker character is visible in every terminal and survives
             * copy-paste; colour provides additional visual hint where supported.
             * Cursor on pending row: add inverse-video so cursor stays visible.
             *   D + red    = trash pending (destructive, first char 'D')
             *   d + yellow = label-remove / archive pending (neutral)
             *   ↩ + green  = restore / unarchive pending (restorative)
             *     (space)  = normal row */
            const char *row_pfx;  /* 2-byte row prefix (marker + space or 2 spaces) */
            if (opts->pager && remove_pending) {
                row_pfx = "D ";
                printf(sel ? "\033[7m\033[31m" : "\033[31m"); /* red (+ inverse if sel) */
            } else if (opts->pager && label_pending) {
                row_pfx = "d ";
                printf(sel ? "\033[7m\033[33m" : "\033[33m"); /* yellow */
            } else if (opts->pager && restore_pending) {
                row_pfx = "u ";
                printf(sel ? "\033[7m\033[32m" : "\033[32m"); /* green */
            } else {
                row_pfx = "  ";
                if (sel) printf("\033[7m");
            }

            /* Status column (5 chars): [P/J/N/-][star/-][D/-][A/-][R/F/-]
             * Position 1: P=phishing(red) > J=junk(yellow) > N=unread(green) > -
             * Position 2: star = flagged (yellow)
             * Position 3: D = done
             * Position 4: A = attachment
             * Position 5: R=answered(cyan), F=forwarded(cyan), - = neither
             *
             * TUI: ANSI colours; sel rows exit/re-enter reverse-video around colour. */
            char sts[96];
            int eflags = entries[ei].flags;
            if (opts->pager && !remove_pending && !label_pending && !restore_pending) {
                const char *n_s;
                if      (eflags & MSG_FLAG_PHISHING)
                    n_s = sel ? "\033[0m\033[1;31mP\033[7m" : "\033[1;31mP\033[0m";
                else if (eflags & MSG_FLAG_JUNK)
                    n_s = sel ? "\033[0m\033[33mJ\033[7m"   : "\033[33mJ\033[0m";
                else if (eflags & MSG_FLAG_UNSEEN)
                    n_s = sel ? "\033[0m\033[32mN\033[7m"   : "\033[32mN\033[0m";
                else
                    n_s = "-";
                const char *f_s = (eflags & MSG_FLAG_FLAGGED)
                    ? (sel ? "\033[0m\033[33m\xe2\x98\x85\033[7m"
                           : "\033[33m\xe2\x98\x85\033[0m")
                    : "-";
                const char *rf_s;
                if      (eflags & MSG_FLAG_ANSWERED)
                    rf_s = sel ? "\033[0m\033[36mR\033[7m" : "\033[36mR\033[0m";
                else if (eflags & MSG_FLAG_FORWARDED)
                    rf_s = sel ? "\033[0m\033[36mF\033[7m" : "\033[36mF\033[0m";
                else
                    rf_s = "-";
                snprintf(sts, sizeof(sts), "%s%s%c%c%s", n_s, f_s,
                    (eflags & MSG_FLAG_DONE)   ? 'D' : '-',
                    (eflags & MSG_FLAG_ATTACH) ? 'A' : '-',
                    rf_s);
            } else {
                sts[0] = (eflags & MSG_FLAG_PHISHING)  ? 'P'
                       : (eflags & MSG_FLAG_JUNK)       ? 'J'
                       : (eflags & MSG_FLAG_UNSEEN)     ? 'N' : '-';
                sts[1] = (eflags & MSG_FLAG_FLAGGED)   ? '*' : '-';
                sts[2] = (eflags & MSG_FLAG_DONE)      ? 'D' : '-';
                sts[3] = (eflags & MSG_FLAG_ATTACH)    ? 'A' : '-';
                sts[4] = (eflags & MSG_FLAG_ANSWERED)  ? 'R'
                       : (eflags & MSG_FLAG_FORWARDED) ? 'F' : '-';
                sts[5] = '\0';
            }
            if (show_uid)
                printf("%s%-16.16s  %-16.16s  %s  ", row_pfx, entries[ei].uid, date, sts);
            else
                printf("%s%-16.16s  %s  ", row_pfx, date, sts);
            print_padded_col(subject, subj_w);
            printf("  ");
            print_padded_col(from,    from_w);

            if (opts->pager && (sel || remove_pending || label_pending || restore_pending))
                printf("\033[K\033[0m");
            printf("\n");
            fflush(stdout); /* show row immediately as it arrives */
        }
        if (manifest_dirty && !is_virtual_search) manifest_save(folder, manifest);
        if (load_interrupted) goto list_done;

        if (!opts->pager) {
            if (wend < show_count)
                printf("\n  -- %d more message(s) --  use --offset %d for next page\n",
                       show_count - wend, wend + 1);
            break;
        }

        /* Filter bar — shown when filter is active (separator + input = 2 rows) */
        if (filter_active) {
            static const char *scope_names[] = {"Subject","From","To","Body"};
            printf("  \xe2\x94\x80"); /* ─ */
            for (int _p = 3; _p < tcols - 2; _p++) printf("\xe2\x94\x80");
            printf("\n  Filter [");
            for (int _s = 0; _s < 4; _s++) {
                if (_s > 0) printf("|");
                if (_s == filter_scope) printf("\033[7m%s\033[0m", scope_names[_s]);
                else printf("%s", scope_names[_s]);
            }
            if (filter_scanning)
                printf("]: %s  Scanning body...\033[K\n", filter_buf);
            else
                printf("]: %s%s  Showing %d of %d\033[K\n",
                       filter_buf, filter_input ? "_" : " ", fcount, show_count);
        }

        /* Navigation hint (status bar) — anchored at last terminal row */
        fflush(stdout);
        {
            int trows = terminal_rows();
            if (trows <= 0) trows = limit + 6;
            /* Feedback line — second from bottom, shows last operation result */
            print_infoline(trows, tcols, feedback_msg);
            char sb[256];
            if (is_gmail) {
                int in_trash = (strcmp(folder, "_trash") == 0);
                if (in_trash) {
                    snprintf(sb, sizeof(sb),
                             "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                             "  Backspace=labels  ESC=quit"
                             "  u=restore  t=labels  n=unread  f=star"
                             "  s=sync  U=refresh  l=rules  [%d/%d]",
                             show_count > 0 ? cursor + 1 : 0, show_count);
                } else {
                    snprintf(sb, sizeof(sb),
                             "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                             "  Backspace=labels  ESC=quit"
                             "  c=compose  r=reply  F=fwd  A=r-all  n=unread  f=star  a=archive"
                             "  d=rm-label  D=trash  t=labels  s=sync  U=refresh  l=rules  [%d/%d]",
                             show_count > 0 ? cursor + 1 : 0, show_count);
                }
            } else {
                snprintf(sb, sizeof(sb),
                         "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                         "  Backspace=folders  ESC=quit"
                         "  c=compose  r=reply  F=fwd  A=r-all  n=new  f=flag  d=done"
                         "  s=sync  U=refresh  l=rules  [%d/%d]",
                         cursor + 1, show_count);
            }
            print_statusbar(trows, tcols, sb);
        }

        /* After render: if rebuild is needed (new char, backspace, Tab), show
         * "Scanning body..." first for body scope, then do the actual rebuild. */
        if (filter_dirty) {
            filter_dirty = 0;
            if (filter_buf[0] && filter_scope == 3) {
                filter_scanning = 1;
                continue;  /* re-render showing "Scanning body..." */
            }
            if (filter_buf[0])
                list_filter_rebuild(entries, show_count, manifest, cfg, folder,
                                    filter_buf, filter_scope, fentries, &fcount);
            cursor = 0;
            continue;
        }
        if (filter_scanning) {
            filter_scanning = 0;
            list_filter_rebuild(entries, show_count, manifest, cfg, folder,
                                filter_buf, filter_scope, fentries, &fcount);
            cursor = 0;
            continue;
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
        /* Only loop if read() was actually interrupted by SIGCHLD (EINTR path
         * returns TERM_KEY_IGNORE with no printable char).  If SIGCHLD fired
         * between prev_sync_done and read(), the real key is not TERM_KEY_IGNORE
         * and must be processed — otherwise the keypress is silently consumed. */
        if (bg_sync_done && !prev_sync_done && key == TERM_KEY_IGNORE
                && !terminal_last_printable()) {
            goto read_key_again;
        }

        switch (key) {
        case TERM_KEY_BACK:
            if (filter_input) {
                /* Backspace: remove last UTF-8 character (skip continuation bytes) */
                size_t fl = strlen(filter_buf);
                while (fl > 0 && (filter_buf[fl - 1] & 0xC0) == 0x80) fl--;
                if (fl > 0) fl--;
                filter_buf[fl] = '\0';
                filter_dirty = 1;
                cursor = 0;
                break;
            }
            list_result = 1;
            goto list_done;
        case TERM_KEY_QUIT:
            goto list_done;
        case TERM_KEY_ESC:
            if (filter_active) {
                /* First ESC clears the filter; second ESC quits */
                filter_active   = 0;
                filter_input    = 0;
                filter_scanning = 0;
                filter_dirty    = 0;
                filter_buf[0]   = '\0';
                list_filter_rebuild(entries, show_count, manifest, cfg, folder,
                                    filter_buf, filter_scope, fentries, &fcount);
                cursor = 0;
                break;
            }
            goto list_done;
        case TERM_KEY_ENTER:
            if (filter_input) {
                /* Enter commits filter text; switch to navigation mode */
                filter_input = 0;
                break;
            }
            {
                int ei_cur = (filter_active && fentries) ? fentries[cursor] : cursor;
                const char *efolder = entries[ei_cur].folder[0] ? entries[ei_cur].folder : folder;
                int prev_flags = entries[ei_cur].flags;
                int new_flags  = prev_flags;
                int ret = show_uid_interactive(cfg, list_mc, efolder,
                                               entries[ei_cur].uid, opts->limit,
                                               prev_flags, &new_flags);
                /* Propagate any flag changes made inside the reader */
                if (new_flags != prev_flags) {
                    entries[ei_cur].flags = new_flags;
                    ManifestEntry *rme = manifest_find(manifest, entries[ei_cur].uid);
                    if (rme) rme->flags = new_flags;
                    if (is_virtual_flags) {
                        /* Virtual list: update the per-folder manifest on disk */
                        Manifest *fm = manifest_load(efolder);
                        if (fm) {
                            ManifestEntry *fme = manifest_find(fm, entries[ei_cur].uid);
                            if (fme) { fme->flags = new_flags; manifest_save(efolder, fm); }
                            manifest_free(fm);
                        }
                    } else if (!is_virtual_search) {
                        manifest_save(folder, manifest);
                    }
                }
                if (ret == 1) { goto list_done; } /* ESC=exit → list_result=0 → quit */
                if (ret == 2) {
                    /* 'r' pressed in reader → reply to this message */
                    memcpy(opts->action_uid, entries[ei_cur].uid, 17);
                    snprintf(opts->action_folder, sizeof(opts->action_folder), "%s", efolder);
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
            cursor = disp_count > 0 ? disp_count - 1 : 0;
            break;
        case TERM_KEY_LEFT:
        case TERM_KEY_RIGHT:
        case TERM_KEY_DELETE:
        case TERM_KEY_TAB:
        case TERM_KEY_SHIFT_TAB:
        case TERM_KEY_IGNORE: {
            int ch = terminal_last_printable();

            /* ── Filter input mode: Tab cycles scope, printable chars typed ── */
            if (filter_input && key == TERM_KEY_TAB) {
                filter_scope = (filter_scope + 1) % 4;
                if (filter_buf[0]) filter_dirty = 1;  /* rebuild after next render */
                break;
            }
            if (filter_input && terminal_last_utf8()[0]) {
                const char *u8 = terminal_last_utf8();
                size_t ulen = strlen(u8);
                size_t fl   = strlen(filter_buf);
                if (fl + ulen < sizeof(filter_buf)) {
                    memcpy(filter_buf + fl, u8, ulen + 1);
                }
                filter_dirty = 1;
                cursor = 0;
                break;
            }
            /* ── Filter activation (/) ──────────────────────────────────── */
            if (ch == '/' && !filter_input) {
                filter_active = 1;
                filter_input  = 1;
                filter_buf[0] = '\0';
                list_filter_rebuild(entries, show_count, manifest, cfg, folder,
                                    filter_buf, filter_scope, fentries, &fcount);
                cursor = 0;
                break;
            }

            /* ── Normal action keys ─────────────────────────────────────── */
            int ei_cur = (filter_active && fentries) ? fentries[cursor] : cursor;
            const char *efolder = entries[ei_cur].folder[0] ? entries[ei_cur].folder : folder;
            if (ch == 'c') {
                list_result = 2;
                goto list_done;
            }
            if (ch == 'r') {
                memcpy(opts->action_uid, entries[ei_cur].uid, 17);
                snprintf(opts->action_folder, sizeof(opts->action_folder), "%s", efolder);
                list_result = 3;
                goto list_done;
            }
            if (ch == 'F') {
                memcpy(opts->action_uid, entries[ei_cur].uid, 17);
                snprintf(opts->action_folder, sizeof(opts->action_folder), "%s", efolder);
                list_result = 5;
                goto list_done;
            }
            if (ch == 'A') {
                memcpy(opts->action_uid, entries[ei_cur].uid, 17);
                snprintf(opts->action_folder, sizeof(opts->action_folder), "%s", efolder);
                list_result = 6;
                goto list_done;
            }
            if (ch == 's') {
                sync_start_background();
                break; /* re-render: shows ⟳ syncing... indicator */
            }
            if (ch == 'U') {
                /* Explicit refresh after sync notification */
                bg_sync_done = 0;
                list_result = 4;
                goto list_done;
            }
            if (ch == 'l') {
                /* Rules editor */
                list_result = 7;
                goto list_done;
            }
            if (ch == 'h' || ch == '?') {
                if (is_gmail) {
                    static const char *ghelp[][2] = {
                        { "\u2191 / \u2193",   "Move cursor up / down"           },
                        { "PgUp / PgDn",        "Page up / down"                  },
                        { "Enter",             "Open selected message"           },
                        { "r",                 "Reply to selected message"       },
                        { "A",                 "Reply-all to selected message"   },
                        { "F",                 "Forward selected message"        },
                        { "c",                 "Compose new message"             },
                        { "n",                 "Toggle Unread label"             },
                        { "f",                 "Toggle Starred label"            },
                        { "a",                 "Archive (remove INBOX label)"    },
                        { "d",                 "Remove current label"            },
                        { "D",                 "Move to Trash"                   },
                        { "u",                 "Untrash (restore to INBOX)"      },
                        { "t",                 "Toggle labels (picker)"          },
                        { "s",                 "Start background sync"           },
                        { "U",                 "Refresh after sync"              },
                        { "l",                 "Open rules editor"               },
                        { "Backspace",         "Open label browser"              },
                        { "ESC / q",           "Quit"                            },
                        { "h / ?",             "Show this help"                  },
                        { "────────────",      "──────────────────────────────" },
                        { "Status col 1:",     "P=Phishing  J=Junk  N=Unread  -" },
                        { "Status col 2:",     "\u2605=Starred  -=normal"          },
                    };
                    show_help_popup("Message list shortcuts (Gmail)",
                                    ghelp, (int)(sizeof(ghelp)/sizeof(ghelp[0])));
                } else {
                    static const char *help[][2] = {
                        { "\u2191 / \u2193",   "Move cursor up / down"           },
                        { "PgUp / PgDn",        "Page up / down"                  },
                        { "Enter",             "Open selected message"           },
                        { "r",                 "Reply to selected message"       },
                        { "A",                 "Reply-all to selected message"   },
                        { "F",                 "Forward selected message"        },
                        { "c",                 "Compose new message"             },
                        { "n",                 "Toggle New (unread) flag"        },
                        { "f",                 "Toggle Flagged (starred) flag"   },
                        { "j",                 "Toggle Junk (spam) flag"         },
                        { "d",                 "Toggle Done flag"                },
                        { "s",                 "Start background sync"           },
                        { "U",                 "Refresh after sync"              },
                        { "l",                 "Open rules editor"               },
                        { "Backspace",         "Open folder browser"             },
                        { "ESC / q",           "Quit"                            },
                        { "h / ?",             "Show this help"                  },
                        { "────────────",      "──────────────────────────────" },
                        { "Status col 1:",     "P=Phishing  J=Junk  N=Unread  -" },
                        { "Status col 2:",     "\u2605=Starred  -=normal"          },
                    };
                    show_help_popup("Message list shortcuts",
                                    help, (int)(sizeof(help)/sizeof(help[0])));
                }
                break;
            }
            if (ch == 'a' && is_gmail) {
                /* Archive: remove ALL labels from this message.
                 * Gmail "archive" = no labels → message lives only in All Mail.
                 * If already in Archive view, the message is already archived — no-op. */
                if (strcmp(folder, "_nolabel") == 0) {
                    snprintf(feedback_msg, sizeof(feedback_msg),
                             "Already in Archive \xe2\x80\x94 no change");
                    break;
                }
                const char *uid = entries[ei_cur].uid;
                char *lbl_str = local_hdr_get_labels("", uid);
                if (lbl_str) {
                    /* Build remove array and strip each label from indexes */
                    int n = 1;
                    for (const char *p = lbl_str; *p; p++) if (*p == ',') n++;
                    char **rm   = malloc((size_t)n * sizeof(char *));
                    char *copy  = strdup(lbl_str);
                    int   rm_n  = 0;
                    if (rm && copy) {
                        char *tok = copy, *sep;
                        while (tok && *tok) {
                            sep = strchr(tok, ',');
                            if (sep) *sep = '\0';
                            if (tok[0]) {
                                /* Remove from local index (skip meta _* labels) */
                                if (tok[0] != '_') label_idx_remove(tok, uid);
                                rm[rm_n++] = tok;
                                /* Remove via Gmail API (skip IMPORTANT/CATEGORY_) */
                                if (list_mc &&
                                    strcmp(tok, "IMPORTANT") != 0 &&
                                    strncmp(tok, "CATEGORY_", 9) != 0)
                                    mail_client_modify_label(list_mc, uid, tok, 0);
                            }
                            tok = sep ? sep + 1 : NULL;
                        }
                        /* Clear labels field in .hdr atomically */
                        local_hdr_update_labels("", uid, NULL, 0,
                                                (const char **)rm, rm_n);
                    }
                    free(copy); free(rm); free(lbl_str);
                }
                /* Ensure UNREAD index is also cleared (belt-and-suspenders) */
                label_idx_remove("UNREAD", uid);
                /* Clear UNSEEN bit in .hdr flags field so the message is not
                 * displayed as unread when browsing Archive. */
                {
                    int new_flags = entries[ei_cur].flags & ~MSG_FLAG_UNSEEN;
                    local_hdr_update_flags("", uid, new_flags);
                    entries[ei_cur].flags = new_flags;
                }
                /* Mark as read via API */
                if (list_mc) mail_client_set_flag(list_mc, uid, "\\Seen", 1);
                /* Put in archive */
                label_idx_add("_nolabel", uid);
                /* Mark for immediate visual feedback (yellow strikethrough) */
                if (pending_label) pending_label[ei_cur] = 1;
                snprintf(feedback_msg, sizeof(feedback_msg), "Archived");
                break;
            }
            if (ch == 'D' && is_gmail) {
                /* No-op in Trash view: message is already in Trash */
                if (strcmp(folder, "_trash") == 0) {
                    snprintf(feedback_msg, sizeof(feedback_msg),
                             "Already in Trash \xe2\x80\x94 no change");
                    break;
                }
                const char *uid = entries[ei_cur].uid;
                if (pending_remove && pending_remove[ei_cur]) {
                    /* Undo: second 'D' restores from Trash back to current folder */
                    label_idx_remove("_trash", uid);
                    const char *restore_lbl = (folder[0] != '_') ? folder : "INBOX";
                    label_idx_add(restore_lbl, uid);
                    /* Update .hdr: remove TRASH, add current folder label */
                    {
                        const char *add_lbl = restore_lbl;
                        const char *rm_lbl  = "TRASH";
                        local_hdr_update_labels("", uid, &add_lbl, 1, &rm_lbl, 1);
                    }
                    /* Gmail API: remove TRASH label, add folder label back */
                    if (list_mc) {
                        mail_client_modify_label(list_mc, uid, "TRASH", 0);
                        mail_client_modify_label(list_mc, uid, restore_lbl, 1);
                    }
                    pending_remove[ei_cur] = 0;
                    snprintf(feedback_msg, sizeof(feedback_msg),
                             "Undo: %s restored", restore_lbl);
                } else {
                    /* First 'D': Gmail compound trash operation */
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
                    /* Mark for immediate visual feedback (red strikethrough) */
                    if (pending_remove) pending_remove[ei_cur] = 1;
                    snprintf(feedback_msg, sizeof(feedback_msg), "Moved to Trash");
                }
                break;
            }
            if (ch == 'u' && is_gmail) {
                /* Untrash: restore from Trash to INBOX. */
                const char *uid = entries[ei_cur].uid;
                label_idx_remove("_trash", uid);
                label_idx_add("INBOX", uid);
                /* Update .hdr: remove TRASH from labels, add INBOX */
                {
                    const char *add_lbl = "INBOX";
                    const char *rm_lbl  = "TRASH";
                    local_hdr_update_labels("", uid, &add_lbl, 1, &rm_lbl, 1);
                }
                /* Gmail API: remove TRASH label, add INBOX */
                if (list_mc) {
                    mail_client_modify_label(list_mc, uid, "TRASH", 0);
                    mail_client_modify_label(list_mc, uid, "INBOX", 1);
                }
                /* Mark for immediate visual feedback (green strikethrough) */
                if (pending_restore) pending_restore[ei_cur] = 1;
                snprintf(feedback_msg, sizeof(feedback_msg), "Restored to Inbox");
                break;
            }
            if (ch == 't' && is_gmail) {
                const char *uid = entries[ei_cur].uid;
                /* Remember whether this message is currently in Archive or Trash
                 * so we can show green feedback if a label addition removes it. */
                int was_archived = label_idx_contains("_nolabel", uid);
                int was_trashed  = label_idx_contains("_trash",   uid);
                show_label_picker(list_mc, uid, feedback_msg, sizeof(feedback_msg));
                /* If the picker added a real label that moved the message out
                 * of Archive (_nolabel) or Trash (_trash), mark row green. */
                if ((was_archived && !label_idx_contains("_nolabel", uid)) ||
                    (was_trashed  && !label_idx_contains("_trash",   uid)))
                    if (pending_restore) pending_restore[ei_cur] = 1;
                break;
            }
            if (ch == 'd' && is_gmail) {
                /* Toggle: first 'd' removes the label (yellow pending_label);
                 * second 'd' on the same row restores it (undo).
                 * Restricted to non-meta labels (no underscore prefix). */
                if (folder[0] != '_') {
                    const char *uid = entries[ei_cur].uid;
                    if (pending_label && pending_label[ei_cur]) {
                        /* Undo: restore the label that was removed */
                        if (list_mc)
                            mail_client_modify_label(list_mc, uid, folder, 1);
                        label_idx_add(folder, uid);
                        local_hdr_update_labels("", uid, &folder, 1, NULL, 0);
                        /* Remove from archive fallback if it was added */
                        label_idx_remove("_nolabel", uid);
                        pending_label[ei_cur] = 0;
                        snprintf(feedback_msg, sizeof(feedback_msg),
                                 "Undo: %s restored", folder);
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
                        /* Mark row for immediate visual feedback (yellow strikethrough).
                         * Also clear pending_remove so yellow takes priority over red
                         * if 'D' was pressed before 'd' on this row. */
                        if (pending_remove) pending_remove[ei_cur] = 0;
                        if (pending_label)  pending_label[ei_cur]  = 1;
                        snprintf(feedback_msg, sizeof(feedback_msg),
                                 "Label removed: %s", folder);
                    }
                }
                break;
            }
            if (ch == 'j') {
                /* Toggle junk: uses dedicated mark_junk / mark_notjunk */
                const char *uid = entries[ei_cur].uid;
                int is_junk = entries[ei_cur].flags & MSG_FLAG_JUNK;
                if (is_junk) {
                    entries[ei_cur].flags &= ~MSG_FLAG_JUNK;
                } else {
                    entries[ei_cur].flags |= MSG_FLAG_JUNK;
                }
                ManifestEntry *me = manifest_find(manifest, uid);
                if (me) me->flags = entries[ei_cur].flags;
                if (is_virtual_flags) {
                    Manifest *fm = manifest_load(efolder);
                    if (fm) {
                        ManifestEntry *fme = manifest_find(fm, uid);
                        if (fme) { fme->flags = entries[ei_cur].flags; manifest_save(efolder, fm); }
                        manifest_free(fm);
                    }
                } else if (!is_virtual_search) {
                    manifest_save(efolder, manifest);
                }
                if (is_gmail) {
                    junk_push_background(cfg, uid, !is_junk);
                } else if (list_mc) {
                    if (is_junk)
                        mail_client_mark_notjunk(list_mc, uid);
                    else
                        mail_client_mark_junk(list_mc, uid);
                }
                snprintf(feedback_msg, sizeof(feedback_msg),
                         is_junk ? "Marked as not-junk" : "Marked as junk");
            } else if (ch == 'n' || ch == 'f' || ch == 'd') {
                const char *uid = entries[ei_cur].uid;
                int bit;
                const char *flag_name;
                if (ch == 'n') {
                    bit = MSG_FLAG_UNSEEN;  flag_name = "\\Seen";
                } else if (ch == 'f') {
                    bit = MSG_FLAG_FLAGGED; flag_name = "\\Flagged";
                } else {
                    bit = MSG_FLAG_DONE;    flag_name = "$Done";
                }
                int currently = entries[ei_cur].flags & bit;
                /* Determine the IMAP add/remove direction */
                int add_flag = (ch == 'n') ? (currently ? 1 : 0) : (!currently ? 1 : 0);

                /* Local update first — instant UI response regardless of network */
                local_pending_flag_add(efolder, uid, flag_name, add_flag);
                entries[ei_cur].flags ^= bit;
                ManifestEntry *me = manifest_find(manifest, uid);
                if (me) me->flags = entries[ei_cur].flags;
                if (is_virtual_flags) {
                    /* Virtual list: save the real per-folder manifest, not the
                     * synthesized one — only then will manifest_count_all_flags
                     * return updated counts for the folder list. */
                    Manifest *fm = manifest_load(efolder);
                    if (fm) {
                        ManifestEntry *fme = manifest_find(fm, uid);
                        if (fme) { fme->flags = entries[ei_cur].flags; manifest_save(efolder, fm); }
                        manifest_free(fm);
                    }
                } else if (!is_virtual_search) {
                    manifest_save(efolder, manifest);
                }

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
                        local_hdr_update_flags("", uid, entries[ei_cur].flags);
                    }
                    flag_push_background(cfg, uid, flag_name, add_flag);
                } else if (list_mc) {
                    /* IMAP online mode: connection already open, push immediately */
                    mail_client_set_flag(list_mc, uid, flag_name, add_flag);
                }
                /* Feedback message */
                if (ch == 'n')
                    snprintf(feedback_msg, sizeof(feedback_msg),
                             currently ? "Marked as read" : "Marked as unread");
                else if (ch == 'f')
                    snprintf(feedback_msg, sizeof(feedback_msg),
                             currently ? "Unstarred" : "Starred");
                else /* 'd' IMAP done toggle */
                    snprintf(feedback_msg, sizeof(feedback_msg),
                             currently ? "Marked not done" : "Marked done");
            }
            break;
        }
        case TERM_KEY_NEXT_LINE:
            if (cursor < disp_count - 1) cursor++;
            break;
        case TERM_KEY_PREV_LINE:
            if (cursor > 0) cursor--;
            break;
        case TERM_KEY_NEXT_PAGE:
            cursor += limit;
            if (cursor >= disp_count) cursor = disp_count > 0 ? disp_count - 1 : 0;
            break;
        case TERM_KEY_PREV_PAGE:
            cursor -= limit;
            if (cursor < 0) cursor = 0;
            break;
        }
    }
list_done:
    free(pending_remove);
    free(pending_label);
    free(pending_restore);
    free(fentries);
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
    if (cfg->gmail_mode) {
        fprintf(stderr, "Error: 'list-folders' is IMAP-only. Use 'list-labels' for Gmail.\n");
        return -1;
    }
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

    /* Virtual prefix rows: [0] "Tags / Flags" header, [1] Unread,
     *                      [2] Flagged,              [3] "Folders" header */
    enum { VP_HDR_FLAGS=0, VP_UNREAD=1, VP_FLAGGED=2, VP_JUNK=3, VP_PHISHING=4,
           VP_ANSWERED=5, VP_FORWARDED=6, VP_HDR_FOLD=7, VPREFIX=8 };
    int vf_unread=0, vf_flagged=0, vf_junk=0, vf_phishing=0, vf_answered=0, vf_forwarded=0;
    manifest_count_all_flags(&vf_unread, &vf_flagged, &vf_junk, &vf_phishing,
                             &vf_answered, &vf_forwarded);

    int cursor = VPREFIX, wstart = 0;  /* default: first real folder */
    int tree_mode = ui_pref_get_int("folder_view_mode", 1);
    char current_prefix[512] = "";   /* flat mode: current navigation level */

    /* Pre-position cursor on current_folder (offset by VPREFIX).
     * INBOX is case-insensitive per RFC 3501 — use strcasecmp so that a
     * config value of "inbox" still matches the server's "INBOX". */
    if (current_folder && *current_folder) {
        if (tree_mode) {
            for (int i = 0; i < count; i++) {
                if (strcasecmp(folders[i], current_folder) == 0) {
                    cursor = VPREFIX + i; break;
                }
            }
        } else {
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
                    cursor = VPREFIX + i; break;
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
            display_count = VPREFIX + count;
        } else {
            vcount = build_flat_view(folders, count, sep, current_prefix, vis);
            display_count = VPREFIX + vcount;
        }
        if (cursor >= display_count && display_count > 0)
            cursor = display_count - 1;
        /* Never land on a section header */
        if (cursor == VP_HDR_FLAGS || cursor == VP_HDR_FOLD) cursor = VP_UNREAD;

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
            /* Virtual prefix rows */
            if (i < VPREFIX) {
                if (i == VP_HDR_FLAGS || i == VP_HDR_FOLD) {
                    const char *htitle = (i == VP_HDR_FLAGS) ? "Tags / Flags" : "Folders";
                    printf("  \033[2m\u2500\u2500 %s ", htitle);
                    int used = 6 + (int)strlen(htitle) + 1;
                    for (int s = used; s < name_w + 28 - 2; s++) fputs("\u2500", stdout);
                    printf("\033[0m\n");
                } else {
                    const char *vname, *vcolor;
                    int vc;
                    switch (i) {
                        case VP_UNREAD:    vc=vf_unread;    vname="Unread";    vcolor="\033[32m"; break;
                        case VP_FLAGGED:   vc=vf_flagged;   vname="Flagged";   vcolor="\033[33m"; break;
                        case VP_JUNK:      vc=vf_junk;      vname="Junk";      vcolor="\033[33m"; break;
                        case VP_PHISHING:  vc=vf_phishing;  vname="Phishing";  vcolor="\033[31m"; break;
                        case VP_ANSWERED:  vc=vf_answered;  vname="Answered";  vcolor="\033[36m"; break;
                        case VP_FORWARDED: vc=vf_forwarded; vname="Forwarded"; vcolor="\033[36m"; break;
                        default:           vc=0; vname="?"; vcolor=""; break;
                    }
                    char cnt[16];
                    /* Virtual rows always show the count, even when zero */
                    if (vc == 0) snprintf(cnt, sizeof(cnt), "0");
                    else fmt_thou(cnt, sizeof(cnt), vc);
                    if (i == cursor) printf("\033[7m");
                    else if (vc == 0) printf("\033[2m");
                    else printf("%s", vcolor);
                    printf("  %6s  %7s  %-*s  %7s",
                           cnt, "-", name_w, vname, "-");
                    if (i == cursor) printf("\033[K\033[0m");
                    else printf("\033[0m");
                    printf("\n");
                }
                continue;
            }
            /* Real folder rows (offset by VPREFIX) */
            int ri = i - VPREFIX;
            if (tree_mode) {
                int msgs = statuses ? statuses[ri].messages : 0;
                int unsn = statuses ? statuses[ri].unseen   : 0;
                int flgd = statuses ? statuses[ri].flagged  : 0;
                print_folder_item(folders, count, ri, sep, 1, i == cursor, 0,
                                  msgs, unsn, flgd, name_w);
            } else {
                int fi = vis[ri];
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
                cursor = VPREFIX; wstart = 0;
            } else {
                if (go_up) {
                    *go_up = 1;
                } else {
                    if (current_folder && *current_folder)
                        selected = strdup(current_folder);
                }
                goto folders_int_done;
            }
            break;
        case TERM_KEY_ENTER:
            if (cursor == VP_UNREAD) {
                selected = strdup("__unread__");    goto folders_int_done;
            } else if (cursor == VP_FLAGGED) {
                selected = strdup("__flagged__");   goto folders_int_done;
            } else if (cursor == VP_JUNK) {
                selected = strdup("__junk__");      goto folders_int_done;
            } else if (cursor == VP_PHISHING) {
                selected = strdup("__phishing__");  goto folders_int_done;
            } else if (cursor == VP_ANSWERED) {
                selected = strdup("__answered__");  goto folders_int_done;
            } else if (cursor == VP_FORWARDED) {
                selected = strdup("__forwarded__"); goto folders_int_done;
            } else if (cursor == VP_HDR_FLAGS || cursor == VP_HDR_FOLD) {
                break; /* header row — ignore */
            } else if (tree_mode) {
                selected = strdup(folders[cursor - VPREFIX]);
                goto folders_int_done;
            } else if (display_count > VPREFIX) {
                int ri = cursor - VPREFIX;
                int fi = vis[ri];
                if (folder_has_children(folders, count, folders[fi], sep)) {
                    strncpy(current_prefix, folders[fi], sizeof(current_prefix) - 1);
                    current_prefix[sizeof(current_prefix) - 1] = '\0';
                    cursor = VPREFIX; wstart = 0;
                } else {
                    selected = strdup(folders[fi]);
                    goto folders_int_done;
                }
            }
            break;
        case TERM_KEY_NEXT_LINE:
            if (cursor < display_count - 1) {
                cursor++;
                if (cursor == VP_HDR_FLAGS || cursor == VP_HDR_FOLD) cursor++;
            }
            break;
        case TERM_KEY_PREV_LINE:
            if (cursor > 0) {
                cursor--;
                if (cursor == VP_HDR_FLAGS || cursor == VP_HDR_FOLD) {
                    if (cursor > 0) cursor--;
                }
            }
            break;
        case TERM_KEY_NEXT_PAGE:
            cursor += limit;
            if (cursor >= display_count) cursor = display_count > 0 ? display_count - 1 : 0;
            if (cursor == VP_HDR_FLAGS || cursor == VP_HDR_FOLD) cursor++;
            break;
        case TERM_KEY_PREV_PAGE:
            cursor -= limit;
            if (cursor < 0) cursor = 0;
            if (cursor == VP_HDR_FLAGS || cursor == VP_HDR_FOLD) cursor++;
            break;
        case TERM_KEY_HOME:
            cursor = VP_UNREAD; wstart = 0;
            break;
        case TERM_KEY_END:
            cursor = display_count > 0 ? display_count - 1 : VP_UNREAD;
            break;
        case TERM_KEY_LEFT:
        case TERM_KEY_RIGHT:
        case TERM_KEY_DELETE:
        case TERM_KEY_TAB:
        case TERM_KEY_SHIFT_TAB:
        case TERM_KEY_IGNORE: {
            int ch = terminal_last_printable();
            if (ch == '/') {
                /* Cross-folder content search */
                static const char *snames[] = {"Subject","From","To","Body"};
                int sscope = 0;
                char sbuf[256] = "";
                int slen = 0;
                int srows = terminal_rows(), scols = terminal_cols();
                if (srows <= 0) srows = 24;
                for (;;) {
                    printf("\033[%d;1H\033[K  Search [%s]: %s_",
                           srows - 1, snames[sscope], sbuf);
                    fflush(stdout);
                    TermKey ikey = terminal_read_key();
                    if (ikey == TERM_KEY_ESC || ikey == TERM_KEY_QUIT) break;
                    if (ikey == TERM_KEY_ENTER) {
                        if (sbuf[0]) {
                            char sfolder[512];
                            snprintf(sfolder, sizeof(sfolder),
                                     "__search__:%d:%s", sscope, sbuf);
                            selected = strdup(sfolder);
                            goto folders_int_done;
                        }
                        break;
                    }
                    if (ikey == TERM_KEY_TAB) {
                        sscope = (sscope + 1) % 4;
                    } else if (ikey == TERM_KEY_BACK) {
                        /* Remove last UTF-8 character */
                        while (slen > 0 && (sbuf[slen - 1] & 0xC0) == 0x80) slen--;
                        if (slen > 0) sbuf[--slen] = '\0';
                    } else if (terminal_last_utf8()[0]) {
                        const char *u8 = terminal_last_utf8();
                        size_t ulen = strlen(u8);
                        if (slen + (int)ulen < (int)sizeof(sbuf)) {
                            memcpy(sbuf + slen, u8, ulen + 1);
                            slen += (int)ulen;
                        }
                    }
                }
                (void)scols;
            } else if (ch == 't') {
                tree_mode = !tree_mode;
                ui_pref_set_int("folder_view_mode", tree_mode);
                cursor = VPREFIX; wstart = 0;
                if (!tree_mode) current_prefix[0] = '\0';
            } else if (ch == 'c') {
                selected = strdup("__compose__");
                goto folders_int_done;
            } else if (ch == 'h' || ch == '?') {
                static const char *help[][2] = {
                    { "\u2191 / \u2193",   "Move cursor up / down"                   },
                    { "PgUp / PgDn",        "Move cursor one page up / down"          },
                    { "Enter",             "Open folder / navigate into subfolder"   },
                    { "/",                 "Cross-folder content search"             },
                    { "c",                 "Compose new message"                     },
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
static void show_label_picker(MailClient *mc, const char *uid,
                               char *feedback_out, int feedback_cap) {
    /* Collect available labels from local .idx files */
    char **all_labels = NULL;
    int all_count = 0;
    label_idx_list(&all_labels, &all_count);

    /* Build display: system labels (UNREAD, INBOX, STARRED, SENT, DRAFTS) first,
     * then user-defined labels.  Skip _nolabel, _spam, _trash (system-managed). */
    char **pick_ids   = NULL;
    char **pick_names = NULL;
    int  *pick_on     = NULL;
    int  pick_count = 0, pick_cap = 0;

    /* Add system labels first */
    static const char *sys_pick[] = {"UNREAD", "INBOX", "STARRED", "SENT", "DRAFTS"};
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
        if (pick_count == pick_cap) {
            int nc = pick_cap ? pick_cap * 2 : 16;
            pick_ids   = realloc(pick_ids,   (size_t)nc * sizeof(char *));
            pick_names = realloc(pick_names, (size_t)nc * sizeof(char *));
            pick_on    = realloc(pick_on,    (size_t)nc * sizeof(int));
            pick_cap   = nc;
        }
        pick_ids[pick_count]   = all_labels[i]; /* transfer ownership */
        char *resolved = local_gmail_label_name_lookup(all_labels[i]);
        pick_names[pick_count] = resolved ? resolved : strdup(all_labels[i]);
        pick_on[pick_count]    = label_idx_contains(all_labels[i], uid);
        pick_count++;
    }
    free(all_labels);

    if (pick_count == 0) {
        free(pick_ids); free(pick_names); free(pick_on);
        return;
    }

    /* Remember initial label state for post-picker feedback computation */
    int *pick_initial = malloc((size_t)pick_count * sizeof(int));
    if (pick_initial)
        memcpy(pick_initial, pick_on, (size_t)pick_count * sizeof(int));
    /* Capture virtual-folder membership before any changes */
    int was_in_nolabel = label_idx_contains("_nolabel", uid);
    int was_in_trash   = label_idx_contains("_trash", uid);

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
                /* Adding a real label (not UNREAD/STARRED/CATEGORY_) implicitly
                 * moves the message out of virtual archive (_nolabel) and/or trash.
                 * Remove the old virtual location from local index + .hdr + API. */
                if (strcmp(lid, "UNREAD") != 0 &&
                    strcmp(lid, "STARRED") != 0 &&
                    strncmp(lid, "CATEGORY_", 9) != 0) {
                    /* Unarchive: remove from _nolabel virtual archive index */
                    if (label_idx_contains("_nolabel", uid))
                        label_idx_remove("_nolabel", uid);
                    /* Untrash: remove TRASH label from local index, .hdr, API */
                    if (label_idx_contains("_trash", uid)) {
                        const char *trash_id = "TRASH";
                        label_idx_remove("_trash", uid);
                        local_hdr_update_labels("", uid, NULL, 0, &trash_id, 1);
                        if (mc) mail_client_modify_label(mc, uid, "TRASH", 0);
                    }
                }
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

    /* Compute feedback: diff pick_initial vs pick_on */
    if (feedback_out && feedback_cap > 0 && pick_initial) {
        int added = 0, removed = 0;
        char added_name[64]   = "";
        char removed_name[64] = "";
        int real_added = 0;
        for (int i = 0; i < pick_count; i++) {
            if (pick_on[i] && !pick_initial[i]) {
                added++;
                if (!added_name[0]) {
                    strncpy(added_name, pick_names[i], sizeof(added_name) - 1);
                    added_name[sizeof(added_name) - 1] = '\0';
                }
                /* "Real" label = not UNREAD/STARRED/CATEGORY_ */
                if (strcmp(pick_ids[i], "UNREAD")    != 0 &&
                    strcmp(pick_ids[i], "STARRED")   != 0 &&
                    strncmp(pick_ids[i], "CATEGORY_", 9) != 0)
                    real_added = 1;
            }
            if (!pick_on[i] && pick_initial[i]) {
                removed++;
                if (!removed_name[0]) {
                    strncpy(removed_name, pick_names[i], sizeof(removed_name) - 1);
                    removed_name[sizeof(removed_name) - 1] = '\0';
                }
            }
        }
        if (added + removed > 0) {
            if (was_in_trash && real_added)
                snprintf(feedback_out, (size_t)feedback_cap,
                         "%s added \xe2\x80\x94 restored from Trash", added_name);
            else if (was_in_nolabel && real_added)
                snprintf(feedback_out, (size_t)feedback_cap,
                         "%s added \xe2\x80\x94 moved out of Archive", added_name);
            else if (added + removed == 1) {
                if (added == 1)
                    snprintf(feedback_out, (size_t)feedback_cap,
                             "Label added: %s", added_name);
                else
                    snprintf(feedback_out, (size_t)feedback_cap,
                             "Label removed: %s", removed_name);
            } else {
                snprintf(feedback_out, (size_t)feedback_cap, "Labels updated");
            }
        }
        /* If no changes, leave feedback_out unchanged (criterion 7) */
    }

    free(pick_initial);
    for (int i = 0; i < pick_count; i++) { free(pick_ids[i]); free(pick_names[i]); }
    free(pick_ids); free(pick_names); free(pick_on);
}

/* ── Gmail Label List (interactive) ──────────────────────────────────── */

/* System labels in display order.  id = .idx filename, name = display name. */
static const struct { const char *id; const char *name; } gmail_system_labels[] = {
    { "UNREAD",   "Unread"  },
    { "STARRED",  "Flagged" },
    { "INBOX",    "Inbox"   },
    { "SENT",     "Sent"    },
    { "DRAFTS",   "Drafts"  },
};
#define GMAIL_SYS_COUNT ((int)(sizeof(gmail_system_labels)/sizeof(gmail_system_labels[0])))
/* First 2 system labels are Tags/Flags; the rest (INBOX, SENT, DRAFTS) are Folders */
#define GMAIL_SYS_FLAGS   2
#define GMAIL_SYS_FOLDERS (GMAIL_SYS_COUNT - GMAIL_SYS_FLAGS)

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
 * Layout: "── Tags / Flags ──" header, UNREAD+STARRED, user labels,
 *         "── Folders ──" header, INBOX+SENT+DRAFTS, categories, special.
 * is_header[i]=1 marks non-selectable section header rows. */
static int build_label_display(
    char ***ids_out, char ***names_out, int **sep_out, int **is_header_out,
    char **user_labels, int user_count,
    char **cat_labels,  int cat_count)
{
    /* 2 section headers + flags + user + folders + cats + special */
    int total = 2 + GMAIL_SYS_FLAGS + user_count
              + GMAIL_SYS_FOLDERS + cat_count + GMAIL_SPECIAL_COUNT;
    char **ids   = calloc((size_t)total, sizeof(char *));
    char **names = calloc((size_t)total, sizeof(char *));
    int  *seps   = calloc((size_t)total, sizeof(int));
    int  *hdrs   = calloc((size_t)total, sizeof(int));
    if (!ids || !names || !seps || !hdrs) {
        free(ids); free(names); free(seps); free(hdrs); return 0;
    }

    int n = 0;

    /* ── Tags / Flags section ──────────────────────────────────────────── */
    ids[n] = strdup("__header__"); names[n] = strdup("Tags / Flags"); hdrs[n] = 1; n++;
    for (int i = 0; i < GMAIL_SYS_FLAGS; i++) {
        ids[n]   = strdup(gmail_system_labels[i].id);
        names[n] = strdup(gmail_system_labels[i].name);
        n++;
    }
    for (int i = 0; i < user_count; i++) {
        ids[n]   = strdup(user_labels[i]);
        char *disp = local_gmail_label_name_lookup(user_labels[i]);
        names[n] = disp ? disp : strdup(user_labels[i]);
        n++;
    }

    /* ── Folders section ───────────────────────────────────────────────── */
    ids[n] = strdup("__header__"); names[n] = strdup("Folders"); hdrs[n] = 1; n++;
    for (int i = GMAIL_SYS_FLAGS; i < GMAIL_SYS_COUNT; i++) {
        ids[n]   = strdup(gmail_system_labels[i].id);
        names[n] = strdup(gmail_system_labels[i].name);
        n++;
    }
    for (int i = 0; i < cat_count; i++) {
        ids[n] = strdup(cat_labels[i]);
        const char *disp = cat_labels[i];
        for (int k = 0; k < GMAIL_CAT_COUNT; k++)
            if (strcmp(cat_labels[i], gmail_cat_labels[k].id) == 0)
                { disp = gmail_cat_labels[k].name; break; }
        names[n] = strdup(disp);
        n++;
    }
    /* Special labels with a thin separator before the first */
    seps[n] = 1;
    for (int i = 0; i < GMAIL_SPECIAL_COUNT; i++) {
        ids[n]   = strdup(gmail_special_labels[i].id);
        names[n] = strdup(gmail_special_labels[i].name);
        n++;
    }

    *ids_out        = ids;
    *names_out      = names;
    *sep_out        = seps;
    *is_header_out  = hdrs;
    return n;
}

static void free_label_display(char **ids, char **names, int *seps, int *hdrs, int count) {
    for (int i = 0; i < count; i++) { free(ids[i]); free(names[i]); }
    free(ids); free(names); free(seps); free(hdrs);
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
    int *lbl_seps = NULL, *lbl_hdr = NULL;
    int lbl_count = build_label_display(&lbl_ids, &lbl_names, &lbl_seps, &lbl_hdr,
                                        user_labels, user_count,
                                        cat_labels, cat_count);
    for (int i = 0; i < user_count; i++) free(user_labels[i]);
    free(user_labels);
    for (int i = 0; i < cat_count; i++) free(cat_labels[i]);
    free(cat_labels);

    if (lbl_count == 0) {
        free_label_display(lbl_ids, lbl_names, lbl_seps, lbl_hdr, 0);
        /* Show an empty "Labels" screen so the user can press Backspace to return. */
        RAII_TERM_RAW TermRawState *_raw = terminal_raw_enter();
        (void)_raw;
        int _tc = terminal_cols(), _tr = terminal_rows();
        if (_tc <= 0) _tc = 80;
        if (_tr <= 0) _tr = 24;
        printf("\033[H\033[2J");
        {
            char _cl[256];
            snprintf(_cl, sizeof(_cl), "  Labels \u2014 %s  (0)",
                     cfg->user ? cfg->user : "?");
            printf("\033[7m%s", _cl);
            int _used = visible_line_cols(_cl, _cl + strlen(_cl));
            for (int _p = _used; _p < _tc; _p++) putchar(' ');
            printf("\033[0m\n\n");
        }
        printf("  No labels synced yet. Run 'email-sync' to populate.\n");
        fflush(stdout);
        print_statusbar(_tr, _tc, "  Backspace=back  ESC=quit");
        for (;;) {
            TermKey _k = terminal_read_key();
            if (_k == TERM_KEY_BACK) { if (go_up) *go_up = 1; return NULL; }
            if (_k == TERM_KEY_QUIT || _k == TERM_KEY_ESC) return NULL;
        }
    }

    int cursor = 0, wstart = 0;
    char *selected = NULL;

    /* Pre-position cursor on current_label; skip header rows */
    if (current_label && *current_label) {
        for (int i = 0; i < lbl_count; i++) {
            if (!lbl_hdr[i] && strcmp(lbl_ids[i], current_label) == 0) {
                cursor = i; break;
            }
        }
    }
    /* Ensure initial cursor is not on a header row */
    while (cursor < lbl_count - 1 && lbl_hdr[cursor]) cursor++;

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
            /* Section header row */
            if (lbl_hdr[i]) {
                printf("  \033[2m\u2500\u2500 %s ", lbl_names[i]);
                int used = 6 + (int)strlen(lbl_names[i]) + 1;
                for (int s = used; s < tcols - 2; s++) fputs("\u2500", stdout);
                printf("\033[0m\n");
                continue;
            }
            /* Thin separator before certain groups (not before the first visible item) */
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
            while (cursor < lbl_count - 1 && lbl_hdr[cursor]) cursor++;
            break;
        case TERM_KEY_END:
            cursor = lbl_count > 0 ? lbl_count - 1 : 0;
            while (cursor > 0 && lbl_hdr[cursor]) cursor--;
            break;
        case TERM_KEY_NEXT_LINE:
            if (cursor < lbl_count - 1) {
                cursor++;
                while (cursor < lbl_count - 1 && lbl_hdr[cursor]) cursor++;
                if (lbl_hdr[cursor]) cursor--;
            }
            break;
        case TERM_KEY_PREV_LINE:
            if (cursor > 0) {
                cursor--;
                while (cursor > 0 && lbl_hdr[cursor]) cursor--;
            }
            break;
        case TERM_KEY_NEXT_PAGE:
            cursor += avail;
            if (cursor >= lbl_count) cursor = lbl_count - 1;
            while (cursor > 0 && lbl_hdr[cursor]) cursor--;
            break;
        case TERM_KEY_PREV_PAGE:
            cursor -= avail;
            if (cursor < 0) cursor = 0;
            while (cursor < lbl_count - 1 && lbl_hdr[cursor]) cursor++;
            break;
        case TERM_KEY_ENTER:
            if (!lbl_hdr[cursor])
                selected = strdup(lbl_ids[cursor]);
            goto labels_done;
        default: {
            int ch = terminal_last_printable();
            if (ch == '/') {
                /* Cross-folder content search */
                static const char *snames[] = {"Subject","From","To","Body"};
                int sscope = 0;
                char sbuf[256] = "";
                int slen = 0;
                int srows = trows;
                for (;;) {
                    printf("\033[%d;1H\033[K  Search [%s]: %s_",
                           srows - 1, snames[sscope], sbuf);
                    fflush(stdout);
                    TermKey ikey = terminal_read_key();
                    if (ikey == TERM_KEY_ESC || ikey == TERM_KEY_QUIT) break;
                    if (ikey == TERM_KEY_ENTER) {
                        if (sbuf[0]) {
                            char sfolder[512];
                            snprintf(sfolder, sizeof(sfolder),
                                     "__search__:%d:%s", sscope, sbuf);
                            selected = strdup(sfolder);
                            goto labels_done;
                        }
                        break;
                    }
                    if (ikey == TERM_KEY_TAB) {
                        sscope = (sscope + 1) % 4;
                    } else if (ikey == TERM_KEY_BACK) {
                        while (slen > 0 && (sbuf[slen - 1] & 0xC0) == 0x80) slen--;
                        if (slen > 0) sbuf[--slen] = '\0';
                    } else if (terminal_last_utf8()[0]) {
                        const char *u8 = terminal_last_utf8();
                        size_t ulen = strlen(u8);
                        if (slen + (int)ulen < (int)sizeof(sbuf)) {
                            memcpy(sbuf + slen, u8, ulen + 1);
                            slen += (int)ulen;
                        }
                    }
                }
                break;
            }
            if (ch == 'h' || ch == '?') {
                static const char *help[][2] = {
                    { "\u2191 / \u2193",   "Move cursor up / down"      },
                    { "PgUp / PgDn",       "Page up / down"             },
                    { "Enter",             "Open selected label"        },
                    { "/",                 "Cross-folder content search"},
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
                        free_label_display(lbl_ids, lbl_names, lbl_seps, lbl_hdr, lbl_count);
                        lbl_hdr = NULL;
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
                        lbl_count = build_label_display(&lbl_ids, &lbl_names, &lbl_seps, &lbl_hdr,
                                                        ul2, uc2, cl2, cc2);
                        for (int i = 0; i < uc2; i++) free(ul2[i]);
                        free(ul2);
                        for (int i = 0; i < cc2; i++) free(cl2[i]);
                        free(cl2);
                        if (lbl_count == 0) {
                            free_label_display(lbl_ids, lbl_names, lbl_seps, lbl_hdr, 0);
                            goto labels_done;
                        }
                    }
                }
            }
            if (ch == 'd' && lbl_count > 0 && !lbl_hdr[cursor]) {
                /* Delete selected label — use the label name as ID (best effort).
                 * TODO: use label ID instead of name for Gmail (ID != name for
                 *       user-defined labels). For IMAP this is correct (name == ID). */
                const char *del_id = lbl_names[cursor];
                if (del_id) {
                    email_service_delete_label(cfg, del_id);
                    /* Rebuild display after deletion */
                    free_label_display(lbl_ids, lbl_names, lbl_seps, lbl_hdr, lbl_count);
                    lbl_hdr = NULL;
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
                    lbl_count = build_label_display(&lbl_ids, &lbl_names, &lbl_seps, &lbl_hdr,
                                                    ul3, uc3, cl3, cc3);
                    for (int i = 0; i < uc3; i++) free(ul3[i]);
                    free(ul3);
                    for (int i = 0; i < cc3; i++) free(cl3[i]);
                    free(cl3);
                    if (cursor >= lbl_count) cursor = lbl_count - 1;
                    if (cursor < 0) cursor = 0;
                    while (cursor < lbl_count - 1 && lbl_hdr[cursor]) cursor++;
                    if (lbl_count == 0) {
                        free_label_display(lbl_ids, lbl_names, lbl_seps, lbl_hdr, 0);
                        goto labels_done;
                    }
                }
            }
            break;
        }
        }
    }
labels_done:
    free_label_display(lbl_ids, lbl_names, lbl_seps, lbl_hdr, lbl_count);
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

int email_service_account_interactive(Config **cfg_out, int *cursor_inout,
                                      const char *flash_msg) {
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
        if (flash_msg) {
            print_infoline(trows, tcols, flash_msg);
            flash_msg = NULL;
        }

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

    print_show_headers(from, subject, date, uid, ro_labels);

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

#define SHOW_HDR_LINES 6
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
                print_show_headers(from, subject, date, uid, ro_labels);
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

/** Convert bare LF to CRLF throughout msg, and ensure a trailing CRLF.
 *  RFC 3501 §4.3 requires message literals to use CRLF line endings.
 *  Returns a heap-allocated NUL-terminated string; sets *len_out.
 *  Returns NULL on allocation failure. */
static char *msg_to_crlf(const char *msg, size_t *len_out) {
    size_t in_len = strlen(msg);
    size_t bare_lf = 0;
    for (size_t i = 0; i < in_len; i++)
        if (msg[i] == '\n' && (i == 0 || msg[i-1] != '\r'))
            bare_lf++;
    /* need_trail: does the output need a final CRLF appended?
     * If the input ends with \n (bare or as part of \r\n), the CRLF loop
     * already produces a trailing \r\n in the output — no extra needed.
     * Only add \r\n when the input has no terminal newline at all. */
    int need_trail = (in_len == 0 || msg[in_len - 1] != '\n');
    size_t out_len = in_len + bare_lf + (need_trail ? 2 : 0);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        if (msg[i] == '\n' && (i == 0 || msg[i-1] != '\r'))
            out[j++] = '\r';
        out[j++] = msg[i];
    }
    if (need_trail) { out[j++] = '\r'; out[j++] = '\n'; }
    out[j] = '\0';
    *len_out = j;
    return out;
}

int email_service_sync(const Config *cfg, int force_reconcile) {
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
        int rc = force_reconcile ? gmail_sync_full(gc) : gmail_sync(gc);
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

    /* Upload locally-queued outgoing messages (sent/draft) to the server.
     * A dedicated connection is used so that a slow or failed APPEND never
     * corrupts the main sync connection used for folder operations. */
    {
        int pac = 0;
        PendingAppend *pa = local_pending_append_load(&pac);
        if (pa && pac > 0) {
            printf("Uploading %d pending message(s)...\n", pac);
            fflush(stdout);
            RAII_MAIL MailClient *append_mc = make_mail(cfg);
            if (!append_mc) {
                printf("  (Upload skipped: cannot connect; will retry on next sync.)\n");
            } else {
                for (int i = 0; i < pac; i++) {
                    char *raw = local_msg_load(pa[i].folder, pa[i].uid);
                    if (!raw) {
                        local_pending_append_remove(pa[i].folder, pa[i].uid);
                        continue;
                    }
                    /* RFC 3501 requires CRLF line endings throughout the message
                     * body.  Normalise bare LF so strict servers accept the literal. */
                    size_t append_len = 0;
                    char *append_msg = msg_to_crlf(raw, &append_len);
                    if (!append_msg) { append_msg = raw; append_len = strlen(raw); }
                    printf("  → %s ...", pa[i].folder); fflush(stdout);
                    if (mail_client_append(append_mc, pa[i].folder, append_msg, append_len) == 0) {
                        local_msg_delete(pa[i].folder, pa[i].uid);
                        Manifest *mf = manifest_load(pa[i].folder);
                        if (mf) {
                            manifest_remove(mf, pa[i].uid);
                            manifest_save(pa[i].folder, mf);
                            manifest_free(mf);
                        }
                        local_pending_append_remove(pa[i].folder, pa[i].uid);
                        printf(" uploaded.\n");
                    } else {
                        printf(" failed (retry on next sync).\n");
                    }
                    if (append_msg != raw) free(append_msg);
                    free(raw);
                }
            }
        }
        free(pa);
    }

    /* One shared mail client connection for all folder operations */
    RAII_MAIL MailClient *sync_mc = make_mail(cfg);
    if (!sync_mc) {
        fprintf(stderr, "sync: could not connect to mail server.\n");
        for (int i = 0; i < folder_count; i++) free(folders[i]);
        free(folders);
        if (pid_path[0]) unlink(pid_path);
        return -1;
    }

    MailRules *imap_rules = mail_rules_load(local_store_account_name());

    for (int fi = 0; fi < folder_count; fi++) {
        const char *folder = folders[fi];
        printf("Syncing %s ...\n", folder);
        fflush(stdout);

        /* ── Load saved CONDSTORE sync state ──────────────────────────── */
        FolderSyncState saved_state = {0, 0};
        int have_saved = (!force_reconcile &&
                          local_sync_state_load(folder, &saved_state) == 0);

        ImapSelectResult sel = {0};
        if (mail_client_select_ext(sync_mc, folder,
                                   have_saved ? saved_state.uidvalidity : 0,
                                   have_saved ? saved_state.highestmodseq : 0,
                                   &sel) != 0) {
            fprintf(stderr, "  WARN: SELECT failed for %s\n", folder);
            errors++;
            continue;
        }

        /* ── Fast path: no changes since last sync ─────────────────────── */
        if (have_saved && sel.highestmodseq != 0 &&
            sel.highestmodseq == saved_state.highestmodseq &&
            sel.uidvalidity   == saved_state.uidvalidity) {
            printf("  (up to date, modseq=%llu)\n",
                   (unsigned long long)sel.highestmodseq);
            free(sel.vanished_uids);
            continue;
        }

        /* ── UIDVALIDITY changed: clear saved state, force full resync ── */
        if (have_saved && sel.uidvalidity != 0 &&
            sel.uidvalidity != saved_state.uidvalidity) {
            fprintf(stderr,
                    "  WARN: UIDVALIDITY changed for %s (%u→%u) — full resync\n",
                    folder, saved_state.uidvalidity, sel.uidvalidity);
            local_sync_state_clear(folder);
            have_saved = 0;
            saved_state.uidvalidity   = 0;
            saved_state.highestmodseq = 0;
        }

        /* incremental=1 when we have a valid saved modseq to use */
        int incremental = (have_saved && saved_state.highestmodseq != 0 &&
                           sel.highestmodseq != 0);

        /* ── SEARCH ALL: current UID set (needed in all paths) ─────────── */
        char (*uids)[17] = NULL;
        int  uid_count = 0;
        if (mail_client_search(sync_mc, MAIL_SEARCH_ALL, &uids, &uid_count) != 0) {
            fprintf(stderr, "  WARN: SEARCH ALL failed for %s\n", folder);
            free(sel.vanished_uids);
            errors++;
            continue;
        }
        if (uid_count == 0) {
            printf("  (empty)\n");
            free(uids);
            free(sel.vanished_uids);
            /* Persist sync state even for empty folders */
            if (sel.uidvalidity && sel.highestmodseq) {
                FolderSyncState ns = { sel.uidvalidity, sel.highestmodseq };
                local_sync_state_save(folder, &ns);
            }
            continue;
        }

        /* Load or create manifest */
        Manifest *manifest = manifest_load(folder);
        if (!manifest) {
            manifest = calloc(1, sizeof(Manifest));
            if (!manifest) {
                fprintf(stderr, "  WARN: out of memory for manifest %s\n", folder);
                free(uids);
                free(sel.vanished_uids);
                errors++;
                continue;
            }
        }

        /* Flush pending folder moves before reading server state */
        {
            int mcount = 0;
            PendingMove *moves = local_pending_move_load(folder, &mcount);
            if (moves && mcount > 0) {
                for (int mi = 0; mi < mcount; mi++)
                    mail_client_move_to_folder(sync_mc,
                                               moves[mi].uid,
                                               moves[mi].target_folder);
                local_pending_move_clear(folder);
            }
            free(moves);
        }

        /* Flush pending local flag changes before reading server state */
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

        /* Evict deleted messages from manifest */
        manifest_retain(manifest, (const char (*)[17])uids, uid_count);

        /* ── Flag acquisition ─────────────────────────────────────────── */
        ImapFlagUpdate *change_updates = NULL;
        int             change_count   = 0;

        char (*unseen_uids)[17]  = NULL; int unseen_count  = 0;
        char (*flagged_uids)[17] = NULL; int flagged_count = 0;
        char (*done_uids)[17]    = NULL; int done_count    = 0;

        if (incremental) {
            /* CONDSTORE path: CHANGEDSINCE replaces three SEARCH commands */
            mail_client_fetch_flags_changedsince(sync_mc,
                                                 saved_state.highestmodseq,
                                                 &change_updates, &change_count);
            /* Apply flag updates to existing manifest entries now */
            for (int ui = 0; ui < change_count; ui++) {
                ManifestEntry *me = manifest_find(manifest, change_updates[ui].uid);
                if (me) me->flags = change_updates[ui].flags;
            }
        } else {
            /* Full path: three SEARCH commands */
            if (mail_client_search(sync_mc, MAIL_SEARCH_UNREAD,
                                   &unseen_uids, &unseen_count) != 0)
                unseen_count = 0;
            mail_client_search(sync_mc, MAIL_SEARCH_FLAGGED,
                               &flagged_uids, &flagged_count);
            mail_client_search(sync_mc, MAIL_SEARCH_DONE,
                               &done_uids, &done_count);
        }

        int fetched = 0, skipped = 0;
        for (int i = 0; i < uid_count; i++) {
            const char *uid = uids[i];
            int uid_flags = 0;

            if (incremental) {
                ManifestEntry *me = manifest_find(manifest, uid);
                if (me) {
                    /* Existing entry: flags already updated from CHANGEDSINCE above */
                    skipped++;
                    printf("  [%d/%d] UID %s\r", i + 1, uid_count, uid);
                    fflush(stdout);
                    continue;
                }
                /* New message: look up flags in change_updates */
                for (int ui = 0; ui < change_count; ui++) {
                    if (strcmp(change_updates[ui].uid, uid) == 0) {
                        uid_flags = change_updates[ui].flags;
                        break;
                    }
                }
                /* Default: new message is unseen */
                if (uid_flags == 0) uid_flags = MSG_FLAG_UNSEEN;
            } else {
                for (int j = 0; j < unseen_count;  j++)
                    if (strcmp(unseen_uids[j],  uid) == 0) { uid_flags |= MSG_FLAG_UNSEEN;  break; }
                for (int j = 0; j < flagged_count; j++)
                    if (strcmp(flagged_uids[j], uid) == 0) { uid_flags |= MSG_FLAG_FLAGGED; break; }
                for (int j = 0; j < done_count;    j++)
                    if (strcmp(done_uids[j],    uid) == 0) { uid_flags |= MSG_FLAG_DONE;    break; }
            }

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
                    /* Update contact suggestions from newly downloaded message */
                    {
                        char *from_h = mime_get_header(raw, "From");
                        char *to_h   = mime_get_header(raw, "To");
                        char *cc_h   = mime_get_header(raw, "Cc");
                        local_contacts_update(from_h, to_h, cc_h);
                        free(from_h); free(to_h); free(cc_h);
                    }
                    /* Apply sorting rules to new message */
                    if (imap_rules && imap_rules->count > 0) {
                        char *from_r = mime_get_header(raw, "From");
                        char *subj_r = mime_get_header(raw, "Subject");
                        char *to_r   = mime_get_header(raw, "To");
                        char *fr_dec = from_r ? mime_decode_words(from_r) : NULL;
                        char *su_dec = subj_r ? mime_decode_words(subj_r) : NULL;
                        char **add_labels = NULL; int add_count = 0;
                        char **rm_labels  = NULL; int rm_count  = 0;
                        int fired_count = mail_rules_apply(imap_rules,
                                             fr_dec ? fr_dec : "",
                                             su_dec ? su_dec : "",
                                             to_r   ? to_r   : "",
                                             NULL,  /* no label-based rules during sync */
                                             NULL, (time_t)0, /* body/date unavailable */
                                             &add_labels, &add_count,
                                             &rm_labels,  &rm_count);
                        if (fired_count > 0) {
                            if (g_verbose) {
                                for (int ri = 0; ri < imap_rules->count; ri++) {
                                    const MailRule *mr = &imap_rules->rules[ri];
                                    if (mail_rule_matches(mr,
                                                          fr_dec ? fr_dec : "",
                                                          su_dec ? su_dec : "",
                                                          to_r   ? to_r   : "",
                                                          NULL, NULL, (time_t)0)) {
                                        printf("  [rule] \"%s\" \xe2\x86\x92 uid:%s",
                                               mr->name ? mr->name : "?", uid);
                                        for (int j = 0; j < mr->then_add_count; j++)
                                            printf("  +%s", mr->then_add_label[j]);
                                        for (int j = 0; j < mr->then_rm_count; j++)
                                            printf("  -%s", mr->then_rm_label[j]);
                                        if (mr->then_move_folder)
                                            printf("  \xe2\x86\x92%s", mr->then_move_folder);
                                        printf("\n");
                                    }
                                }
                            }
                            /* Map label names to IMAP flags for local storage */
                            static const struct { const char *label; int flag; } lmap[] = {
                                { "_junk",      MSG_FLAG_JUNK      },
                                { "_spam",      MSG_FLAG_JUNK      },
                                { "_phishing",  MSG_FLAG_PHISHING  },
                                { "_done",      MSG_FLAG_DONE      },
                                { "_flagged",   MSG_FLAG_FLAGGED   },
                            };
                            for (int ai = 0; ai < add_count; ai++) {
                                for (int li = 0; li < (int)(sizeof(lmap)/sizeof(lmap[0])); li++) {
                                    if (strcasecmp(add_labels[ai], lmap[li].label) == 0)
                                        uid_flags |= lmap[li].flag;
                                }
                                free(add_labels[ai]);
                            }
                            for (int ri = 0; ri < rm_count; ri++) free(rm_labels[ri]);
                            free(add_labels); free(rm_labels);
                        }
                        free(from_r); free(subj_r); free(to_r);
                        free(fr_dec); free(su_dec);
                    }
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
        free(change_updates);
        free(unseen_uids);
        free(flagged_uids);
        free(done_uids);
        free(sel.vanished_uids);
        manifest_save(folder, manifest);
        manifest_free(manifest);
        free(uids);

        /* Persist sync state for incremental sync on next run */
        if (sel.uidvalidity && sel.highestmodseq) {
            FolderSyncState new_state = { sel.uidvalidity, sel.highestmodseq };
            local_sync_state_save(folder, &new_state);
        }

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

    mail_rules_free(imap_rules);
    imap_rules = NULL;

    /* Release PID lock */
    if (pid_path[0]) unlink(pid_path);

    return errors ? -1 : 0;
}

int email_service_sync_all(const char *only_account, int force_reconcile) {
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
        if (email_service_sync(accounts[i].cfg, force_reconcile) < 0)
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

/* ── IMAP per-account custom label helpers for apply_rules ──────────── */

typedef struct { char uid[17]; char *labels; } UidLabel;

static void ul_free_all(UidLabel *arr, int count) {
    for (int i = 0; i < count; i++) free(arr[i].labels);
    free(arr);
}

static const char *ul_get(const UidLabel *arr, int count, const char *uid) {
    for (int i = 0; i < count; i++)
        if (strcmp(arr[i].uid, uid) == 0) return arr[i].labels;
    return NULL;
}

static void ul_set(UidLabel **arr, int *count, int *cap, const char *uid, const char *lbl) {
    for (int i = 0; i < *count; i++) {
        if (strcmp((*arr)[i].uid, uid) != 0) continue;
        free((*arr)[i].labels);
        (*arr)[i].labels = strdup(lbl);
        return;
    }
    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        UidLabel *tmp = realloc(*arr, (size_t)nc * sizeof(UidLabel));
        if (!tmp) return;
        *arr = tmp; *cap = nc;
    }
    snprintf((*arr)[*count].uid, sizeof((*arr)[*count].uid), "%s", uid);
    (*arr)[*count].labels = strdup(lbl);
    (*count)++;
}

static UidLabel *ul_load(const char *path, int *count_out) {
    *count_out = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    int cap = 64;
    UidLabel *arr = malloc((size_t)cap * sizeof(UidLabel));
    if (!arr) { fclose(fp); return NULL; }
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        char *lbl = tab + 1;
        char *nl = strchr(lbl, '\n');
        if (nl) *nl = '\0';
        if (*count_out >= cap) {
            cap *= 2;
            UidLabel *tmp = realloc(arr, (size_t)cap * sizeof(UidLabel));
            if (!tmp) break;
            arr = tmp;
        }
        snprintf(arr[*count_out].uid, 17, "%.16s", line);
        arr[*count_out].labels = strdup(lbl);
        (*count_out)++;
    }
    fclose(fp);
    return arr;
}

static int ul_save(const char *path, const UidLabel *arr, int count) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    for (int i = 0; i < count; i++)
        fprintf(fp, "%s\t%s\n", arr[i].uid, arr[i].labels ? arr[i].labels : "");
    fclose(fp);
    return 0;
}

/* Return 1 if label s is present in comma-separated labels_csv */
static int csv_has_label(const char *csv, const char *s) {
    if (!csv || !s || !*s) return 0;
    size_t slen = strlen(s);
    const char *p = csv;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == slen && strncasecmp(p, s, slen) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* Build updated labels CSV: existing + add - rm */
static char *csv_update_labels(const char *existing,
                                char **add, int add_n,
                                char **rm,  int rm_n) {
    char buf[4096] = "";
    /* Keep existing labels that are not in rm */
    if (existing && existing[0]) {
        char *copy = strdup(existing);
        char *tok = copy, *s;
        while (tok && *tok) {
            s = strchr(tok, ',');
            if (s) *s = '\0';
            int do_rm = 0;
            for (int i = 0; i < rm_n; i++)
                if (rm[i] && strcasecmp(tok, rm[i]) == 0) { do_rm = 1; break; }
            if (!do_rm) {
                if (buf[0]) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
                strncat(buf, tok, sizeof(buf) - strlen(buf) - 1);
            }
            tok = s ? s + 1 : NULL;
        }
        free(copy);
    }
    /* Append add labels (skip duplicates) */
    for (int i = 0; i < add_n; i++) {
        if (!add[i] || !add[i][0]) continue;
        if (!csv_has_label(buf, add[i])) {
            if (buf[0]) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, add[i], sizeof(buf) - strlen(buf) - 1);
        }
    }
    return strdup(buf);
}

/* ── apply_rules: print rule match lines ─────────────────────────────── */
static void print_rule_matches(const MailRules *rules,
                                const char *from, const char *subject,
                                const char *to, const char *labels,
                                const char *uid, int dry_run) {
    for (int r = 0; r < rules->count; r++) {
        if (!mail_rule_matches(&rules->rules[r], from, subject, to, labels,
                               NULL, (time_t)0))
            continue;
        const MailRule *mr = &rules->rules[r];
        printf("  %s \"%s\" \xe2\x86\x92 uid:%s",
               dry_run ? "[dry-run]" : "[rule]",
               mr->name ? mr->name : "?", uid);
        for (int j = 0; j < mr->then_add_count; j++)
            printf("  +%s", mr->then_add_label[j]);
        for (int j = 0; j < mr->then_rm_count; j++)
            printf("  -%s", mr->then_rm_label[j]);
        if (mr->then_move_folder)
            printf("  \xe2\x86\x92%s", mr->then_move_folder);
        printf("\n");
    }
}

int email_service_apply_rules(const char *only_account, int dry_run, int verbose) {
    int count = 0;
    AccountEntry *accounts = config_list_accounts(&count);
    if (!accounts || count == 0) {
        fprintf(stderr, "No accounts configured.\n");
        config_free_account_list(accounts, count);
        return -1;
    }

    int errors = 0, done = 0;
    int total_fired = 0;
    for (int i = 0; i < count; i++) {
        if (only_account && only_account[0] &&
            strcmp(accounts[i].name, only_account) != 0)
            continue;

        printf("=== %s rules: %s ===\n",
               dry_run ? "Dry-run" : "Applying", accounts[i].name);
        local_store_init(accounts[i].cfg->host, accounts[i].cfg->user);

        MailRules *rules = mail_rules_load(accounts[i].name);
        if (!rules || rules->count == 0) {
            printf("  No rules found for %s.\n", accounts[i].name);
            mail_rules_free(rules);
            done++;
            continue;
        }

        int fired_total = 0;

        if (accounts[i].cfg->gmail_mode) {
            /* ── Gmail path: .hdr files are tab-separated ── */
            char (*uids)[17] = NULL;
            int uid_count = 0;
            local_hdr_list_all_uids("", &uids, &uid_count);

            for (int u = 0; u < uid_count; u++) {
                const char *uid = uids[u];
                char *hdr = local_hdr_load("", uid);
                if (!hdr) continue;

                /* Parse: from\tsubject\tdate\tlabels\tflags */
                char *fields[5] = {NULL};
                char *p = hdr;
                for (int f = 0; f < 5; f++) {
                    fields[f] = p;
                    char *tab = strchr(p, '\t');
                    if (tab) { *tab = '\0'; p = tab + 1; }
                    else     { p += strlen(p); }
                }
                for (int f = 0; f < 5; f++) {
                    if (fields[f]) {
                        char *nl = strchr(fields[f], '\n');
                        if (nl) *nl = '\0';
                    }
                }

                char **add_out = NULL; int add_count = 0;
                char **rm_out  = NULL; int rm_count  = 0;
                int fired = mail_rules_apply(rules,
                                              fields[0], fields[1], NULL, fields[3],
                                              NULL, (time_t)0,
                                              &add_out, &add_count,
                                              &rm_out,  &rm_count);
                if (fired > 0) {
                    /* Idempotency: skip if all changes already applied */
                    int has_new = 0;
                    for (int j = 0; j < add_count && !has_new; j++)
                        if (!csv_has_label(fields[3], add_out[j])) has_new = 1;
                    for (int j = 0; j < rm_count && !has_new; j++)
                        if (csv_has_label(fields[3], rm_out[j])) has_new = 1;

                    if (has_new) {
                        if (verbose || dry_run)
                            print_rule_matches(rules, fields[0], fields[1],
                                               NULL, fields[3], uid, dry_run);
                        fired_total++;
                        if (!dry_run) {
                            local_hdr_update_labels("", uid,
                                                     (const char **)add_out, add_count,
                                                     (const char **)rm_out,  rm_count);
                            for (int j = 0; j < add_count; j++) label_idx_add(add_out[j], uid);
                            for (int j = 0; j < rm_count;  j++) label_idx_remove(rm_out[j], uid);
                        }
                    }
                    for (int j = 0; j < add_count; j++) free(add_out[j]);
                    for (int j = 0; j < rm_count;  j++) free(rm_out[j]);
                    free(add_out); free(rm_out);
                }
                free(hdr);
            }
            free(uids);

        } else {
            /* ── IMAP path: use manifest + per-account applied_labels.tsv ── */
            const char *imap_folder = (accounts[i].cfg->folder && accounts[i].cfg->folder[0])
                                      ? accounts[i].cfg->folder : "INBOX";

            /* Path to applied labels persistence file */
            const char *data_dir = platform_data_dir();
            char lpath[8192] = "";
            if (data_dir && accounts[i].cfg->user && accounts[i].cfg->user[0])
                snprintf(lpath, sizeof(lpath), "%s/email-cli/accounts/%s/applied_labels.tsv",
                         data_dir, accounts[i].cfg->user);

            /* Load existing custom labels */
            int ul_count = 0, ul_cap = 0;
            UidLabel *ul_arr = NULL;
            if (lpath[0]) ul_arr = ul_load(lpath, &ul_count);
            int ul_dirty = 0;

            /* Iterate over manifest entries (decoded from/subject + flags) */
            Manifest *mf = manifest_load(imap_folder);
            if (!mf || mf->count == 0) {
                printf("  No messages found in folder %s.\n", imap_folder);
                manifest_free(mf);
                if (ul_arr) ul_free_all(ul_arr, ul_count);
                mail_rules_free(rules);
                done++;
                continue;
            }

            int mf_dirty = 0;
            /* Standard label→flag mapping */
            static const struct { const char *lbl; int flag; } lmap[] = {
                { "_junk",     MSG_FLAG_JUNK     },
                { "_spam",     MSG_FLAG_JUNK     },
                { "_phishing", MSG_FLAG_PHISHING },
                { "_done",     MSG_FLAG_DONE     },
                { "_flagged",  MSG_FLAG_FLAGGED  },
            };
            const int lmap_n = (int)(sizeof(lmap) / sizeof(lmap[0]));

            /* Label→IMAP flag mapping for pending_flags queue */
            static const struct {
                const char *lbl;
                const char *imap_flag;
                int add;
            } fmap[] = {
                { "UNREAD",    "\\Seen",     0 },
                { "_flagged",  "\\Flagged",  1 },
                { "_junk",     "$Junk",      1 },
                { "_spam",     "$Junk",      1 },
                { "_done",     "$Done",      1 },
                { "_trash",    "\\Deleted",  1 },
            };
            const int fmap_n = (int)(sizeof(fmap) / sizeof(fmap[0]));

            char *move_folder = NULL;

            for (int u = 0; u < mf->count; u++) {
                ManifestEntry *e = &mf->entries[u];

                /* Existing labels: custom (from persistence file) */
                const char *existing = ul_arr ? ul_get(ul_arr, ul_count, e->uid) : NULL;

                char **add_out = NULL; int add_count = 0;
                char **rm_out  = NULL; int rm_count  = 0;
                int fired = mail_rules_apply_ex(rules,
                                                e->from    ? e->from    : "",
                                                e->subject ? e->subject : "",
                                                NULL, existing,
                                                NULL, (time_t)0,
                                                &add_out, &add_count,
                                                &rm_out,  &rm_count,
                                                &move_folder);
                if (fired > 0) {
                    /* Check if any new custom labels would be added/removed */
                    int has_new = 0;
                    for (int j = 0; j < add_count && !has_new; j++)
                        if (!csv_has_label(existing, add_out[j])) has_new = 1;
                    for (int j = 0; j < rm_count && !has_new; j++)
                        if (csv_has_label(existing, rm_out[j])) has_new = 1;

                    /* Also check if any standard flag labels would change */
                    int new_flags = e->flags;
                    for (int j = 0; j < add_count; j++)
                        for (int k = 0; k < lmap_n; k++)
                            if (strcasecmp(add_out[j], lmap[k].lbl) == 0)
                                new_flags |= lmap[k].flag;
                    for (int j = 0; j < rm_count; j++)
                        for (int k = 0; k < lmap_n; k++)
                            if (strcasecmp(rm_out[j], lmap[k].lbl) == 0)
                                new_flags &= ~lmap[k].flag;
                    if (new_flags != e->flags) has_new = 1;

                    /* Also fire if a folder move is requested */
                    if (move_folder) has_new = 1;

                    if (has_new) {
                        if (verbose || dry_run)
                            print_rule_matches(rules,
                                               e->from    ? e->from    : "",
                                               e->subject ? e->subject : "",
                                               NULL, existing, e->uid, dry_run);
                        fired_total++;
                        if (!dry_run) {
                            /* Persist custom labels */
                            char *new_lbl = csv_update_labels(existing,
                                                               add_out, add_count,
                                                               rm_out,  rm_count);
                            ul_set(&ul_arr, &ul_count, &ul_cap, e->uid,
                                   new_lbl ? new_lbl : "");
                            free(new_lbl);
                            ul_dirty = 1;

                            /* Update manifest flags for standard labels */
                            if (new_flags != e->flags) {
                                e->flags = new_flags;
                                local_hdr_update_flags(imap_folder, e->uid, new_flags);
                                mf_dirty = 1;
                            }

                            /* Queue pending IMAP operations for server push */
                            for (int j = 0; j < add_count; j++) {
                                if (strcmp(add_out[j], "UNREAD") == 0)
                                    local_pending_flag_add(imap_folder, e->uid, "\\Seen", 0);
                                for (int k = 1; k < fmap_n; k++)
                                    if (strcasecmp(add_out[j], fmap[k].lbl) == 0)
                                        local_pending_flag_add(imap_folder, e->uid,
                                                               fmap[k].imap_flag, 1);
                            }
                            for (int j = 0; j < rm_count; j++) {
                                if (strcmp(rm_out[j], "UNREAD") == 0)
                                    local_pending_flag_add(imap_folder, e->uid, "\\Seen", 1);
                                if (strcasecmp(rm_out[j], "_flagged") == 0)
                                    local_pending_flag_add(imap_folder, e->uid, "\\Flagged", 0);
                            }
                            if (move_folder)
                                local_pending_move_add(imap_folder, e->uid, move_folder);
                        }
                    }
                    for (int j = 0; j < add_count; j++) free(add_out[j]);
                    for (int j = 0; j < rm_count;  j++) free(rm_out[j]);
                    free(add_out); free(rm_out);
                }
                free(move_folder); move_folder = NULL;
            }

            if (!dry_run && mf_dirty) manifest_save(imap_folder, mf);
            if (!dry_run && ul_dirty && lpath[0]) ul_save(lpath, ul_arr, ul_count);
            manifest_free(mf);
            if (ul_arr) ul_free_all(ul_arr, ul_count);
        }

        mail_rules_free(rules);
        if (dry_run)
            printf("  Rules dry-run: %d message(s) would be modified.\n", fired_total);
        else
            printf("  Rules applied: %d message(s) modified.\n", fired_total);
        total_fired += fired_total;
        done++;
    }
    config_free_account_list(accounts, count);

    if (done == 0) {
        fprintf(stderr, "Account '%s' not found.\n",
                only_account ? only_account : "");
        return -1;
    }
    return errors > 0 ? -1 : total_fired;
}

int email_service_rebuild_contacts(const char *only_account) {
    int count = 0;
    AccountEntry *accounts = config_list_accounts(&count);
    if (!accounts || count == 0) {
        fprintf(stderr, "No accounts configured.\n");
        config_free_account_list(accounts, count);
        return -1;
    }

    int done = 0;
    for (int i = 0; i < count; i++) {
        if (only_account && only_account[0] &&
            strcmp(accounts[i].name, only_account) != 0)
            continue;
        printf("=== Rebuilding contacts: %s ===\n", accounts[i].name);
        local_store_init(accounts[i].cfg->host, accounts[i].cfg->user);
        local_contacts_rebuild();
        done++;
    }
    config_free_account_list(accounts, count);

    if (done == 0) {
        fprintf(stderr, "Account '%s' not found.\n",
                only_account ? only_account : "");
        return -1;
    }
    return 0;
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
    if (!cfg->gmail_mode) {
        fprintf(stderr, "Error: 'list-labels' is Gmail-only. Use 'list-folders' for IMAP.\n");
        return -1;
    }
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
        if (rc == 0) printf("No labels found.\n");
        /* free any partial results */
        for (int i = 0; i < count; i++) {
            if (names) free(names[i]);
            if (ids)   free(ids[i]);
        }
        free(names);
        free(ids);
        return rc;
    }

    printf("%-30s  %s\n", "Label", "ID");
    printf("%-30s  %s\n", "------------------------------",
           "------------------------------");
    for (int i = 0; i < count; i++) {
        printf("%-30s  %s\n",
               names[i] ? names[i] : "",
               ids[i]   ? ids[i]   : "");
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
    if (!cfg->gmail_mode) {
        fprintf(stderr, "Error: 'create-label' is Gmail-only. Use 'create-folder' for IMAP.\n");
        return -1;
    }
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
    if (!cfg->gmail_mode) {
        fprintf(stderr, "Error: 'delete-label' is Gmail-only. Use 'delete-folder' for IMAP.\n");
        return -1;
    }
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

int email_service_mark_junk(const Config *cfg, const char *uid) {
    local_store_init(cfg->host, cfg->user);
    MailClient *mc = make_mail(cfg);
    if (!mc) { fprintf(stderr, "Error: Could not connect.\n"); return -1; }
    int rc = mail_client_mark_junk(mc, uid);
    mail_client_free(mc);
    if (rc == 0) printf("Message %s marked as junk.\n", uid);
    return rc;
}

int email_service_mark_notjunk(const Config *cfg, const char *uid) {
    local_store_init(cfg->host, cfg->user);
    MailClient *mc = make_mail(cfg);
    if (!mc) { fprintf(stderr, "Error: Could not connect.\n"); return -1; }
    int rc = mail_client_mark_notjunk(mc, uid);
    mail_client_free(mc);
    if (rc == 0) printf("Message %s marked as not-junk.\n", uid);
    return rc;
}

int email_service_create_folder(const Config *cfg, const char *name) {
    if (cfg->gmail_mode) {
        fprintf(stderr, "Error: 'create-folder' is IMAP-only. Use 'create-label' for Gmail.\n");
        return -1;
    }
    MailClient *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect.\n");
        return -1;
    }
    int rc = mail_client_create_folder(mc, name);
    mail_client_free(mc);

    if (rc == 0)
        printf("Folder '%s' created.\n", name);
    return rc;
}

int email_service_delete_folder(const Config *cfg, const char *name) {
    if (cfg->gmail_mode) {
        fprintf(stderr, "Error: 'delete-folder' is IMAP-only. Use 'delete-label' for Gmail.\n");
        return -1;
    }
    MailClient *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect.\n");
        return -1;
    }
    int rc = mail_client_delete_folder(mc, name);
    mail_client_free(mc);

    if (rc == 0)
        printf("Folder '%s' deleted.\n", name);
    return rc;
}

int email_service_save_sent(const Config *cfg, const char *msg, size_t msg_len) {
    local_store_init(cfg->host, cfg->user);
    const char *folder = cfg->sent_folder ? cfg->sent_folder : "Sent";
    return local_save_outgoing(folder, msg, msg_len);
}

int email_service_save_draft(const Config *cfg, const char *msg, size_t msg_len) {
    local_store_init(cfg->host, cfg->user);
    return local_save_outgoing("Drafts", msg, msg_len);
}
