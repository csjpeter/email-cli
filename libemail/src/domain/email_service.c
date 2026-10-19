/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-24 */
/* The mail service: listing, reading, syncing and applying rules through the mail client. */
#include "email_service.h"
#include "config_store.h"
#include "input_line.h"
#include "tui_frame.h"
#include "key_table.h"
#include "status_line.h"
#include <tuikit/text.h>
#include <tuikit/dialog.h>
#include "path_complete.h"
#include "mail_account.h"
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
static void warn_charset(const MimeTextInfo *info, const char *uid);
static void show_label_picker(emc_client *mc, const char *uid,
                               char *feedback_out, int feedback_cap);
static int is_system_or_special_label(const char *name);
static void flag_push_background(const Config *cfg, const char *uid,
                                  const char *flag_name, int add_flag);
static void junk_push_background(const Config *cfg, const char *uid, int mark_junk);

/**
 * Pager prompt for the standalone `show` command.
 * Returns scroll delta: 0 = quit, positive = forward N lines, negative = back N.
 */
static int pager_prompt(int cur_page, int total_pages, int page_size,
                        int term_rows, int sb_width) {
    for (;;) {
        tui_statusline_hint h[4];
        int nh = 0;
        #define HINT(k, l, p) do { h[nh].key = (k); h[nh].label = (l); h[nh].priority = (p); nh++; } while (0)
        HINT("PgDn/\u2193", "scroll", 60);
        HINT("PgUp/\u2191", "back", 60);
        HINT("ESC", "quit", SL_PRIO_KEEP);
#undef HINT
        status_line_draw(term_rows, sb_width, h, nh, NULL, cur_page, total_pages, NULL);
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
 * Show @p text on the status line (the last row), replacing the hints, as a
 * prompt or a message (EMAIL-98: one line).  Empty text draws an empty bar.
 */
static void print_infoline(int trows, int width, const char *text) {
    status_line_draw(trows, width, NULL, 0, NULL, 0, 0, text);
}

/** Copies @p src into @p dst (size @p n), cutting at a character boundary. */
static void cut_utf8(char *dst, size_t n, const char *src, size_t max) {
    size_t len = strlen(src);
    if (len > max) {
        len = max;
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/**
 * The status line of a message list: the key hints of the list (Gmail or
 * IMAP, Trash or not), the position [cur/total] and an optional message that
 * replaces them until the next key.  @p items is 0 for an empty list (no
 * marking, no bulk menu).
 */
static void list_status_line(int trows, int tcols, int is_gmail, int in_trash,
                             int items, int cur, int total, const char *msg) {
    tui_statusline_hint h[STATUS_LINE_MAX_HINTS];
    int nh = 0;
#define HINT(k, l, p) do { h[nh].key = (k); h[nh].label = (l); h[nh].priority = (p); nh++; } while (0)
    HINT("j/k", "step", 60);
    HINT("PgDn/PgUp", "page", 40);
    HINT("Enter", "open", 90);
    HINT("Esc/q/BS", is_gmail ? "labels" : "folders", SL_PRIO_KEEP);
    HINT(":", "actions", 80);
    HINT("c", "compose", 50);
    HINT("r", "reply", 50);
    HINT("F", "fwd", 30);
    HINT("A", "r-all", 30);
    HINT("*", "star", 45);
    HINT("+/-", "read", 45);
    HINT("!", "junk", 45);
    HINT("d", in_trash ? "del" : "trash", 55);
    if (is_gmail) HINT("t", "labels", 35);
    HINT("U", "refresh", 35);
    HINT("l", "rules", 30);
    if (items) {
        HINT("Space", "mark", 35);
    }
#undef HINT
    status_line_draw(trows, tcols, h, nh, "?", cur, total, msg);
}

/**
 * Show a help popup over the view and wait until it is closed.
 *
 * A modal of libtuikit (tui_dialog_help, EMAIL-110): two columns, the keys in
 * bold, the title naming the view; Esc, q and Backspace close it, the
 * movement keys scroll it when it is longer than the screen, any other key
 * changes nothing (policies/tui-interface-model.md, "The help screen").
 *
 * @param title  Title displayed in the popup header.
 * @param rows   Array of {key_label, description} string pairs.
 * @param n      Number of rows.
 */
static void show_help_popup(const char *title,
                            const char *rows[][2], int n) {
    tui_dialog d;
    if (tui_dialog_help(&d, title, &rows[0][0], n) != 0) return;
    tui_frame_run_dialog(&d);
    tui_dialog_free(&d);
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

static emc_client *make_mail(const Config *cfg) {
    return mail_connect(cfg);
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
    EMC_AUTO_CLOSE emc_client *mc = make_mail(cfg);
    if (!mc) return NULL;
    if (emc_select(mc, folder) != 0) return NULL;
    return headers_only ? emc_fetch_headers(mc, uid)
                        : emc_fetch_body(mc, uid);
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

/* Model of the attachment picker's table: name, content type, size. */
typedef struct { const MimeAttachment *atts; } AttachModel;

static void attach_cell(void *ctx, int i, int col, tui_table_cell *c) {
    const AttachModel *m = ctx;
    const MimeAttachment *a = &m->atts[i];
    char buf[256];
    if (col == 0) {
        tui_table_cell_put(c, a->filename ? a->filename : "(no name)", TUI_STYLE_DEFAULT);
    } else if (col == 1) {
        tui_table_cell_put(c, a->content_type ? a->content_type : "", TUI_STYLE_DEFAULT);
    } else {
        if (a->size >= 1024 * 1024)
            snprintf(buf, sizeof(buf), "%.1f MB", (double)a->size / (1024.0 * 1024.0));
        else if (a->size >= 1024)
            snprintf(buf, sizeof(buf), "%.0f KB", (double)a->size / 1024.0);
        else
            snprintf(buf, sizeof(buf), "%zu B", a->size);
        tui_table_cell_put(c, buf, TUI_STYLE_DEFAULT);
    }
}

/**
 * Like fetch_uid_headers_cached but uses an already-connected and folder-selected
 * emc_client instead of opening a new connection.  Falls back to the cache first.
 * Caller must free the returned string.
 */
static char *fetch_uid_headers_via(emc_client *mc, const char *folder, const char *uid) {
    if (local_hdr_exists(folder, uid))
        return local_hdr_load(folder, uid);
    char *hdrs = emc_fetch_headers(mc, uid);
    if (hdrs)
        local_hdr_save(folder, uid, hdrs, strlen(hdrs));
    return hdrs;
}

/* ── Show helpers ────────────────────────────────────────────────────── */

/* Header bar: bold white on blue (policies/tui-interface-model.md, Palette). */
#define TUI_HDR_ON "\033[1;37;44m"

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

static void print_show_headers(const char *from, const char *to,
                                const char *subject,
                                const char *date, const char *uid,
                                const char *labels,
                                const char *file_path,
                                const char *dmarc_desc,
                                const char *attachments) {
    /* label = 9 chars ("From:    "), remaining = SHOW_WIDTH - 9 = 71 */
    printf("From:    "); print_clean(from,    "(none)", SHOW_WIDTH - 9); putchar('\n');
    if (to && to[0]) {
        printf("To:      "); print_clean(to, "", SHOW_WIDTH - 9); putchar('\n');
    }
    printf("Subject: "); print_clean(subject, "(none)", SHOW_WIDTH - 9); putchar('\n');
    printf("Date:    "); print_clean(date,    "(none)", SHOW_WIDTH - 9); putchar('\n');
    printf("UID:     %s\n", uid ? uid : "(none)");
    if (labels && labels[0])
        printf("Labels:  %s\n", labels);
    if (file_path && file_path[0])
        printf("File:    %s\n", file_path);
    if (dmarc_desc && dmarc_desc[0]) {
        printf("DMARC:   "); print_clean(dmarc_desc, "", SHOW_WIDTH - 9); putchar('\n');
    }
    if (attachments && attachments[0]) {
        printf("Attach:  "); print_clean(attachments, "", SHOW_WIDTH - 9); putchar('\n');
    }
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
 * Returns selected index (0-based), -1 for Backspace (back), -2 for Esc/q
 * (back), -3 for ctrl-c (quit the program). */
static int show_attachment_picker(const MimeAttachment *atts, int count,
                                  int tcols, int trows) {
    int cursor = 0;
    for (;;) {
        AttachModel am = { atts };
        static const tui_table_col acols[] = {
            { "Name", 20, 3, TUI_ALIGN_LEFT,  0, 0, 0 },
            { "Type", 12, 2, TUI_ALIGN_LEFT,  0, 0, 0 },
            { "Size",  8, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
        };
        tui_table_model amod = { NULL, attach_cell, &am };
        tui_table atbl;
        tui_table_init(&atbl, acols, 3, &amod);
        atbl.flags |= TUI_TABLE_NO_HEADER;
        tui_table_set_count(&atbl, count);
        tui_table_select(&atbl, cursor);
        char title[64];
        snprintf(title, sizeof(title), "  Attachments (%d):", count);
        tui_frame_draw_table(title, &atbl, trows, tcols, 0, NULL, NULL);
        {
            tui_statusline_hint h[3];
            int nh = 0;
            #define HINT(k, l, p) do { h[nh].key = (k); h[nh].label = (l); h[nh].priority = (p); nh++; } while (0)
            HINT("\u2191\u2193", "select", 60);
            HINT("Enter", "choose", 90);
            HINT("Esc/q/BS", "back", SL_PRIO_KEEP);
#undef HINT
            status_line_draw(trows, tcols, h, nh, NULL, cursor + 1, count, NULL);
        }

        TermKey key = terminal_read_key();
        int ch = key == TERM_KEY_IGNORE ? terminal_last_printable() : 0;
        int page = trows > 6 ? trows - 4 : 1;
        if (ch == 'q') return -2;
        if (ch == 'j') key = TERM_KEY_NEXT_LINE;
        else if (ch == 'k') key = TERM_KEY_PREV_LINE;
        else if (ch == 'g') key = TERM_KEY_HOME;
        else if (ch == 'G') key = TERM_KEY_END;
        else if (ch == '?') {
            ekeys_help(EKV_ATTACH, "Attachment picker shortcuts");
            continue;
        }
        switch (key) {
        case TERM_KEY_BACK:    return -1;
        case TERM_KEY_ESC:     return -2;
        case TERM_KEY_QUIT:    return -3;
        case TERM_KEY_ENTER:   return cursor;
        case TERM_KEY_NEXT_LINE:
            if (cursor < count - 1) cursor++;
            break;
        case TERM_KEY_PREV_LINE:
            if (cursor > 0) cursor--;
            break;
        case TERM_KEY_NEXT_PAGE:
            cursor = cursor + page < count ? cursor + page : count - 1;
            break;
        case TERM_KEY_PREV_PAGE:
            cursor = cursor - page > 0 ? cursor - page : 0;
            break;
        case TERM_KEY_HOME:    cursor = 0; break;
        case TERM_KEY_END:     cursor = count - 1; break;
        default: break;
        }
    }
}

/**
 * Show a message in interactive pager mode.
 * Returns 0 = back to list (Backspace/ESC/q), 2 = reply, 5 = forward, -1 = error.
 * mc may be NULL (operations then queue for background sync).
 * initial_flags: caller-supplied MSG_FLAG_* bitmask (used for IMAP where .hdr
 *   does not carry a flags field).
 * flags_out: if non-NULL, receives the final flag state on exit.
 */
/** IMAP trash / permanent delete of @p uid in @p folder from the TUI.
 *  With a connection the server is told at once; without one (offline
 *  mode) a move to the trash is queued as a pending move for the next sync,
 *  and a permanent delete, which cannot be queued, is refused.
 *  Fills @p msg with the feedback.  Returns 0 when the message is done or
 *  queued (the caller may drop it locally), -1 when nothing was done. */
static int imap_trash_or_queue(emc_client *mc, const Config *cfg,
                               const char *folder, const char *uid,
                               char *msg, size_t msg_sz) {
    const char *trf = cfg->trash_folder ? cfg->trash_folder : "Trash";
    if (strcmp(folder, trf) == 0) {
        if (!mc) {
            snprintf(msg, msg_sz, "No connection: not deleted");
            return -1;
        }
        if (emc_delete(mc, uid) != 0) {
            snprintf(msg, msg_sz, "Could not delete the message");
            return -1;
        }
        snprintf(msg, msg_sz, "Permanently deleted");
        return 0;
    }
    if (mc && emc_move_to_folder(mc, uid, trf) == 0) {
        snprintf(msg, msg_sz, "Moved to Trash");
        return 0;
    }
    if (local_pending_move_add(folder, uid, trf) != 0) {
        snprintf(msg, msg_sz, "Could not move to Trash");
        return -1;
    }
    snprintf(msg, msg_sz, "Moved to Trash (queued, no connection)");
    return 0;
}

/** Gmail without a connection in the TUI: queue the removal of every real
 *  label the message carries (the same set the online archive removes) for
 *  the next sync.  Must run before the .hdr labels are rewritten. */
static void gmail_queue_label_removals(const char *uid) {
    char *labels = local_hdr_get_labels("", uid);
    if (!labels) return;
    char *tok = labels, *sep;
    while (tok && *tok) {
        sep = strchr(tok, ',');
        if (sep) *sep = '\0';
        if (tok[0] && tok[0] != '_' && strcmp(tok, "IMPORTANT") != 0 &&
            strncmp(tok, "CATEGORY_", 9) != 0)
            local_pending_label_add(uid, tok, 0);
        tok = sep ? sep + 1 : NULL;
    }
    free(labels);
}

/** Drops one queued label change of @p uid; returns 1 when it was queued.
 *  Used by undo, so a trash that never reached the server is cancelled
 *  instead of followed by its opposite in the same push. */
static int gmail_unqueue_label(const char *uid, const char *label, int add) {
    int n = 0, found = 0;
    PendingLabel *q = local_pending_label_load(&n);
    if (!q || n == 0) { free(q); return 0; }
    local_pending_label_clear();
    for (int i = 0; i < n; i++) {
        if (!found && strcmp(q[i].uid, uid) == 0 && q[i].add == add &&
            strcmp(q[i].label, label) == 0) { found = 1; continue; }
        local_pending_label_add(q[i].uid, q[i].label, q[i].add);
    }
    free(q);
    return found;
}

/** Gmail TUI trash: with a connection the server is told at once, without
 *  one the trash is queued for the next sync. */
static void gmail_trash_or_queue(emc_client *mc, const char *uid) {
    if (mc && emc_trash(mc, uid) == 0) return;
    gmail_queue_label_removals(uid);
    local_pending_label_add(uid, "_trash", 1);
}

/** Gmail permanent delete: cannot be queued, so it needs a connection (the
 *  list's own, or a one-shot one).  Returns 0 when the server did it. */
static int gmail_delete_now(const Config *cfg, emc_client *mc, const char *uid) {
    emc_client *c = mc ? mc : mail_connect(cfg);
    if (!c) return -1;
    int rc = emc_delete(c, uid);
    if (!mc) emc_close(c);
    return rc;
}

static int show_uid_interactive(const Config *cfg, emc_client *mc,
                                const char *folder,
                                const char *uid, int page_size,
                                int initial_flags, int *flags_out) {
    char *raw = NULL;
    /* For Gmail (and virtual flag views) the .eml is stored under the empty
     * folder regardless of which label manifest the entry came from. */
    const char *load_folder = folder;
    if (!local_msg_exists(folder, uid) && cfg->gmail_mode
            && local_msg_exists("", uid))
        load_folder = "";
    if (local_msg_exists(load_folder, uid)) {
        raw = local_msg_load(load_folder, uid);
    } else if (mc) {
        if (emc_select(mc, folder) == 0)
            raw = emc_fetch_body(mc, uid);
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
    /* To: field for reader header */
    char *to_raw  = mime_get_header(raw, "To");
    char *show_to = to_raw ? mime_decode_words(to_raw) : NULL;
    free(to_raw);
    /* Local .eml file path and DMARC description for the reader header */
    char *show_path = local_msg_path(folder, uid);
    char *ar_hdr    = mime_get_header(raw, "Authentication-Results");
    char *show_dmarc = mime_describe_dmarc(ar_hdr);
    free(ar_hdr);
    /* Load current flags for the star / read / delete operations.
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
        MimeTextInfo tinfo;
        char *plain = mime_get_text_body_ex(raw, &tinfo);
        if (plain) {
            warn_charset(&tinfo, uid);
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
    /* Propagate attachment result back to caller via reader_flags */
    if (att_count > 0)
        reader_flags |= MSG_FLAG_ATTACH | MSG_FLAG_ATTACH_CHECKED;
    else
        reader_flags |= MSG_FLAG_ATTACH_CHECKED;
    /* Build single-line attachment summary for reader header */
    char attach_buf[256] = "";
    if (att_count > 0) {
        int pos = snprintf(attach_buf, sizeof(attach_buf),
                           "%d file%s: ", att_count, att_count == 1 ? "" : "s");
        for (int _i = 0; _i < att_count && pos < (int)sizeof(attach_buf) - 2; _i++) {
            const char *fn = atts[_i].filename ? atts[_i].filename : "?";
            if (_i > 0) { attach_buf[pos++] = ','; attach_buf[pos++] = ' '; attach_buf[pos] = '\0'; }
            if (pos + (int)strlen(fn) > SHOW_WIDTH - 12) {
                snprintf(attach_buf + pos, sizeof(attach_buf) - (size_t)pos,
                         "(+%d more)", att_count - _i);
                break;
            }
            size_t av = sizeof(attach_buf) - (size_t)pos - 1;
            strncpy(attach_buf + pos, fn, av);
            attach_buf[sizeof(attach_buf) - 1] = '\0';
            pos += (int)strlen(fn);
        }
    }

/* Header: From+Subject+Date+UID+separator = 5 base rows; optional To/Labels/File/DMARC/Attach.
 * Footer: info line (trows-1) + statusbar (trows) = 2 rows. */
#define SHOW_HDR_LINES_INT 8  /* kept for #undef below */
    int hdr_rows = 5;  /* From + Subject + Date + UID + separator */
    if (show_to    && show_to[0])    hdr_rows++;
    if (show_labels && show_labels[0]) hdr_rows++;
    if (show_path  && show_path[0])  hdr_rows++;
    if (show_dmarc && show_dmarc[0]) hdr_rows++;
    if (att_count > 0)               hdr_rows++;
    int rows_avail  = (term_rows > hdr_rows + 2) ? term_rows - hdr_rows - 2 : 1;
    int view_raw    = 0; /* 0=rendered, 1=raw source */
    int body_vrows  = count_visual_rows(body_text, term_cols);
    int total_pages = (body_vrows + rows_avail - 1) / rows_avail;
    if (total_pages < 1) total_pages = 1;

    char search_buf[256] = ""; /* last search term; empty = none */

    /* Persistent info message — stays until replaced by a newer one */
    char info_msg[2048] = "";
    EmailAction deferred_act = EA_NONE;   /* a menu action that prompts runs after a redraw */

    int result = 0;
    for (int cur_line = 0;;) {
        /* A resize (EMAIL-97): take the new size and lay the message out
         * again; the position in the text is kept, within the new length. */
        {
            int nc = terminal_cols(), nr = terminal_rows();
            if (nc > 0 && nr > 0 && (nc != term_cols || nr != term_rows)) {
                int old_wrap = wrap_cols;
                term_cols = nc;
                term_rows = nr;
                wrap_cols = term_cols > SHOW_WIDTH ? SHOW_WIDTH : term_cols;
                rows_avail = (term_rows > hdr_rows + 2) ? term_rows - hdr_rows - 2 : 1;
                if (wrap_cols != old_wrap) {
                    char *nb = NULL;
                    char *hr = mime_get_html_part(raw);
                    if (hr) {
                        nb = html_render(hr, wrap_cols, 1);
                        free(hr);
                    } else {
                        MimeTextInfo ti;
                        char *pl = mime_get_text_body_ex(raw, &ti);
                        if (pl) {
                            char *w = word_wrap(pl, wrap_cols);
                            if (w) { free(pl); nb = w; }
                            else nb = pl;
                        }
                    }
                    free(body);
                    body = nb;
                }
            }
        }
        /* Recompute active body text and pagination for current view mode */
        body_text  = view_raw ? raw : (body ? body : "(no readable text body)");
        body_vrows = count_visual_rows(body_text, term_cols);
        if (cur_line >= body_vrows) cur_line = body_vrows > 0 ? body_vrows - 1 : 0;
        total_pages = (body_vrows + rows_avail - 1) / rows_avail;
        if (total_pages < 1) total_pages = 1;

        printf("\033[0m\033[H\033[2J");     /* reset attrs + clear screen */
        print_show_headers(from, show_to, subject, date, uid, show_labels, show_path, show_dmarc, attach_buf);
        print_body_page(body_text, cur_line, rows_avail, term_cols);
        printf("\033[0m");                  /* close any open ANSI from body */
        fflush(stdout);

        int cur_page = cur_line / rows_avail + 1;

        /* One status line: a message replaces the hints until the next key. */
        {
            tui_statusline_hint h[STATUS_LINE_MAX_HINTS];
            int nh = 0;
            char attlbl[32];
#define HINT(k, l, p) do { h[nh].key = (k); h[nh].label = (l); h[nh].priority = (p); nh++; } while (0)
            snprintf(attlbl, sizeof(attlbl), "save-all(%d)", att_count);
            HINT("j/k", "scroll", 60);
            HINT("Esc/q/BS", "list", SL_PRIO_KEEP);
            HINT(":", "actions", 80);
            HINT("r", "reply", 50);
            HINT("F", "fwd", 40);
            HINT("*", "star", 45);
            HINT("+/-", "read", 45);
            HINT("!", "junk", 45);
            HINT("d", "del", 55);
            if (cfg->gmail_mode) HINT("t", "labels", 35);
            if (att_count > 0) {
                snprintf(attlbl, sizeof(attlbl), "save(%d)", att_count);
                HINT("a", attlbl, 55);
            }
            HINT("/", "search", 55);
            HINT("v", view_raw ? "rendered" : "source", 35);
            status_line_draw(term_rows, term_cols, h, nh, "?", cur_page,
                             total_pages, info_msg);
        }

        EmailAction deferred_now = deferred_act;
        deferred_act = EA_NONE;
        TermKey key = deferred_now ? TERM_KEY_IGNORE : terminal_read_key();
        fprintf(stderr, "\r\033[K");
        fflush(stderr);
        /* The message lives until the next key. */
        if (key != TERM_KEY_IGNORE || terminal_last_printable())
            info_msg[0] = '\0';

        switch (key) {
        case TERM_KEY_BACK:
        case TERM_KEY_ESC:
            result = 0;          /* back to list (Esc, q and Backspace alike) */
            goto show_int_done;
        case TERM_KEY_QUIT:
            result = 1;          /* ctrl-c: leave the program at once */
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
            /* The keys that change state come from the key table, and so
             * does the ':' menu (EMAIL-175). */
            const char *rview = is_gmail ? EKV_READ_GMAIL : EKV_READ_IMAP;
            if (deferred_now) ch = 0;
            EmailAction act = deferred_now ? deferred_now : ekeys_find(rview, key, ch);
            char del_q[600] = "";
            {
                char fr[128], su[160];
                cut_utf8(fr, sizeof(fr), from ? from : "(no from)", 80);
                cut_utf8(su, sizeof(su), subject ? subject : "(no subject)", 120);
                int trash_here = is_gmail ? strcmp(folder, "_trash") == 0
                    : strcmp(folder, cfg->trash_folder ? cfg->trash_folder : "Trash") == 0;
                snprintf(del_q, sizeof(del_q), trash_here ? "Delete \"%s - %s\" for good"
                                                          : "Move \"%s - %s\" to Trash",
                         fr, su);
            }
            int menu_confirmed = deferred_now != EA_NONE;
            if (ch == ':') {
                const char *tt[EA_COUNT] = { NULL };
                tt[EA_DELETE] = del_q;
                act = ekeys_menu(rview, tt);
                menu_confirmed = 1;
                if (act == EA_SAVE_ALL) {
                    deferred_act = act;      /* draw the message, then ask */
                    break;
                }
            }
            if (ch == 'q') {
                result = 0;      /* back to list */
                goto show_int_done;
            } else if (ch == 'j') {
                if (cur_line < body_vrows - 1) cur_line++;
                break;
            } else if (ch == 'k') {
                if (cur_line > 0) cur_line--;
                break;
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
            } else if (ch == '?') {
                ekeys_help(rview, is_gmail ? "Message reader shortcuts (Gmail)"
                                           : "Message reader shortcuts");
                break;
            } else if (act == EA_RM_LABEL && folder[0] != '_') {
                /* Remove current label from this message */
                const char *lbl = folder;
                label_idx_remove(lbl, uid);
                local_hdr_update_labels("", uid, NULL, 0, &lbl, 1);
                if (mc) emc_modify_label(mc, uid, lbl, 0);
                snprintf(info_msg, sizeof(info_msg), "Label removed: %s", lbl);
                /* Reload show_labels to reflect the change */
                free(show_labels);
                show_labels = local_hdr_get_labels("", uid);
                break;
            } else if (act == EA_DELETE && is_gmail) {
                if (!menu_confirmed && !ekeys_confirm(rview, EA_DELETE, del_q)) {
                    snprintf(info_msg, sizeof(info_msg), "Not deleted");
                    break;
                }
                if (strcmp(folder, "_trash") == 0) {
                    /* In Trash: permanently delete (needs a connection) */
                    if (gmail_delete_now(cfg, mc, uid) != 0) {
                        snprintf(info_msg, sizeof(info_msg),
                                 "No connection: not deleted");
                        break;
                    }
                    label_idx_remove("_trash", uid);
                    result = 6;   /* signal list loop to mark entry removed */
                    goto show_int_done;
                }
                /* Trash: Gmail compound trash operation (queued offline) */
                gmail_trash_or_queue(mc, uid);
                char **all_labels = NULL; int all_count = 0;
                label_idx_list(&all_labels, &all_count);
                for (int j = 0; j < all_count; j++) {
                    label_idx_remove(all_labels[j], uid);
                    free(all_labels[j]);
                }
                free(all_labels);
                label_idx_add("_trash", uid);
                result = 6;   /* go back to list; entry will be marked removed */
                goto show_int_done;
            } else if (act == EA_ARCHIVE) {
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
                                if (strcmp(tok, "IMPORTANT") != 0 &&
                                    strncmp(tok, "CATEGORY_", 9) != 0) {
                                    if (mc) emc_modify_label(mc, uid, tok, 0);
                                    else local_pending_label_add(uid, tok, 0);
                                }
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
                if (mc) emc_set_flag(mc, uid, "\\Seen", 1);
                else local_pending_label_add(uid, "UNREAD", 0);
                label_idx_add("_nolabel", uid);
                free(show_labels);
                show_labels = local_hdr_get_labels("", uid);
                snprintf(info_msg, sizeof(info_msg), "Archived");
                break;
            } else if (act == EA_LABELS) {
                show_label_picker(mc, uid, info_msg, sizeof(info_msg));
                free(show_labels);
                show_labels = local_hdr_get_labels("", uid);
                break;
            } else if (act == EA_STAR) {
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
                    emc_set_flag(mc, uid, "\\Flagged", add_flag);
                }
                snprintf(info_msg, sizeof(info_msg),
                         currently ? "Unstarred" : "Starred");
                break;
            } else if (act == EA_READ || act == EA_UNREAD) {
                /* Mark read ('+') or unread ('-') */
                int currently = reader_flags & MSG_FLAG_UNSEEN;
                if ((act == EA_UNREAD) == (currently != 0)) {
                    snprintf(info_msg, sizeof(info_msg),
                             currently ? "Already unread" : "Already read");
                    break;
                }
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
                    emc_set_flag(mc, uid, "\\Seen", add_flag);
                }
                snprintf(info_msg, sizeof(info_msg),
                         currently ? "Marked as read" : "Marked as unread");
                break;
            } else if (act == EA_DONE) {
                /* IMAP only: toggle Done flag */
                int currently = reader_flags & MSG_FLAG_DONE;
                int add_flag  = currently ? 0 : 1;
                reader_flags ^= MSG_FLAG_DONE;
                /* .hdr flags field is not used for IMAP; skip local_hdr_update_flags */
                local_pending_flag_add(folder, uid, "$Done", add_flag);
                if (mc) emc_set_flag(mc, uid, "$Done", add_flag);
                snprintf(info_msg, sizeof(info_msg),
                         currently ? "Marked not done" : "Marked done");
                break;
            } else if (act == EA_JUNK) {
                int want = !(reader_flags & MSG_FLAG_JUNK);
                reader_flags ^= MSG_FLAG_JUNK;
                if (is_gmail) junk_push_background(cfg, uid, want);
                else if (mc) {
                    if (want) emc_mark_junk(mc, uid);
                    else      emc_mark_notjunk(mc, uid);
                }
                snprintf(info_msg, sizeof(info_msg),
                         want ? "Marked as junk" : "Marked as not-junk");
                break;
            } else if (act == EA_DELETE) {
                /* IMAP trash / permanent delete from reader */
                if (!menu_confirmed && !ekeys_confirm(rview, EA_DELETE, del_q)) {
                    snprintf(info_msg, sizeof(info_msg), "Not deleted");
                    break;
                }
                if (imap_trash_or_queue(mc, cfg, folder, uid,
                                        info_msg, sizeof(info_msg)) != 0)
                    break;    /* nothing done: stay in the reader */
                result = 6;   /* signal list loop to mark entry removed */
                goto show_int_done;
            } else if (ch == 'r') {
                /* Ensure the raw message is in the local cache so cmd_reply
                 * can reload it without requiring a live IMAP connection. */
                if (!local_msg_exists(folder, uid))
                    local_msg_save(folder, uid, raw, strlen(raw));
                result = 2;      /* reply to this message */
                goto show_int_done;
            } else if (ch == 'F') {
                /* Forward: cache the raw message so cmd_forward can read it
                 * without a live connection, then signal the list loop. */
                if (!local_msg_exists(folder, uid))
                    local_msg_save(folder, uid, raw, strlen(raw));
                result = 5;      /* forward this message */
                goto show_int_done;
            } else if (ch == 'A') {
                /* Reply-all: same key as in the list (EMAIL-155). */
                if (!local_msg_exists(folder, uid))
                    local_msg_save(folder, uid, raw, strlen(raw));
                result = 7;      /* reply to all of this message */
                goto show_int_done;
            } else if (att_count > 0 && ch == 'a') {
                int sel = 0;
                if (att_count > 1) {
                    sel = show_attachment_picker(atts, att_count,
                                                 term_cols, term_rows);
                    if (sel == -3) {
                        result = 1;  /* ctrl-c: leave the program at once */
                        goto show_int_done;
                    }
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
            } else if (ch == 'a') {
                snprintf(info_msg, sizeof(info_msg), "No attachments");
                break;
            } else if (act == EA_SAVE_ALL && att_count == 0) {
                snprintf(info_msg, sizeof(info_msg), "No attachments");
                break;
            } else if (act == EA_SAVE_ALL) {
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
    free(show_path); free(show_dmarc); free(show_to);
    free(body); free(body_wrapped); free(from); free(subject); free(date); free(show_labels); free(raw);
    if (flags_out) *flags_out = reader_flags;
    return result;
}

/* ── List helpers ────────────────────────────────────────────────────── */

typedef struct { char uid[17]; int flags; time_t epoch; char folder[256]; } MsgEntry;

/* Write s as a JSON string value, escaping what RFC 8259 requires.
 * Bytes are emitted as-is: the manifest already holds UTF-8. */
static void print_json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout);  break;
            case '\r': fputs("\\r", stdout);  break;
            case '\t': fputs("\\t", stdout);  break;
            default:
                if (*p < 0x20) printf("\\u%04x", *p);
                else           putchar((char)*p);
        }
    }
    putchar('"');
}

/* EMAIL-122: when set, the read commands print JSON instead of the table. */
static int g_json_out;

void email_service_set_json(int on) { g_json_out = on ? 1 : 0; }

void email_service_print_json_string(const char *s) { print_json_string(s); }

/* True when no batch filter is configured — the common case, kept cheap. */
/**
 * The ':' menu of a list with no rows: only a sync can be started, after a
 * confirmation that names the account and the folder (EMAIL-175).
 */
static int sync_start_background(void);
static void sync_menu(const Config *cfg, const char *folder_display) {
    char t[400];
    const char *tt[EA_COUNT] = { NULL };
    snprintf(t, sizeof(t), "%s (%s)", folder_display ? folder_display : "?",
             cfg->user ? cfg->user : "?");
    tt[EA_SYNC] = t;
    if (ekeys_menu(cfg->gmail_mode ? EKV_LIST_GMAIL : EKV_LIST_IMAP, tt) == EA_SYNC)
        sync_start_background();
}

static int list_filters_off(const EmailListOpts *o) {
    return !(o->filter_from   && o->filter_from[0])
        && !(o->filter_since  && o->filter_since[0])
        && !(o->filter_before && o->filter_before[0]);
}

/* Match one entry against --from / --since / --before.
 *
 * Dates are compared on the "YYYY-MM-DD" prefix of the manifest's
 * "YYYY-MM-DD HH:MM" field, which sorts correctly as text: --since is
 * inclusive, --before exclusive.  Entries with no manifest row (headers not
 * fetched yet) cannot be judged and are dropped, so a filtered list never
 * claims a match it did not verify. */
static int entry_matches_filters(const Manifest *m, const MsgEntry *e,
                                 const EmailListOpts *o) {
    if (list_filters_off(o)) return 1;
    const ManifestEntry *me = manifest_find(m, e->uid);
    if (!me) return 0;

    if (o->filter_from && o->filter_from[0] &&
        (!me->from || !strcasestr(me->from, o->filter_from)))
        return 0;
    if (o->filter_since && o->filter_since[0] &&
        (!me->date || strncmp(me->date, o->filter_since, 10) < 0))
        return 0;
    if (o->filter_before && o->filter_before[0] &&
        (!me->date || strncmp(me->date, o->filter_before, 10) >= 0))
        return 0;
    return 1;
}

/* Remove non-matching entries in place; returns the remaining count. */
static int apply_list_filters(const Manifest *m, MsgEntry *entries, int count,
                              const EmailListOpts *o) {
    if (list_filters_off(o)) return count;
    int kept = 0;
    for (int i = 0; i < count; i++)
        if (entry_matches_filters(m, &entries[i], o))
            entries[kept++] = entries[i];
    return kept;
}


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
        emc_client *mc = make_mail(cfg);
        if (mc) {
            emc_set_flag(mc, uid, flag_name, add_flag);
            emc_close(mc);
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
        emc_client *mc = make_mail(cfg);
        if (mc) {
            if (mark_junk) emc_mark_junk(mc, uid);
            else           emc_mark_notjunk(mc, uid);
            emc_close(mc);
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
 * prefix="" means root level (folders with no sep in their name).  Inside a
 * folder (prefix not empty) the folder itself is the first entry.
 * Returns number of visible entries written into vis_out[].
 */
static int build_flat_view(char **names, int count, char sep,
                           const char *prefix, int *vis_out) {
    int vcount = 0;
    size_t plen = strlen(prefix);
    /* Inside a folder the folder itself comes first, so that its own
     * messages can be opened (Enter on a folder with children goes into it). */
    for (int i = 0; plen > 0 && i < count; i++)
        if (strcmp(names[i], prefix) == 0) { vis_out[vcount++] = i; break; }
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

/* Model of the folder browser's table (tui_table): the rows are the virtual
 * prefix rows (two section headings and the six tag/flag views) followed by
 * the folders, as a tree or as one level of the flat view.  Columns: Unread,
 * Flagged, Folder, Total. */
enum { FB_HDR_FLAGS = 0, FB_UNREAD = 1, FB_FLAGGED = 2, FB_JUNK = 3, FB_PHISHING = 4,
       FB_ANSWERED = 5, FB_FORWARDED = 6, FB_HDR_FOLD = 7, FB_PREFIX = 8 };

typedef struct {
    char               **names;
    int                  count;
    char                 sep;
    const FolderStatus  *statuses;
    int                  tree;        /* 1 tree, 0 flat */
    const int           *vis;         /* flat: indexes into names[] */
    int                  vf[FB_PREFIX]; /* counts of the virtual rows */
    int                  selected;    /* the cursor row: never dimmed */
    const char          *prefix;      /* flat: the folder being looked into, or NULL */
} FolderModel;

/* Index into names[] of table row @p i (>= FB_PREFIX). */
static int fm_name_index(const FolderModel *m, int i) {
    return m->tree ? i - FB_PREFIX : m->vis[i - FB_PREFIX];
}

/* Totals of a real folder row: own counts, or the subtree's in the flat view
 * when the folder has children. */
static void fm_counts(const FolderModel *m, int ni, int *msgs, int *unseen,
                      int *flagged, int *has_kids) {
    *has_kids = 0;
    /* The folder being looked into shows its own counts, not its subtree's. */
    int is_prefix = !m->tree && m->prefix && m->prefix[0] &&
                    strcmp(m->names[ni], m->prefix) == 0;
    if (!m->tree && !is_prefix && folder_has_children(m->names, m->count, m->names[ni], m->sep)) {
        *has_kids = 1;
        sum_subtree(m->names, m->count, m->sep, m->names[ni], m->statuses,
                    msgs, unseen, flagged);
        return;
    }
    *msgs   = m->statuses ? m->statuses[ni].messages : 0;
    *unseen = m->statuses ? m->statuses[ni].unseen   : 0;
    *flagged= m->statuses ? m->statuses[ni].flagged  : 0;
}

static void folders_row(void *ctx, int i, tui_table_row *r) {
    const FolderModel *m = ctx;
    if (i == FB_HDR_FLAGS || i == FB_HDR_FOLD) {
        tui_table_row_kind(r, TUI_ROW_SECTION);
        tui_table_row_text(r, i == FB_HDR_FLAGS ? "Tags / Flags" : "Folders");
        tui_table_row_style(r, tui_style_attrs(TUI_ATTR_DIM));
        return;
    }
    if (i < FB_PREFIX) {
        tui_style st = TUI_STYLE_DEFAULT;
        if (m->vf[i] == 0) {
            if (i != m->selected) st = tui_style_attrs(TUI_ATTR_DIM);
        } else switch (i) {
            case FB_UNREAD:    st.fg = TUI_GREEN;  break;
            case FB_FLAGGED:
            case FB_JUNK:      st.fg = TUI_YELLOW; break;
            case FB_PHISHING:  st.fg = TUI_RED;    break;
            default:           st.fg = TUI_CYAN;   break;   /* answered, forwarded */
        }
        tui_table_row_style(r, st);
        return;
    }
    int msgs, unseen, flagged, kids;
    fm_counts(m, fm_name_index(m, i), &msgs, &unseen, &flagged, &kids);
    if (msgs == 0 && i != m->selected)
        tui_table_row_style(r, tui_style_attrs(TUI_ATTR_DIM));
}

static void folders_cell(void *ctx, int i, int col, tui_table_cell *c) {
    const FolderModel *m = ctx;
    char buf[512];
    if (i < FB_PREFIX) {
        static const char *const vname[FB_PREFIX] = {
            "", "Unread", "Flagged", "Junk", "Phishing", "Answered", "Forwarded", "" };
        switch (col) {
        case 0: fmt_thou(buf, sizeof(buf), m->vf[i]);
                if (m->vf[i] == 0) snprintf(buf, sizeof(buf), "0");
                break;
        case 1: snprintf(buf, sizeof(buf), "-"); break;
        case 2: snprintf(buf, sizeof(buf), "%s", vname[i]); break;
        default: snprintf(buf, sizeof(buf), "-"); break;
        }
        tui_table_cell_put(c, buf, TUI_STYLE_DEFAULT);
        return;
    }
    int ni = fm_name_index(m, i);
    int msgs, unseen, flagged, kids;
    fm_counts(m, ni, &msgs, &unseen, &flagged, &kids);
    const char *comp = strrchr(m->names[ni], m->sep);
    comp = comp ? comp + 1 : m->names[ni];
    switch (col) {
    case 0: fmt_thou(buf, sizeof(buf), unseen);  break;
    case 1: fmt_thou(buf, sizeof(buf), flagged); break;
    case 3: fmt_thou(buf, sizeof(buf), msgs);    break;
    default:
        if (m->tree) {
            /* Tree glyphs are a prefix that is never cut. */
            char pre[512];
            int pos = 0, depth = 0;
            for (const char *p = m->names[ni]; *p; p++)
                if (*p == m->sep) depth++;
            for (int lv = 0; lv < depth; lv++) {
                const char *br = ancestor_is_last(m->names, m->count, ni, lv, m->sep)
                                 ? "    " : "\u2502   ";
                int bl = (int)strlen(br);
                if (pos + bl < (int)sizeof(pre) - 1) { memcpy(pre + pos, br, (size_t)bl); pos += bl; }
            }
            const char *cn = is_last_sibling(m->names, m->count, ni, m->sep)
                             ? "\u2514\u2500\u2500 " : "\u251c\u2500\u2500 ";
            int cl = (int)strlen(cn);
            if (pos + cl < (int)sizeof(pre) - 1) { memcpy(pre + pos, cn, (size_t)cl); pos += cl; }
            pre[pos] = '\0';
            tui_table_cell_prefix(c, pre, TUI_STYLE_DEFAULT);
            tui_table_cell_put(c, comp, TUI_STYLE_DEFAULT);
        } else {
            snprintf(buf, sizeof(buf), "%s%s", comp, kids ? "/" : "");
            tui_table_cell_put(c, buf, TUI_STYLE_DEFAULT);
        }
        return;
    }
    tui_table_cell_put(c, buf, TUI_STYLE_DEFAULT);
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

/* TUI date scopes (EMAIL-128): same semantics as the CLI --since (inclusive)
 * and --before (exclusive).  Only a complete YYYY-MM-DD is a date; while the
 * text is incomplete the filter does not narrow the list. */
static int tui_date_text_valid(const char *s) {
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return 0; }
        else if (s[i] < '0' || s[i] > '9') return 0;
    }
    return s[10] == '\0';
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
    if (!fbuf || fbuf[0] == '\0' ||
        (fscope >= 4 && !tui_date_text_valid(fbuf))) {
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
        } else if (fscope == 4) {
            match = me && me->date && strncmp(me->date, fbuf, 10) >= 0;
        } else if (fscope == 5) {
            match = me && me->date && strncmp(me->date, fbuf, 10) < 0;
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

/* A virtual name is a view, not a mailbox: no message is stored under it. */
static int is_virtual_folder_name(const char *f) {
    return f && f[0] == '_' && f[1] == '_';
}

/* Nothing is cached for this view: say so, and in the TUI offer a sync.
 *
 * Shared by the IMAP manifest branch and the Gmail label-index branch — both
 * can legitimately find an empty cache, and the advice (run email-sync) is the
 * same either way; only the artifact they looked in differs.  Returns the same
 * codes as email_service_list(): 0 quit, 1 back, 4 refresh, 7 rules editor. */
static int list_empty_cache_view(const Config *cfg, const EmailListOpts *opts,
                                 const char *folder, const char *folder_display) {
    if (!opts->pager) {
        if (opts->json) {
            /* JSON mode must always emit one parseable document; the
             * advice goes to stderr so stdout stays machine-readable. */
            printf("[\n]\n");
            fprintf(stderr,
                    "No cached data for %s. Run 'email-sync' first.\n", folder);
        } else {
            printf("No cached data for %s. Run 'email-cli sync' first.\n", folder);
        }
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
        printf("\033[1;1H" TUI_HDR_ON "%s", cl);
        int used = visible_line_cols(cl, cl + strlen(cl));
        for (int p = used; p < tcols; p++) putchar(' ');
        printf("\033[0m");
        printf("\033[3;1H  %-16s  %-6s  %-*s  %s\n",
               "Date", "Sts", subj_w, "Subject", "From");
        printf("  ");
        print_dbar(16); printf("  \u2550\u2550\u2550\u2550\u2550\u2550  ");
        print_dbar(subj_w); printf("  "); print_dbar(from_w); printf("\n");
        printf("\n  \033[2m(empty)\033[0m\n");
        fflush(stdout);
        list_status_line(trows, tcols, cfg->gmail_mode, 0, 0, 0, 0, NULL);
    }
    for (;;) {
        TermKey key = terminal_read_key();
        if (key == TERM_KEY_BACK || key == TERM_KEY_ESC ||
            (key == TERM_KEY_IGNORE && terminal_last_printable() == 'q'))
            return 1;     /* back (Esc, q, Backspace) */
        if (key == TERM_KEY_QUIT) return 0;   /* ctrl-c: quit at once */
        int ch = terminal_last_printable();
        if (ch == ':') sync_menu(cfg, folder_display);
        if (ch == 'U') return 4; /* refresh: re-list */
        if (ch == 'l') return 7; /* rules editor */
    }
}

/* ── Bulk actions on the marked messages of the TUI list (EMAIL-86) ─────
 *
 * The marked rows are the table's marks (Space marks and steps down); the
 * action menu (m) applies one change to every marked message, or to the
 * message under the cursor when none is marked.  Each function does what the
 * single-message key does for one entry, and returns 0 or -1; a failure is
 * counted and the rest are still done. */
typedef struct {
    const Config *cfg;
    emc_client   *mc;               /* may be NULL (offline: queued for sync) */
    Manifest     *manifest;
    MsgEntry     *entries;
    const char   *folder;
    int           is_gmail;
    int           is_virtual_flags;
    int           is_virtual_search;
    int          *pend_remove;      /* may be NULL */
    int          *pend_label;       /* may be NULL */
    int          *pend_restore;     /* may be NULL */
} BulkCtx;

static const char *bulk_folder(const BulkCtx *b, int ei) {
    return b->entries[ei].folder[0] ? b->entries[ei].folder : b->folder;
}

/* Writes the entry's flags to the manifest(s) like the single-key handlers. */
static void bulk_save_flags(BulkCtx *b, int ei) {
    const char *uid = b->entries[ei].uid;
    const char *efolder = bulk_folder(b, ei);
    ManifestEntry *me = manifest_find(b->manifest, uid);
    if (me) me->flags = b->entries[ei].flags;
    if (b->is_virtual_flags) {
        Manifest *fm = manifest_load(efolder);
        if (fm) {
            ManifestEntry *fme = manifest_find(fm, uid);
            if (fme) { fme->flags = b->entries[ei].flags; manifest_save(efolder, fm); }
            manifest_free(fm);
        }
    } else if (!b->is_virtual_search) {
        manifest_save(efolder, b->manifest);
    }
}

/** Sets (@p want_bit 1) or clears (0) one flag bit of entry @p ei; the
 *  server's \Seen goes the other way round for MSG_FLAG_UNSEEN. */
static int bulk_set_flag(BulkCtx *b, int ei, int bit, const char *flag_name,
                         int want_bit) {
    MsgEntry *e = &b->entries[ei];
    int currently = (e->flags & bit) != 0;
    if (currently == (want_bit != 0)) return 0;
    const char *uid = e->uid;
    const char *efolder = bulk_folder(b, ei);
    int add_flag = (bit == MSG_FLAG_UNSEEN) ? !want_bit : want_bit;

    local_pending_flag_add(efolder, uid, flag_name, add_flag);
    e->flags ^= bit;
    bulk_save_flags(b, ei);

    int rc = 0;
    if (b->is_gmail) {
        const char *lbl = (bit == MSG_FLAG_UNSEEN)  ? "UNREAD"
                        : (bit == MSG_FLAG_FLAGGED) ? "STARRED" : NULL;
        if (lbl) {
            if (want_bit) {
                label_idx_add(lbl, uid);
                local_hdr_update_labels("", uid, &lbl, 1, NULL, 0);
            } else {
                label_idx_remove(lbl, uid);
                local_hdr_update_labels("", uid, NULL, 0, &lbl, 1);
            }
        } else {
            local_hdr_update_flags("", uid, e->flags);
        }
        flag_push_background(b->cfg, uid, flag_name, add_flag);
    } else if (b->mc) {
        rc = emc_set_flag(b->mc, uid, flag_name, add_flag);
    }
    return rc == 0 ? 0 : -1;
}

/** Adds (@p on 1) or removes (0) a Gmail label on entry @p ei. */
static int bulk_label(BulkCtx *b, int ei, const char *label, int on) {
    const char *uid = b->entries[ei].uid;
    int rc = 0;
    if (b->mc) rc = emc_modify_label(b->mc, uid, label, on);
    else rc = local_pending_label_add(uid, label, on);   /* offline: queue */
    if (on) {
        label_idx_add(label, uid);
        local_hdr_update_labels("", uid, &label, 1, NULL, 0);
    } else {
        label_idx_remove(label, uid);
        local_hdr_update_labels("", uid, NULL, 0, &label, 1);
    }
    return rc == 0 ? 0 : -1;
}

/** Moves entry @p ei to folder @p target (IMAP) or to the label @p target
 *  taking the current label away (Gmail). */
static int bulk_move(BulkCtx *b, int ei, const char *target) {
    const char *uid = b->entries[ei].uid;
    const char *efolder = bulk_folder(b, ei);
    int rc = 0;
    if (b->is_gmail) {
        if (efolder[0] != '_' && strcmp(efolder, target) != 0)
            rc |= bulk_label(b, ei, efolder, 0);
        rc |= bulk_label(b, ei, target, 1);
        if (b->pend_label && efolder[0] != '_') b->pend_label[ei] = 1;
        return rc == 0 ? 0 : -1;
    }
    if (b->mc) rc = emc_move_to_folder(b->mc, uid, target);
    if (rc != 0 || !b->mc)   /* no connection or refused: queue for the sync */
        rc = local_pending_move_add(efolder, uid, target);
    if (rc == 0) {
        manifest_remove(b->manifest, uid);
        if (b->pend_remove) b->pend_remove[ei] = 1;
        if (!b->is_virtual_flags && !b->is_virtual_search)
            manifest_save(efolder, b->manifest);
    }
    return rc == 0 ? 0 : -1;
}

/** Trash (or, in the trash, delete for good) entry @p ei. */
static int bulk_delete(BulkCtx *b, int ei) {
    const char *uid = b->entries[ei].uid;
    const char *efolder = bulk_folder(b, ei);
    int rc = 0;
    if (b->is_gmail) {
        int in_trash = strcmp(b->folder, "_trash") == 0;
        if (in_trash) {
            rc = gmail_delete_now(b->cfg, b->mc, uid);
            if (rc == 0) label_idx_remove("_trash", uid);
        } else {
            gmail_trash_or_queue(b->mc, uid);
            char **all_labels = NULL;
            int all_count = 0;
            label_idx_list(&all_labels, &all_count);
            for (int j = 0; j < all_count; j++) {
                label_idx_remove(all_labels[j], uid);
                free(all_labels[j]);
            }
            free(all_labels);
            label_idx_add("_trash", uid);
        }
    } else {
        char msg[80];
        rc = imap_trash_or_queue(b->mc, b->cfg, efolder, uid, msg, sizeof(msg));
    }
    if (rc == 0) {
        manifest_remove(b->manifest, uid);
        if (b->pend_remove) b->pend_remove[ei] = 1;
        if (!b->is_virtual_flags && !b->is_virtual_search)
            manifest_save(efolder, b->manifest);
    }
    return rc == 0 ? 0 : -1;
}

/* ── The keys that change state in the message list (EMAIL-175) ──────────
 *
 * The registry keys ('*', '+', '-', '!', 'd', Delete) and the actions of the
 * ':' menu act on the marked rows, or on the row under the cursor when none
 * is marked.  Routine ones happen at once, in the list and on the server; a
 * delete asks first, naming the message; the actions that start a process
 * or cannot be undone are confirmed by the table (key_table.c). */

/** Sets (@p want 1) or clears (0) the junk flag of entry @p ei, locally and on the server. */
static int bulk_junk(BulkCtx *b, int ei, int want) {
    MsgEntry *e = &b->entries[ei];
    int is_junk = (e->flags & MSG_FLAG_JUNK) != 0;
    if (is_junk == (want != 0)) return 0;
    const char *uid = e->uid;
    if (want) e->flags |= MSG_FLAG_JUNK;
    else      e->flags &= ~MSG_FLAG_JUNK;
    bulk_save_flags(b, ei);
    int rc = 0;
    if (b->is_gmail)
        junk_push_background(b->cfg, uid, want != 0);
    else if (b->mc)
        rc = want ? emc_mark_junk(b->mc, uid) : emc_mark_notjunk(b->mc, uid);
    return rc == 0 ? 0 : -1;
}

/** Gmail archive: takes every label off entry @p ei and files it as archived. */
static void gmail_archive_one(BulkCtx *b, int ei) {
    const char *uid = b->entries[ei].uid;
    char *lbl_str = local_hdr_get_labels("", uid);
    if (lbl_str) {
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
                    if (tok[0] != '_') label_idx_remove(tok, uid);
                    rm[rm_n++] = tok;
                    if (strcmp(tok, "IMPORTANT") != 0 &&
                        strncmp(tok, "CATEGORY_", 9) != 0) {
                        if (b->mc) emc_modify_label(b->mc, uid, tok, 0);
                        else local_pending_label_add(uid, tok, 0);
                    }
                }
                tok = sep ? sep + 1 : NULL;
            }
            local_hdr_update_labels("", uid, NULL, 0, (const char **)rm, rm_n);
        }
        free(copy); free(rm); free(lbl_str);
    }
    label_idx_remove("UNREAD", uid);
    {
        int new_flags = b->entries[ei].flags & ~MSG_FLAG_UNSEEN;
        local_hdr_update_flags("", uid, new_flags);
        b->entries[ei].flags = new_flags;
    }
    if (b->mc) emc_set_flag(b->mc, uid, "\\Seen", 1);
    else local_pending_label_add(uid, "UNREAD", 0);
    label_idx_add("_nolabel", uid);
    if (b->pend_label) b->pend_label[ei] = 1;
}

/** Gmail restore from Trash to the inbox. */
static void gmail_restore_one(BulkCtx *b, int ei) {
    const char *uid = b->entries[ei].uid;
    label_idx_remove("_trash", uid);
    label_idx_add("INBOX", uid);
    {
        const char *add_lbl = "INBOX";
        const char *rm_lbl  = "TRASH";
        local_hdr_update_labels("", uid, &add_lbl, 1, &rm_lbl, 1);
    }
    if (b->mc) {
        emc_modify_label(b->mc, uid, "TRASH", 0);
        emc_modify_label(b->mc, uid, "INBOX", 1);
    } else {
        /* Offline: a trash still queued is cancelled, else queue the restore */
        if (!gmail_unqueue_label(uid, "_trash", 1))
            local_pending_label_add(uid, "_trash", 0);
        local_pending_label_add(uid, "INBOX", 1);
    }
    if (b->pend_restore) b->pend_restore[ei] = 1;
}

/** Gmail: takes the current label (the list's folder) off entry @p ei; the
 *  second time on the same row puts it back. */
static void gmail_rm_label_one(BulkCtx *b, int ei, char *fb, size_t fsz) {
    const char *folder = b->folder;
    if (folder[0] == '_') return;
    const char *uid = b->entries[ei].uid;
    if (b->pend_label && b->pend_label[ei]) {
        if (b->mc) emc_modify_label(b->mc, uid, folder, 1);
        label_idx_add(folder, uid);
        local_hdr_update_labels("", uid, &folder, 1, NULL, 0);
        label_idx_remove("_nolabel", uid);
        b->pend_label[ei] = 0;
        snprintf(fb, fsz, "Undo: %s restored", folder);
        return;
    }
    if (b->mc) emc_modify_label(b->mc, uid, folder, 0);
    label_idx_remove(folder, uid);
    local_hdr_update_labels("", uid, NULL, 0, &folder, 1);
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
            if (strcmp(lb, folder) != 0 && strcmp(lb, "UNREAD") != 0 &&
                strcmp(lb, "IMPORTANT") != 0 && strncmp(lb, "CATEGORY_", 9) != 0)
                has_real = 1;
            tok = s ? s + 1 : NULL;
        }
        free(lbl);
    }
    if (!has_real) label_idx_add("_nolabel", uid);
    if (b->pend_remove) b->pend_remove[ei] = 0;
    if (b->pend_label)  b->pend_label[ei]  = 1;
    snprintf(fb, fsz, "Label removed: %s", folder);
}

/** "Sender - Subject" of entry @p ei, for a dialog that names the message. */
static void msg_describe(const BulkCtx *b, int ei, char *out, size_t sz) {
    ManifestEntry *me = manifest_find(b->manifest, b->entries[ei].uid);
    char from[128], subj[160];
    cut_utf8(from, sizeof(from),
             (me && me->from && me->from[0]) ? me->from : "(no from)", 80);
    cut_utf8(subj, sizeof(subj),
             (me && me->subject && me->subject[0]) ? me->subject : "(no subject)", 120);
    snprintf(out, sz, "\"%s - %s\"", from, subj);
}

/** Whether entry @p ei lies in the Trash (a delete there is for good). */
static int bulk_in_trash(const BulkCtx *b, int ei) {
    if (b->is_gmail) return strcmp(b->folder, "_trash") == 0;
    const char *trf = b->cfg->trash_folder ? b->cfg->trash_folder : "Trash";
    return strcmp(bulk_folder(b, ei), trf) == 0;
}

/** The question of the delete dialog: it names the message (sender and
 *  subject), or the count of messages. */
static void delete_question(const BulkCtx *b, const int *tg, int nt, int marked,
                            char *out, size_t sz) {
    int for_good = bulk_in_trash(b, tg[0]);
    if (nt == 1) {
        char d[400];
        msg_describe(b, tg[0], d, sizeof(d));
        snprintf(out, sz, for_good ? "Delete %s for good" : "Move %s to Trash", d);
    } else {
        snprintf(out, sz, for_good ? "Delete %d %smessages for good"
                                   : "Move %d %smessages to Trash",
                 nt, marked ? "marked " : "");
    }
}

/** Asks for one line on the bottom rows; returns 1 when entered and not empty. */
static int ask_line(const char *prompt, char *buf, size_t size) {
    int trows = terminal_rows();
    if (trows <= 0) trows = 24;
    InputLine il = {0};
    buf[0] = '\0';
    input_line_init(&il, buf, size, NULL);
    return input_line_run(&il, trows - 1, prompt) && buf[0];
}

/**
 * Performs @p act on the @p nt entries @p tg (indexes into the entries), the
 * marked rows or the row under the cursor, and leaves a message in @p fb.
 * @param view       The table's view, for the confirmation.
 * @param confirmed  1 when the ':' menu has asked already.
 * @return 1 when the marks should be cleared (something was done), else 0.
 */
static int list_act(EmailAction act, BulkCtx *b, const int *tg, int nt, int marked,
                    const char *view, int confirmed, char *fb, size_t fsz) {
    int done = 0, failed = 0;
    const char *one = "", *many = "";      /* the message for one / several */
    char argbuf[256] = "";
    int first_flags = b->entries[tg[0]].flags;

    switch (act) {
    case EA_STAR: {
        int want = !(first_flags & MSG_FLAG_FLAGGED);
        for (int i = 0; i < nt; i++)
            if (bulk_set_flag(b, tg[i], MSG_FLAG_FLAGGED, "\\Flagged", want) == 0) done++; else failed++;
        one = want ? "Starred" : "Unstarred";
        many = want ? "starred" : "unstarred";
        break;
    }
    case EA_READ:
    case EA_UNREAD: {
        int unseen = act == EA_UNREAD;
        for (int i = 0; i < nt; i++)
            if (bulk_set_flag(b, tg[i], MSG_FLAG_UNSEEN, "\\Seen", unseen) == 0) done++; else failed++;
        one = unseen ? "Marked as unread" : "Marked as read";
        many = unseen ? "marked unread" : "marked read";
        break;
    }
    case EA_DONE: {
        int want = !(first_flags & MSG_FLAG_DONE);
        for (int i = 0; i < nt; i++)
            if (bulk_set_flag(b, tg[i], MSG_FLAG_DONE, "$Done", want) == 0) done++; else failed++;
        one = want ? "Marked done" : "Marked not done";
        many = want ? "marked done" : "marked not done";
        break;
    }
    case EA_JUNK: {
        int want = !(first_flags & MSG_FLAG_JUNK);
        for (int i = 0; i < nt; i++)
            if (bulk_junk(b, tg[i], want) == 0) done++; else failed++;
        one = want ? "Marked as junk" : "Marked as not-junk";
        many = want ? "marked junk" : "marked not junk";
        break;
    }
    case EA_DELETE: {
        char q[600];
        delete_question(b, tg, nt, marked, q, sizeof(q));
        if (!confirmed && !ekeys_confirm(view, EA_DELETE, q)) {
            snprintf(fb, fsz, "Not deleted");
            return 0;
        }
        int for_good = bulk_in_trash(b, tg[0]);
        for (int i = 0; i < nt; i++)
            if (bulk_delete(b, tg[i]) == 0) done++; else failed++;
        one = for_good ? "Permanently deleted" : "Moved to Trash";
        many = for_good ? "deleted for good" : "moved to Trash";
        break;
    }
    case EA_ARCHIVE:
        if (strcmp(b->folder, "_nolabel") == 0) {
            snprintf(fb, fsz, "Already in Archive \xe2\x80\x94 no change");
            return 0;
        }
        for (int i = 0; i < nt; i++) { gmail_archive_one(b, tg[i]); done++; }
        one = "Archived"; many = "archived";
        break;
    case EA_RESTORE:
        for (int i = 0; i < nt; i++) { gmail_restore_one(b, tg[i]); done++; }
        one = "Restored to Inbox"; many = "restored to Inbox";
        break;
    case EA_RM_LABEL:
        if (b->folder[0] == '_') {
            snprintf(fb, fsz, "Not in a label \xe2\x80\x94 no change");
            return 0;
        }
        if (nt == 1) {
            gmail_rm_label_one(b, tg[0], fb, fsz);
            return 1;
        }
        for (int i = 0; i < nt; i++) { gmail_rm_label_one(b, tg[i], fb, fsz); done++; }
        many = "label removed from";
        break;
    case EA_ADD_LABEL:
    case EA_REMOVE_LABEL:
        if (!ask_line(act == EA_ADD_LABEL ? "Add label: " : "Remove label: ",
                      argbuf, sizeof(argbuf)))
            return 0;
        for (int i = 0; i < nt; i++)
            if (bulk_label(b, tg[i], argbuf, act == EA_ADD_LABEL) == 0) done++; else failed++;
        one = many = act == EA_ADD_LABEL ? "labelled" : "label removed from";
        break;
    case EA_MOVE:
        if (!ask_line(b->is_gmail ? "Move to label: " : "Move to folder: ",
                      argbuf, sizeof(argbuf)))
            return 0;
        for (int i = 0; i < nt; i++)
            if (bulk_move(b, tg[i], argbuf) == 0) done++; else failed++;
        one = many = "moved";
        break;
    default:
        return 0;
    }
    if (failed)
        snprintf(fb, fsz, "%d message(s) %s, %d failed", done, many, failed);
    else if (nt == 1 && one[0])
        snprintf(fb, fsz, "%s", one);
    else
        snprintf(fb, fsz, "%d message(s) %s", done, many);
    return done > 0;
}

/* Model of the message list's table (tui_table) in the TUI: the rows are the
 * entries shown (through the filter map when a filter is active).  Columns:
 * Date, Sts, Subject, From.  A row with a pending operation carries a mark
 * ("D" red trash, "d" yellow label removed / archived, "u" green restore) and
 * its own colour. */
typedef struct {
    const MsgEntry *entries;
    const int      *fentries;   /* display index -> entries[] index, or NULL */
    Manifest       *manifest;
    const int      *pend_remove;
    const int      *pend_label;
    const int      *pend_restore;
    int             from_w;     /* cells the From column has, 0 when unknown */
} MsgListModel;

static int mlm_index(const MsgListModel *m, int i) {
    return m->fentries ? m->fentries[i] : i;
}

static void msglist_row(void *ctx, int i, tui_table_row *r) {
    const MsgListModel *m = ctx;
    int ei = mlm_index(m, i);
    if (m->pend_remove && m->pend_remove[ei]) {
        tui_table_row_mark(r, "D", TUI_STYLE_ERROR);
        tui_table_row_style(r, (tui_style){ TUI_RED, TUI_COLOR_DEFAULT, 0 });
    } else if (m->pend_label && m->pend_label[ei]) {
        tui_table_row_mark(r, "d", (tui_style){ TUI_YELLOW, TUI_COLOR_DEFAULT, TUI_ATTR_BOLD });
        tui_table_row_style(r, (tui_style){ TUI_YELLOW, TUI_COLOR_DEFAULT, 0 });
    } else if (m->pend_restore && m->pend_restore[ei]) {
        tui_table_row_mark(r, "u", TUI_STYLE_OK);
        tui_table_row_style(r, (tui_style){ TUI_GREEN, TUI_COLOR_DEFAULT, 0 });
    }
}

static void msglist_cell(void *ctx, int i, int col, tui_table_cell *c) {
    const MsgListModel *m = ctx;
    int ei = mlm_index(m, i);
    const MsgEntry *e = &m->entries[ei];
    ManifestEntry *me = manifest_find(m->manifest, e->uid);
    if (col == 0) {
        tui_table_cell_put(c, (me && me->date) ? me->date : "", TUI_STYLE_DEFAULT);
        return;
    }
    if (col == 2) {
        tui_table_cell_put(c, (me && me->subject && me->subject[0]) ? me->subject
                                                                    : "(no subject)",
                           TUI_STYLE_DEFAULT);
        return;
    }
    if (col == 3) {
        const char *from = (me && me->from && me->from[0]) ? me->from : "(no from)";
        /* A column too narrow for "Name <address>" shows the name alone: a
         * cut address is unreadable, a whole name is not. */
        if (m->from_w > 0 && tui_str_width(from) > m->from_w) {
            const char *lt = strstr(from, " <");
            if (lt && lt > from) {
                char name[256];
                size_t n = (size_t)(lt - from);
                if (n >= sizeof(name)) n = sizeof(name) - 1;
                memcpy(name, from, n);
                name[n] = '\0';
                /* strip the quotes of "Doe, John" <j@example.org> */
                char *q = name;
                size_t ql = strlen(q);
                if (ql >= 2 && q[0] == '"' && q[ql - 1] == '"') { q[ql - 1] = '\0'; q++; }
                tui_table_cell_put(c, q, TUI_STYLE_DEFAULT);
                return;
            }
        }
        tui_table_cell_put(c, from, TUI_STYLE_DEFAULT);
        return;
    }
    /* Status column (6 cells): [P/J/N/-][star/-][D/-][A/-][R/F/-][v/x/-];
     * the status letters carry their colour only while no operation is
     * pending on the row (the row's own colour shows that instead). */
    int computed = MSG_FLAG_ATTACH | MSG_FLAG_ATTACH_CHECKED |
                   MSG_FLAG_DMARC_PASS | MSG_FLAG_DMARC_FAIL;
    int f = e->flags | (me ? (me->flags & computed) : 0);
    int pending = (m->pend_remove && m->pend_remove[ei]) ||
                  (m->pend_label && m->pend_label[ei]) ||
                  (m->pend_restore && m->pend_restore[ei]);
    tui_style plain = TUI_STYLE_DEFAULT;
    tui_style red = { TUI_RED, TUI_COLOR_DEFAULT, TUI_ATTR_BOLD };
    tui_style yel = { TUI_YELLOW, TUI_COLOR_DEFAULT, 0 };
    tui_style grn = { TUI_GREEN, TUI_COLOR_DEFAULT, 0 };
    tui_style cya = { TUI_CYAN, TUI_COLOR_DEFAULT, 0 };
    if (pending) { red = yel = grn = cya = plain; }
    if      (f & MSG_FLAG_PHISHING) tui_table_cell_put(c, "P", red);
    else if (f & MSG_FLAG_JUNK)     tui_table_cell_put(c, "J", yel);
    else if (f & MSG_FLAG_UNSEEN)   tui_table_cell_put(c, "N", grn);
    else                            tui_table_cell_put(c, "-", plain);
    if (f & MSG_FLAG_FLAGGED) tui_table_cell_put(c, pending ? "*" : "\xe2\x98\x85", yel);
    else                      tui_table_cell_put(c, "-", plain);
    tui_table_cell_put(c, (f & MSG_FLAG_DONE)   ? "D" : "-", plain);
    tui_table_cell_put(c, (f & MSG_FLAG_ATTACH) ? "A" : "-", plain);
    if      (f & MSG_FLAG_ANSWERED)  tui_table_cell_put(c, "R", cya);
    else if (f & MSG_FLAG_FORWARDED) tui_table_cell_put(c, "F", cya);
    else                             tui_table_cell_put(c, "-", plain);
    if (f & MSG_FLAG_DMARC_PASS)
        tui_table_cell_put(c, pending ? "v" : "\xe2\x9c\x93", grn);
    else if (f & MSG_FLAG_DMARC_FAIL)
        tui_table_cell_put(c, pending ? "x" : "\xe2\x9c\x97", red);
    else
        tui_table_cell_put(c, "-", plain);
}
/* Identity of a table row for the marks: the entry's UID, so the marks stay
 * with the message when a filter hides or reorders rows. */
static uint64_t msglist_row_id(void *ctx, int index) {
    const MsgListModel *m = ctx;
    const char *uid = m->entries[mlm_index(m, index)].uid;
    uint64_t h = 1469598103934665603ULL;            /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)uid; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
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
        /* Mask 0 = no flag requirement, i.e. every cached message in every
         * folder — the cross-folder counterpart of a plain folder listing. */
        if (strcmp(folder, "__all__")       == 0) { is_virtual_flags = 1; virtual_flag_mask = 0;                    folder_display = "All mail";  }
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
    EMC_AUTO_CLOSE emc_client *list_mc = NULL;

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
        local_search(search_query, search_scope, cfg->gmail_mode, &sr, &sr_count);
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
    } else if (cfg->sync_interval > 0 && !cfg->gmail_mode) {
        /* ── IMAP cron / cache-only mode: serve entirely from manifest ───
         *
         * Manifests are an IMAP-side artifact: gmail_sync.c writes
         * labels/<label>.idx and headers/<uid>.hdr, never a manifest.  Testing
         * sync_interval before gmail_mode therefore routed every Gmail account
         * with a cron sync into a branch reading a file nothing ever fills,
         * reporting "No cached data" over a fully populated store.  Storage
         * format follows the account type; only the refresh policy follows
         * sync_interval. */
        if (manifest->count == 0) {
            manifest_free(manifest);
            return list_empty_cache_view(cfg, opts, folder, folder_display);
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

        /* The cross-folder flag views are an IMAP construct: Gmail expresses
         * the same thing as real labels, which the user selects by name.
         * Saying so beats advising a sync that would change nothing. */
        if (is_virtual_folder_name(folder)) {
            free(idx_uids);
            manifest_free(manifest);
            if (!opts->pager)
                fprintf(stderr,
                        "'%s' is an IMAP-only view. On Gmail use the label "
                        "itself, e.g. --folder UNREAD or --folder STARRED "
                        "('list-labels' shows them all).\n", folder);
            return 1;
        }

        /* A missing or empty label index means the label was never synced —
         * the Gmail equivalent of an absent manifest, so give the same advice
         * rather than the flat "no messages" that suggests an empty mailbox.
         *
         * Batch output only: the TUI keeps rendering its normal empty view,
         * whose status bar carries the hints that belong to the label being
         * viewed (Trash offers u=restore, for one).  The generic panel would
         * drop them, and an empty Trash is an ordinary state, not a missing
         * cache. */
        if (idx_count == 0 && !opts->pager) {
            free(idx_uids);
            manifest_free(manifest);
            return list_empty_cache_view(cfg, opts, folder, folder_display);
        }

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
        if (emc_select(list_mc, folder) != 0) {
            manifest_free(manifest);
            fprintf(stderr, "Failed to select folder %s.\n", folder);
            return -1;
        }

        char (*unseen_uids)[17] = NULL;
        int  unseen_uid_count = 0;
        if (emc_search(list_mc, EMC_SEARCH_UNREAD, &unseen_uids, &unseen_uid_count) != 0) {
            manifest_free(manifest);
            fprintf(stderr, "Failed to search mailbox.\n");
            return -1;
        }

        char (*flagged_uids)[17] = NULL;
        int flagged_count = 0;
        emc_search(list_mc, EMC_SEARCH_FLAGGED, &flagged_uids, &flagged_count);
        /* ignore errors — treat as 0 flagged */

        char (*done_uids)[17] = NULL;
        int done_count = 0;
        emc_search(list_mc, EMC_SEARCH_DONE, &done_uids, &done_count);
        /* ignore errors — treat as 0 done */

        char (*all_uids)[17] = NULL;
        int  all_count = 0;
        if (emc_search(list_mc, EMC_SEARCH_ALL, &all_uids, &all_count) != 0) {
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
                printf("\033[1;1H" TUI_HDR_ON "%s", cl);
                int used = visible_line_cols(cl, cl + strlen(cl));
                for (int p = used; p < tcols; p++) putchar(' ');
                printf("\033[0m");
                printf("\033[3;1H  %-16s  %-6s  %-*s  %s\n",
                       "Date", "Sts", subj_w, "Subject", "From");
                printf("  ");
                print_dbar(16); printf("  \u2550\u2550\u2550\u2550\u2550\u2550  ");
                print_dbar(subj_w); printf("  "); print_dbar(from_w); printf("\n");
                printf("\n  \033[2m(empty)\033[0m\n");
                fflush(stdout);
                list_status_line(trows, tcols, cfg->gmail_mode,
                                 cfg->gmail_mode && strcmp(folder, "_trash") == 0,
                                 0, 0, 0, NULL);
            }
            for (;;) {
                TermKey key = terminal_read_key();
                if (key == TERM_KEY_BACK || key == TERM_KEY_ESC ||
                    (key == TERM_KEY_IGNORE && terminal_last_printable() == 'q'))
                    return 1;     /* back (Esc, q, Backspace) */
                if (key == TERM_KEY_QUIT) return 0;   /* ctrl-c: quit at once */
                int ch = terminal_last_printable();
                if (ch == 'c') return 2; /* compose */
                if (ch == ':') sync_menu(cfg, folder_display);
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
            printf("\033[1;1H" TUI_HDR_ON "%s", cl);
            int used = visible_line_cols(cl, cl + strlen(cl));
            for (int p = used; p < tcols; p++) putchar(' ');
            printf("\033[0m");
            printf("\033[3;1H  %-16s  %-6s  %-*s  %s\n",
                   "Date", "Sts", subj_w, "Subject", "From");
            printf("  ");
            print_dbar(16); printf("  \u2550\u2550\u2550\u2550\u2550\u2550  ");
            print_dbar(subj_w); printf("  "); print_dbar(from_w); printf("\n");
            printf("\n  \033[2m(empty)\033[0m\n");
            fflush(stdout);
            list_status_line(trows, tcols, cfg->gmail_mode,
                             cfg->gmail_mode && strcmp(folder, "_trash") == 0,
                             0, 0, 0, NULL);
        }
        for (;;) {
            TermKey key = terminal_read_key();
            if (key == TERM_KEY_BACK || key == TERM_KEY_ESC ||
                (key == TERM_KEY_IGNORE && terminal_last_printable() == 'q'))
                return 1;     /* back (Esc, q, Backspace) */
            if (key == TERM_KEY_QUIT) return 0;   /* ctrl-c: quit at once */
            int ch = terminal_last_printable();
            if (ch == 'c') return 2; /* compose */
            if (ch == ':') sync_menu(cfg, folder_display);
            if (ch == 'U') return 4; /* refresh */
            if (ch == 'l') return 7; /* rules editor */
        }
    }

    /* Batch filters run before sorting and paging so that --limit/--offset and
     * the "N more message(s)" hint all refer to the filtered set. */
    show_count = apply_list_filters(manifest, entries, show_count, opts);

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
    /* An action of the ':' menu that asks for a line (move, add or remove a
     * label) runs after the view has been drawn again, so that no part of the
     * menu is left on the screen under the prompt (EMAIL-175). */
    EmailAction deferred_act = EA_NONE;

    /* ── Live filter state ──────────────────────────────────────────── */
    int   filter_active = 0;   /* filter bar is visible */
    int   filter_input  = 0;   /* typing mode (vs navigation mode) */
    int   filter_scope  = 0;   /* 0=Subject 1=From 2=To 3=Body 4=Since 5=Before */
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

    /* The TUI's table: columns, model and the marks of Space (EMAIL-86). */
    static const tui_table_col list_cols[] = {
        { "Date",    16, 0, TUI_ALIGN_LEFT, 0, 0, 0 },
        { "Sts",      6, 0, TUI_ALIGN_LEFT, 0, 0, 0 },
        { "Subject", 10, 3, TUI_ALIGN_LEFT, 0, 0, 0 },
        { "From",    10, 2, TUI_ALIGN_LEFT, 0, 0, 0 },
    };
    MsgListModel lmodel = { entries, NULL, manifest, pending_remove, pending_label,
                            pending_restore, 0 };
    tui_table_model ltmodel = { msglist_row, msglist_cell, &lmodel };
    tui_table ltbl;
    tui_table_init(&ltbl, list_cols, 4, &ltmodel);
    ltbl.flags |= TUI_TABLE_RULER | TUI_TABLE_MARKS;
    tui_table_set_row_id(&ltbl, msglist_row_id, &lmodel);

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
        /* Cross-folder views draw rows from several mailboxes; without the
         * folder the UID is not enough to open the message again. */
        int show_folder = !opts->pager && (is_virtual_flags || is_virtual_search);
        int overhead = show_uid ? 48 : 30;  /* 48 = 30 + uid(16) + sep(2) */
        if (show_folder) overhead += 22;    /* folder(20) + sep(2) */
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
                ManifestEntry *cme = manifest_find(manifest, entries[i].uid);
                int need_dmarc  = cme && !(cme->flags & MSG_FLAG_DMARC_CHECKED);
                int need_attach = cme && !(cme->flags & MSG_FLAG_ATTACH_CHECKED);
                if (cme && !need_dmarc && !need_attach) continue;
                /* For Gmail: .hdr files use from\tsubject\tdate\tlabels\tflags format,
                 * not RFC 2822 headers — Content-Type/Authentication-Results are absent.
                 * Read the full local message instead (no network access needed). */
                char *hdrs;
                if (cfg->gmail_mode) {
                    hdrs = local_msg_load("", entries[i].uid);
                } else {
                    hdrs = list_mc
                           ? fetch_uid_headers_via(list_mc, folder, entries[i].uid)
                           : fetch_uid_headers_cached(cfg, folder, entries[i].uid);
                }
                if (!cme) {
                    /* New entry: full header parse */
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
                    entries[i].flags |= MSG_FLAG_ATTACH_CHECKED;
                    free(ct_raw);
                    char *ar_raw = hdrs ? mime_get_header(hdrs, "Authentication-Results") : NULL;
                    int dmarc_st = mime_get_dmarc_status(ar_raw);
                    free(ar_raw);
                    if      (dmarc_st ==  1) entries[i].flags |= MSG_FLAG_DMARC_PASS;
                    else if (dmarc_st == -1) entries[i].flags |= MSG_FLAG_DMARC_FAIL;
                    if (dmarc_st != -2) entries[i].flags |= MSG_FLAG_DMARC_CHECKED;
                    free(hdrs);
                    manifest_upsert(manifest, entries[i].uid, fr, su, dt, entries[i].flags);
                } else {
                    /* Existing entry: update DMARC and/or attach bits from local cache */
                    if (need_attach) {
                        char *ct2 = hdrs ? mime_get_header(hdrs, "Content-Type") : NULL;
                        if (ct2 && strcasestr(ct2, "multipart/mixed"))
                            cme->flags |= MSG_FLAG_ATTACH;
                        cme->flags |= MSG_FLAG_ATTACH_CHECKED;
                        free(ct2);
                    }
                    char *ar_raw = hdrs ? mime_get_header(hdrs, "Authentication-Results") : NULL;
                    int dmarc_st = mime_get_dmarc_status(ar_raw);
                    free(ar_raw);
                    free(hdrs);
                    if      (dmarc_st ==  1) cme->flags |= MSG_FLAG_DMARC_PASS;
                    else if (dmarc_st == -1) cme->flags |= MSG_FLAG_DMARC_FAIL;
                    if (dmarc_st != -2) cme->flags |= MSG_FLAG_DMARC_CHECKED;
                    entries[i].flags = cme->flags;
                    /* Persist updated flags so next startup skips backfill */
                    if (cfg->gmail_mode)
                        local_hdr_update_flags("", entries[i].uid, cme->flags);
                }
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

        char pager_hdr[512] = "";

        /* Count / status line — suppressed in JSON mode, where stdout must be
         * a single parseable document and nothing else. */
        if (!opts->json) {
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
                static const char *scn[] = {"Subject","From","To","Body","Since","Before"};
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
                /* TUI mode: the header bar is drawn with the table */
                int nm = tui_table_marked_count(&ltbl);
                if (nm > 0) {
                    size_t cl_len = strlen(cl);
                    snprintf(cl + cl_len, sizeof(cl) - cl_len, "  [%d marked]", nm);
                }
                snprintf(pager_hdr, sizeof(pager_hdr), "%s", cl);
            } else {
                printf("%s\n\n", cl);
            }
        }
        if (opts->json) {
            printf("[");
        } else if (!opts->pager) {
        if (show_uid && show_folder)
            printf("  %-16s  %-16s  %-6s  %-20s  %-*s  %s\n",
                   "UID", "Date", "Sts", "Folder", subj_w, "Subject", "From");
        else if (show_uid)
            printf("  %-16s  %-16s  %-6s  %-*s  %s\n",
                   "UID", "Date", "Sts", subj_w, "Subject", "From");
        else
            printf("  %-16s  %-6s  %-*s  %s\n",
                   "Date", "Sts", subj_w, "Subject", "From");
        printf("  ");
        if (show_uid) { print_dbar(16); printf("  "); }
        print_dbar(16); printf("  ");
        printf("\u2550\u2550\u2550\u2550\u2550\u2550  ");
        if (show_folder) { print_dbar(20); printf("  "); }
        print_dbar(subj_w > 0 ? subj_w : 30); printf("  ");
        print_dbar(from_w > 0 ? from_w : 40); printf("\n");
        }

        /* TUI: the rows are drawn by the table, one frame per fetched row so a
         * slow fetch shows progress, and a last frame when the window is full.
         * The table itself (its marks) lives across the iterations. */
        lmodel.fentries = (filter_active && fentries) ? fentries : NULL;
        int pager_frame = opts->pager && !opts->json;
        int pager_rows = terminal_rows();
        int pager_cols = terminal_cols();
        if (pager_rows <= 0) pager_rows = 24;
        if (pager_cols <= 0) pager_cols = 80;
        {
            int lw[4] = { 0, 0, 0, 0 };
            tui_table_widths(&ltbl, pager_cols, lw);
            lmodel.from_w = lw[3];
        }

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
                        if (key == TERM_KEY_BACK || key == TERM_KEY_ESC ||
                            (key == TERM_KEY_IGNORE &&
                             terminal_last_printable() == 'q')) {
                            list_result = 1; load_interrupted = 1; break;
                        }
                        if (key == TERM_KEY_QUIT) {
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
                /* Detect attachment and DMARC result from headers */
                char *ct_raw = hdrs ? mime_get_header(hdrs, "Content-Type") : NULL;
                if (ct_raw && strcasestr(ct_raw, "multipart/mixed"))
                    entries[ei].flags |= MSG_FLAG_ATTACH;
                entries[ei].flags |= MSG_FLAG_ATTACH_CHECKED;
                free(ct_raw);
                char *ar_raw2 = hdrs ? mime_get_header(hdrs, "Authentication-Results") : NULL;
                int dmarc_st2 = mime_get_dmarc_status(ar_raw2);
                free(ar_raw2);
                if      (dmarc_st2 ==  1) entries[ei].flags |= MSG_FLAG_DMARC_PASS;
                else if (dmarc_st2 == -1) entries[ei].flags |= MSG_FLAG_DMARC_FAIL;
                if (dmarc_st2 != -2) entries[ei].flags |= MSG_FLAG_DMARC_CHECKED;
                free(hdrs);
                manifest_upsert(manifest, entries[ei].uid, fr, su, dt, entries[ei].flags);
                manifest_dirty = 1;
            } else if (!(cached_me->flags & MSG_FLAG_DMARC_CHECKED) ||
                       !(cached_me->flags & MSG_FLAG_ATTACH_CHECKED)) {
                /* Cached entry needing DMARC and/or attach check.
                 * For Gmail: headers are in `from\tsubject\tdate\tlabels\tflags` format,
                 * not RFC 2822 — Content-Type/Authentication-Results are not available
                 * there.  Read from the full local message instead (no network).
                 * For IMAP: fall back to the per-folder header cache (may download
                 * if the header file is missing, but that is IMAP-normal). */
                char *hdrs2;
                const char *hf2 = entries[ei].folder[0] ? entries[ei].folder : folder;
                if (cfg->gmail_mode) {
                    hdrs2 = local_msg_load("", entries[ei].uid);
                } else {
                    hdrs2 = fetch_uid_headers_cached(cfg, hf2, entries[ei].uid);
                }
                if (!(cached_me->flags & MSG_FLAG_ATTACH_CHECKED)) {
                    char *ct3 = hdrs2 ? mime_get_header(hdrs2, "Content-Type") : NULL;
                    if (ct3 && strcasestr(ct3, "multipart/mixed"))
                        cached_me->flags |= MSG_FLAG_ATTACH;
                    cached_me->flags |= MSG_FLAG_ATTACH_CHECKED;
                    free(ct3);
                }
                char *ar_raw3 = hdrs2 ? mime_get_header(hdrs2, "Authentication-Results") : NULL;
                int dmarc_st3 = mime_get_dmarc_status(ar_raw3);
                free(ar_raw3);
                free(hdrs2);
                if      (dmarc_st3 ==  1) cached_me->flags |= MSG_FLAG_DMARC_PASS;
                else if (dmarc_st3 == -1) cached_me->flags |= MSG_FLAG_DMARC_FAIL;
                if (dmarc_st3 != -2) cached_me->flags |= MSG_FLAG_DMARC_CHECKED;
                /* Persist updated flags to the .hdr file so next startup skips backfill */
                local_hdr_update_flags(cfg->gmail_mode ? "" : hf2,
                                       entries[ei].uid, cached_me->flags);
                manifest_dirty = 1;
            } else {
                /* Sync server-controlled bits; preserve locally-computed bits */
                int computed = MSG_FLAG_ATTACH | MSG_FLAG_ATTACH_CHECKED |
                               MSG_FLAG_DMARC_PASS | MSG_FLAG_DMARC_FAIL | MSG_FLAG_DMARC_CHECKED;
                int merged = entries[ei].flags | (cached_me->flags & computed);
                if (merged != cached_me->flags) {
                    cached_me->flags = merged;
                    manifest_dirty = 1;
                }
            }

            if (pager_frame) {
                tui_table_set_count(&ltbl, i + 1);
                tui_table_select(&ltbl, cursor);
                ltbl.lv.scroll_top = wstart;
                if (!cached_me) tui_frame_draw_table(pager_hdr, &ltbl, pager_rows, pager_cols, 0, NULL, NULL);
                continue;
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

            /* Status column (6 chars): [P/J/N/-][star/-][D/-][A/-][R/F/-][✓/✗/-]
             * Position 1: P=phishing(red) > J=junk(yellow) > N=unread(green) > -
             * Position 2: star = flagged (yellow)
             * Position 3: D = done
             * Position 4: A = attachment
             * Position 5: R=answered(cyan), F=forwarded(cyan), - = neither
             * Position 6: ✓=dmarc pass(green), ✗=dmarc fail(red), - = unknown
             *
             * TUI: ANSI colours; sel rows exit/re-enter reverse-video around colour. */
            char sts[128];
            /* Merge server bits (entries[]) with computed bits (manifest) */
            ManifestEntry *me_sts = manifest_find(manifest, entries[ei].uid);
            int computed_mask = MSG_FLAG_ATTACH | MSG_FLAG_ATTACH_CHECKED | MSG_FLAG_DMARC_PASS | MSG_FLAG_DMARC_FAIL;
            int eflags = entries[ei].flags | (me_sts ? (me_sts->flags & computed_mask) : 0);
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
                const char *dm_s;
                if      (eflags & MSG_FLAG_DMARC_PASS)
                    dm_s = sel ? "\033[0m\033[32m\xe2\x9c\x93\033[7m"
                               : "\033[32m\xe2\x9c\x93\033[0m";
                else if (eflags & MSG_FLAG_DMARC_FAIL)
                    dm_s = sel ? "\033[0m\033[1;31m\xe2\x9c\x97\033[7m"
                               : "\033[1;31m\xe2\x9c\x97\033[0m";
                else
                    dm_s = "-";
                snprintf(sts, sizeof(sts), "%s%s%c%c%s%s", n_s, f_s,
                    (eflags & MSG_FLAG_DONE)   ? 'D' : '-',
                    (eflags & MSG_FLAG_ATTACH) ? 'A' : '-',
                    rf_s, dm_s);
            } else {
                sts[0] = (eflags & MSG_FLAG_PHISHING)  ? 'P'
                       : (eflags & MSG_FLAG_JUNK)       ? 'J'
                       : (eflags & MSG_FLAG_UNSEEN)     ? 'N' : '-';
                sts[1] = (eflags & MSG_FLAG_FLAGGED)   ? '*' : '-';
                sts[2] = (eflags & MSG_FLAG_DONE)      ? 'D' : '-';
                sts[3] = (eflags & MSG_FLAG_ATTACH)    ? 'A' : '-';
                sts[4] = (eflags & MSG_FLAG_ANSWERED)  ? 'R'
                       : (eflags & MSG_FLAG_FORWARDED) ? 'F' : '-';
                sts[5] = (eflags & MSG_FLAG_DMARC_PASS) ? 'v'
                       : (eflags & MSG_FLAG_DMARC_FAIL)  ? 'x' : '-';
                sts[6] = '\0';
            }
            if (opts->json) {
                /* One object per message, fields untruncated — the whole point
                 * of this mode is that a consumer gets complete values. */
                printf("%s\n  {\"uid\": ", (i > wstart) ? "," : "");
                print_json_string(entries[ei].uid);
                printf(", \"date\": ");    print_json_string(date);
                printf(", \"from\": ");    print_json_string(from);
                printf(", \"subject\": "); print_json_string(subject);
                printf(", \"folder\": ");
                print_json_string(entries[ei].folder[0] ? entries[ei].folder : folder);
                printf(", \"status\": ");  print_json_string(sts);
                printf(", \"flags\": %d", eflags);
                printf(", \"unread\": %s",    (eflags & MSG_FLAG_UNSEEN)   ? "true" : "false");
                printf(", \"flagged\": %s",   (eflags & MSG_FLAG_FLAGGED)  ? "true" : "false");
                printf(", \"attachments\": %s",(eflags & MSG_FLAG_ATTACH)  ? "true" : "false");
                printf("}");
                fflush(stdout);
                continue;
            }
            if (show_uid)
                printf("%s%-16.16s  %-16.16s  %s  ", row_pfx, entries[ei].uid, date, sts);
            else
                printf("%s%-16.16s  %s  ", row_pfx, date, sts);
            if (show_folder)
                printf("%-20.20s  ",
                       entries[ei].folder[0] ? entries[ei].folder : folder);
            print_padded_col(subject, subj_w);
            printf("  ");
            print_padded_col(from,    from_w);

            if (opts->pager && (sel || remove_pending || label_pending || restore_pending))
                printf("\033[K\033[0m");
            printf("\n");
            fflush(stdout); /* show row immediately as it arrives */
        }
        if (manifest_dirty && !is_virtual_flags && !is_virtual_search) manifest_save(folder, manifest);
        if (load_interrupted) goto list_done;
        if (pager_frame) {
            tui_table_set_count(&ltbl, wend);
            tui_table_select(&ltbl, cursor);
            ltbl.lv.scroll_top = wstart;
            tui_frame_draw_table(pager_hdr, &ltbl, pager_rows, pager_cols, 0, NULL, NULL);
        }

        if (opts->json) {
            printf("\n]\n");
            fflush(stdout);
            break;
        }
        if (!opts->pager) {
            if (wend < show_count)
                printf("\n  -- %d more message(s) --  use --offset %d for next page\n",
                       show_count - wend, wend + 1);
            break;
        }

        /* Filter bar — shown when filter is active (separator + input = 2 rows) */
        if (filter_active) {
            static const char *scope_names[] = {"Subject","From","To","Body","Since","Before"};
            printf("\033[%d;1H", 5 + (wend - wstart));
            printf("  \xe2\x94\x80"); /* ─ */
            for (int _p = 3; _p < tcols - 2; _p++) printf("\xe2\x94\x80");
            printf("\n  Filter [");
            for (int _s = 0; _s < 6; _s++) {
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
            const char *imap_trf = cfg->trash_folder ? cfg->trash_folder : "Trash";
            int in_trash = is_gmail ? strcmp(folder, "_trash") == 0
                                    : strcmp(folder, imap_trf) == 0;
            /* One line: a message replaces the hints until the next key. */
            list_status_line(trows, tcols, is_gmail, in_trash, 1,
                             show_count > 0 ? cursor + 1 : 0, show_count,
                             feedback_msg);
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
        EmailAction deferred_now = deferred_act;
        deferred_act = EA_NONE;
        TermKey key = deferred_now ? TERM_KEY_IGNORE : terminal_read_key();
        fprintf(stderr, "\r\033[K"); fflush(stderr);
        /* The message lives until the next key (not until the next redraw). */
        if (key != TERM_KEY_IGNORE || terminal_last_printable())
            feedback_msg[0] = '\0';
        /* Only loop if read() was actually interrupted by SIGCHLD (EINTR path
         * returns TERM_KEY_IGNORE with no printable char).  If SIGCHLD fired
         * between prev_sync_done and read(), the real key is not TERM_KEY_IGNORE
         * and must be processed — otherwise the keypress is silently consumed. */
        if (!deferred_now && bg_sync_done && !prev_sync_done && key == TERM_KEY_IGNORE
                && !terminal_last_printable()) {
            goto read_key_again;
        }

        /* q is Esc (go back); it never quits.  While a filter is being typed
         * it is just a character. */
        if (key == TERM_KEY_IGNORE && terminal_last_printable() == 'q' &&
            !filter_input)
            key = TERM_KEY_ESC;

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
            goto list_done;          /* ctrl-c: list_result 0 = quit at once */
        case TERM_KEY_ESC:
            if (tui_table_marked_count(&ltbl) > 0) {
                /* First Esc drops the marks; the next goes on */
                tui_table_clear_marks(&ltbl);
                break;
            }
            if (filter_active) {
                /* First Esc clears the filter; the next goes back */
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
            list_result = 1;         /* back to the folder / label browser */
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
                if (ret == 7) {
                    /* 'A' pressed in reader -> reply to all */
                    memcpy(opts->action_uid, entries[ei_cur].uid, 17);
                    snprintf(opts->action_folder, sizeof(opts->action_folder), "%s", efolder);
                    list_result = 6;
                    goto list_done;
                }
                if (ret == 5) {
                    /* 'F' pressed in reader → forward this message */
                    memcpy(opts->action_uid, entries[ei_cur].uid, 17);
                    snprintf(opts->action_folder, sizeof(opts->action_folder), "%s", efolder);
                    list_result = 5;
                    goto list_done;
                }
                if (ret == 6) {
                    /* 'D' in reader → trash or permanent delete; mark entry removed */
                    const char *del_uid = entries[ei_cur].uid;
                    manifest_remove(manifest, del_uid);
                    if (pending_remove) pending_remove[ei_cur] = 1;
                    if (!is_virtual_flags && !is_virtual_search) manifest_save(efolder, manifest);
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
                filter_scope = (filter_scope + 1) % 6;
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

            /* ── Marks (EMAIL-86): Space marks and steps down, x clears ──── */
            if (ch == ' ' && !filter_input && disp_count > 0) {
                tui_table_set_marked(&ltbl, cursor, !tui_table_marked(&ltbl, cursor));
                if (cursor < disp_count - 1) cursor++;
                break;
            }
            if (ch == 'x' && !filter_input) {
                tui_table_clear_marks(&ltbl);
                break;
            }
            /* ── Movement: j and k (EMAIL-175) ───────────────────────────── */
            if (!filter_input && ch == 'j') {
                if (cursor < disp_count - 1) cursor++;
                break;
            }
            if (!filter_input && ch == 'k') {
                if (cursor > 0) cursor--;
                break;
            }

            /* ── Keys that change state and the ':' menu (EMAIL-175) ──────
             * Every one of them comes from the key table (key_table.c); the
             * routine ones act at once on the marked rows, or on the row
             * under the cursor, a delete asks first naming the message, and
             * the rest is in the menu. */
            const char *list_view = is_gmail ? EKV_LIST_GMAIL : EKV_LIST_IMAP;
            if (deferred_now) ch = 0;
            EmailAction act = deferred_now ? deferred_now
                            : filter_input ? EA_NONE : ekeys_find(list_view, key, ch);
            if (!filter_input && (ch == ':' || act != EA_NONE)) {
                int *targets = malloc((size_t)(disp_count > 0 ? disp_count : 1) * sizeof(int));
                int nt = 0;
                for (int di = 0; targets && di < disp_count; di++)
                    if (tui_table_marked(&ltbl, di))
                        targets[nt++] = (filter_active && fentries) ? fentries[di] : di;
                int on_marked = nt > 0;
                if (targets && nt == 0 && disp_count > 0)
                    targets[nt++] = (filter_active && fentries) ? fentries[cursor] : cursor;
                BulkCtx bctx = { cfg, list_mc, manifest, entries, folder, is_gmail,
                                 is_virtual_flags, is_virtual_search,
                                 pending_remove, pending_label, pending_restore };
                int confirmed = deferred_now != EA_NONE;
                if (ch == ':') {
                    char sync_t[400], del_t[600];
                    const char *tt[EA_COUNT] = { NULL };
                    snprintf(sync_t, sizeof(sync_t), "%s (%s)", folder_display,
                             cfg->user ? cfg->user : "?");
                    tt[EA_SYNC] = sync_t;
                    if (targets && nt > 0) {
                        delete_question(&bctx, targets, nt, on_marked, del_t, sizeof(del_t));
                        tt[EA_DELETE] = del_t;
                    }
                    act = ekeys_menu(list_view, tt);
                    confirmed = 1;      /* the menu asked what needed asking */
                    if (act == EA_MOVE || act == EA_ADD_LABEL || act == EA_REMOVE_LABEL) {
                        deferred_act = act;         /* draw the list, then ask */
                        free(targets);
                        break;
                    }
                }
                if (act == EA_SYNC) {
                    sync_start_background();   /* the confirmation was the menu's */
                } else if (act == EA_LABELS && targets && nt > 0) {
                    const char *uid = entries[targets[0]].uid;
                    /* Remember whether this message is currently in Archive or Trash
                     * so we can show green feedback if a label addition removes it. */
                    int was_archived = label_idx_contains("_nolabel", uid);
                    int was_trashed  = label_idx_contains("_trash",   uid);
                    show_label_picker(list_mc, uid, feedback_msg, sizeof(feedback_msg));
                    if ((was_archived && !label_idx_contains("_nolabel", uid)) ||
                        (was_trashed  && !label_idx_contains("_trash",   uid)))
                        if (pending_restore) pending_restore[targets[0]] = 1;
                } else if (act != EA_NONE && targets && nt > 0) {
                    if (list_act(act, &bctx, targets, nt, on_marked, list_view,
                                 confirmed, feedback_msg, sizeof(feedback_msg))
                        && on_marked)
                        tui_table_clear_marks(&ltbl);
                }
                free(targets);
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
            if (ch == '?') {
                ekeys_help(list_view, is_gmail ? "Message list shortcuts (Gmail)"
                                               : "Message list shortcuts");
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
    tui_table_free(&ltbl);
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
    EMC_AUTO_CLOSE emc_client *mc = make_mail(cfg);
    if (!mc) return NULL;

    char **folders = NULL;
    int count = 0;
    char sep = '.';
    if (emc_list_names(mc, &folders, &count, &sep) != 0) return NULL;

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
        if (g_json_out && folders) printf("[]\n");
        else printf("No folders found.\n");
        if (folders) free(folders);
        return folders ? 0 : -1;
    }

    qsort(folders, (size_t)count, sizeof(char *), cmp_str);

    FolderStatus *statuses = fetch_all_folder_statuses(cfg, folders, count);

    if (g_json_out) {
        /* the complete record: every folder with its counters; the tree is
         * the folder names split at "separator" */
        printf("[");
        for (int i = 0; i < count; i++) {
            printf("%s\n  {\"name\": ", i ? "," : "");
            print_json_string(folders[i]);
            printf(", \"separator\": \"%c\", \"unread\": %d, \"flagged\": %d, \"total\": %d}",
                   sep, statuses ? statuses[i].unseen : 0,
                   statuses ? statuses[i].flagged : 0,
                   statuses ? statuses[i].messages : 0);
        }
        printf("\n]\n");
    } else if (tree) {
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

static int change_folder(const Config *cfg, const char *name, int create, int quiet);

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
    enum { VP_HDR_FLAGS = FB_HDR_FLAGS, VP_UNREAD = FB_UNREAD, VP_FLAGGED = FB_FLAGGED,
           VP_JUNK = FB_JUNK, VP_PHISHING = FB_PHISHING, VP_ANSWERED = FB_ANSWERED,
           VP_FORWARDED = FB_FORWARDED, VP_HDR_FOLD = FB_HDR_FOLD, VPREFIX = FB_PREFIX };
    int vf_unread=0, vf_flagged=0, vf_junk=0, vf_phishing=0, vf_answered=0, vf_forwarded=0;
    manifest_count_all_flags(&vf_unread, &vf_flagged, &vf_junk, &vf_phishing,
                             &vf_answered, &vf_forwarded);

    int cursor = VPREFIX, wstart = 0;  /* default: first real folder */
    int tree_mode = ui_pref_get_int("folder_view_mode", 1);
    char current_prefix[512] = "";   /* flat mode: current navigation level */

    /* Pre-position cursor on current_folder (offset by VPREFIX).
     * Virtual folders (__unread__, __flagged__, …) map to their prefix row.
     * Real folders: INBOX is case-insensitive per RFC 3501. */
    if (current_folder && *current_folder) {
        if      (strcmp(current_folder, "__unread__")    == 0) cursor = VP_UNREAD;
        else if (strcmp(current_folder, "__flagged__")   == 0) cursor = VP_FLAGGED;
        else if (strcmp(current_folder, "__junk__")      == 0) cursor = VP_JUNK;
        else if (strcmp(current_folder, "__phishing__")  == 0) cursor = VP_PHISHING;
        else if (strcmp(current_folder, "__answered__")  == 0) cursor = VP_ANSWERED;
        else if (strcmp(current_folder, "__forwarded__") == 0) cursor = VP_FORWARDED;
        else if (tree_mode) {
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
    EmailAction fdefer = EA_NONE;   /* a menu action that prompts runs after a redraw */
    char fmsg[600] = "";            /* the result of the last folder action */

    RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();

    for (;;) {
        int rows  = terminal_rows();
        int limit = (rows > 5) ? rows - 5 : 3;

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

        int tcols_f = terminal_cols();
        if (tcols_f <= 0) tcols_f = 80;
        int trows_d = terminal_rows();
        if (trows_d <= 0) trows_d = 24;

        FolderModel fmodel = { folders, count, sep, statuses, tree_mode, vis,
                               { 0, vf_unread, vf_flagged, vf_junk, vf_phishing,
                                 vf_answered, vf_forwarded, 0 }, cursor,
                               tree_mode ? NULL : current_prefix };
        static const tui_table_col fcols[] = {
            { "Unread",  6, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
            { "Flagged", 7, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
            { "Folder", 20, 1, TUI_ALIGN_LEFT,  0, 0, 0 },
            { "Total",   7, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
        };
        tui_table_model tmodel = { folders_row, folders_cell, &fmodel };
        tui_table tbl;
        tui_table_init(&tbl, fcols, 4, &tmodel);
        tbl.flags |= TUI_TABLE_RULER;
        tui_table_set_count(&tbl, display_count);
        tui_table_select(&tbl, cursor);
        tbl.lv.scroll_top = wstart;

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
            tui_frame_draw_table(cl, &tbl, trows_d, tcols_f, 0, NULL, NULL);
        }

        fflush(stdout);
        {
            int trows_f = terminal_rows();
            if (trows_f <= 0) trows_f = limit + 4;
            int tcols_f = terminal_cols();
            tui_statusline_hint h[6];
            int nh = 0;
            #define HINT(k, l, p) do { h[nh].key = (k); h[nh].label = (l); h[nh].priority = (p); nh++; } while (0)
            int up = !tree_mode && current_prefix[0];
            HINT("\u2191\u2193", "step", 60);
            HINT("PgDn/PgUp", "page", 40);
            HINT("Enter", "open/select", 90);
            HINT(":", "actions", 80);
            HINT("t", up ? "tree" : (tree_mode ? "flat" : "tree"), 50);
            HINT("Esc/q/BS", up ? "up" : "back", SL_PRIO_KEEP);
#undef HINT
            status_line_draw(trows_f, tcols_f, h, nh, "?",
                             display_count > 0 ? cursor + 1 : 0, display_count,
                             fmsg[0] ? fmsg : NULL);
            fmsg[0] = '\0';          /* shown until the next key */
        }

        EmailAction fdeferred = fdefer;
        fdefer = EA_NONE;
        TermKey key = fdeferred ? TERM_KEY_IGNORE : terminal_read_key();
        if (key == TERM_KEY_IGNORE && !fdeferred && terminal_last_printable() == 'q')
            key = TERM_KEY_ESC;      /* q is Esc: back, never quit */
        if (key == TERM_KEY_IGNORE && !fdeferred && terminal_last_printable() == 'j')
            key = TERM_KEY_NEXT_LINE;    /* j and k move (EMAIL-175) */
        if (key == TERM_KEY_IGNORE && !fdeferred && terminal_last_printable() == 'k')
            key = TERM_KEY_PREV_LINE;

        switch (key) {
        case TERM_KEY_QUIT:          /* ctrl-c: out at once, no go_up */
            goto folders_int_done;
        case TERM_KEY_ESC:
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
                if (current_prefix[0] && strcmp(folders[fi], current_prefix) == 0) {
                    /* the folder itself, listed first inside it: open it */
                    selected = strdup(folders[fi]);
                    goto folders_int_done;
                }
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
            int ch = fdeferred ? 0 : terminal_last_printable();
            EmailAction fact = fdeferred;
            if (ch == ':') {
                /* Creating and deleting a folder are in the menu (EMAIL-129);
                 * deleting names the folder and asks, No being the default. */
                const char *tt[EA_COUNT] = { NULL };
                const char *at = NULL;
                if (cursor >= VPREFIX) {
                    if (tree_mode && cursor - VPREFIX < count)
                        at = folders[cursor - VPREFIX];
                    else if (!tree_mode && cursor - VPREFIX < vcount)
                        at = folders[vis[cursor - VPREFIX]];
                }
                tt[EA_FOLDER_DELETE] = at ? at : "(no folder selected)";
                fact = ekeys_menu(EKV_FOLDERS, tt);
                if (fact == EA_FOLDER_DELETE && !at) {
                    snprintf(fmsg, sizeof(fmsg), "Select a folder to delete");
                    fact = EA_NONE;
                }
                if (fact == EA_FOLDER_CREATE) {
                    fdefer = fact;      /* draw the folders, then ask for the name */
                    fact = EA_NONE;
                }
                if (fact == EA_FOLDER_DELETE) {
                    char dname[512];
                    snprintf(dname, sizeof(dname), "%s", at);
                    if (change_folder(cfg, dname, 0, 1) == 0)
                        snprintf(fmsg, sizeof(fmsg), "Folder %s deleted", dname);
                    else
                        snprintf(fmsg, sizeof(fmsg), "Could not delete %s", dname);
                    goto folders_reload;
                }
            } else if (fact == EA_FOLDER_CREATE) {
                char nname[512] = "";
                if (!tree_mode && current_prefix[0])
                    snprintf(nname, sizeof(nname), "%.500s%c", current_prefix, sep);
                InputLine il;
                input_line_init(&il, nname, sizeof(nname), nname);
                int trw = terminal_rows();
                if (trw <= 0) trw = 24;
                int okn = input_line_run(&il, trw - 2, "New folder name: ");
                if (okn && nname[0]) {
                    if (change_folder(cfg, nname, 1, 1) == 0)
                        snprintf(fmsg, sizeof(fmsg), "Folder %s created", nname);
                    else
                        snprintf(fmsg, sizeof(fmsg), "Could not create %s", nname);
                    goto folders_reload;
                }
            } else if (ch == '/') {
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
            } else if (ch == '?') {
                ekeys_help(EKV_FOLDERS, "Folder browser shortcuts");
            }
            break;
        }
        }
        continue;

    folders_reload: {
        /* A folder was created or deleted: read the list again (the cache
         * was refreshed by the change) and the counters of every folder. */
        free(statuses);
        free(vis);
        for (int i = 0; i < count; i++) free(folders[i]);
        free(folders);
        statuses = NULL;
        vis = NULL;
        count = 0;
        folders = fetch_folder_list(cfg, &count, &sep);
        if (!folders || count == 0) {
            free(folders);
            return NULL;
        }
        qsort(folders, (size_t)count, sizeof(char *), cmp_str);
        statuses = fetch_all_folder_statuses(cfg, folders, count);
        vis = malloc((size_t)count * sizeof(int));
        if (!vis) {
            free(statuses);
            for (int i = 0; i < count; i++) free(folders[i]);
            free(folders);
            return NULL;
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
 * Applies changes via emc_set_flag and updates local .idx.
 * Returns when the user presses ESC/Backspace/q.
 */
/** Add (@p adding = 1) or remove label @p lid on message @p uid: the local
 *  index, the .hdr and, with a client, the server.  Adding a real label (not
 *  UNREAD/STARRED/CATEGORY_) implicitly moves the message out of the virtual
 *  archive (_nolabel) and out of the trash. */
static void apply_label_change(emc_client *mc, const char *uid, const char *lid,
                               int adding) {
    if (adding) {
        label_idx_add(lid, uid);
        local_hdr_update_labels("", uid, &lid, 1, NULL, 0);
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
                if (mc) emc_modify_label(mc, uid, "TRASH", 0);
            }
        }
    } else {
        label_idx_remove(lid, uid);
        local_hdr_update_labels("", uid, NULL, 0, &lid, 1);
    }
    if (mc) {
        if (strcmp(lid, "STARRED") == 0)
            emc_set_flag(mc, uid, "\\Flagged", adding);
        else
            emc_modify_label(mc, uid, lid, adding);
    }
}

static void show_label_picker(emc_client *mc, const char *uid,
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

    /* The labels as a checklist of libtuikit (EMAIL-110): Space ticks, Enter
     * applies what was ticked, Esc leaves everything as it was. */
    unsigned char *ticked = calloc((size_t)pick_count, 1);
    if (ticked) {
        for (int i = 0; i < pick_count; i++) ticked[i] = pick_on[i] ? 1 : 0;
        tui_dialog dlg;
        if (tui_dialog_checklist(&dlg, "Toggle Labels", (const char *const *)pick_names,
                                 pick_count, ticked, 0) == 0) {
            if (tui_frame_run_dialog(&dlg) == TUI_DIALOG_OK) {
                for (int i = 0; i < pick_count; i++) {
                    int now = tui_dialog_is_checked(&dlg, i);
                    if (now == (pick_on[i] ? 1 : 0)) continue;
                    pick_on[i] = now;
                    apply_label_change(mc, uid, pick_ids[i], now);
                }
            }
            tui_dialog_free(&dlg);
        }
        free(ticked);
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

/* Model of the Gmail label browser's table: rows are labels, section
 * headings (group headers) and rules (vrow[i] == -1). */
typedef struct {
    char **ids;
    char **names;
    const int *hdr;
    const int *vrow;
} LabelModel;

static void labels_row(void *ctx, int i, tui_table_row *r) {
    const LabelModel *m = ctx;
    int li = m->vrow[i];
    if (li < 0) {
        tui_table_row_kind(r, TUI_ROW_SECTION);
        tui_table_row_text(r, "");
        tui_table_row_style(r, tui_style_attrs(TUI_ATTR_DIM));
    } else if (m->hdr[li]) {
        tui_table_row_kind(r, TUI_ROW_SECTION);
        tui_table_row_text(r, m->names[li]);
        tui_table_row_style(r, tui_style_attrs(TUI_ATTR_DIM));
    }
}

static void labels_cell(void *ctx, int i, int col, tui_table_cell *c) {
    const LabelModel *m = ctx;
    int li = m->vrow[i];
    char buf[16];
    if (col == 0) {
        fmt_thou(buf, sizeof(buf), label_idx_count(m->ids[li]));
        tui_table_cell_put(c, buf, TUI_STYLE_DEFAULT);
    } else {
        tui_table_cell_put(c, m->names[li], TUI_STYLE_DEFAULT);
    }
}

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

/** Asks for a label name and creates the label.  Returns 1 when it was
 *  created; a failure is put in @p flash for the status line (EMAIL-162). */
static int label_create_prompt(const Config *cfg, int prompt_row,
                               char *flash, size_t flash_sz) {
    char new_name[256] = "";
    InputLine il;
    input_line_init(&il, new_name, sizeof(new_name), "");
    int confirmed = input_line_run(&il, prompt_row, "New label name: ");
    if (!confirmed || !new_name[0]) return 0;
    if (email_service_create_label(cfg, new_name) == 0) return 1;
    snprintf(flash, flash_sz, "  Could not create the label %s.", new_name);
    return 0;
}

char *email_service_list_labels_interactive(const Config *cfg,
                                            const char *current_label,
                                            int *go_up) {
    local_store_init(cfg->host, cfg->user);
    if (go_up) *go_up = 0;
    EmailAction lbl_defer = EA_NONE;   /* a menu action that prompts runs after a redraw */

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
            printf(TUI_HDR_ON "%s", _cl);
            int _used = visible_line_cols(_cl, _cl + strlen(_cl));
            for (int _p = _used; _p < _tc; _p++) putchar(' ');
            printf("\033[0m\n\n");
        }
        printf("  No labels synced yet. Run 'email-sync' to populate.\n");
        fflush(stdout);
        {
            tui_statusline_hint h[1] = {{ "Esc/q/BS", "back", SL_PRIO_KEEP }};
            status_line_draw(_tr, _tc, h, 1, NULL, 0, 0, NULL);
        }
        for (;;) {
            TermKey _k = terminal_read_key();
            if (_k == TERM_KEY_BACK || _k == TERM_KEY_ESC ||
                (_k == TERM_KEY_IGNORE && terminal_last_printable() == 'q')) {
                if (go_up) *go_up = 1;
                return NULL;
            }
            if (_k == TERM_KEY_QUIT) return NULL;
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
    char lbl_flash[300] = "";

    for (;;) {
        int trows = terminal_rows();
        int tcols = terminal_cols();
        if (trows <= 0) trows = 24;
        if (tcols <= 0) tcols = 80;
        /* Name column width: "  " + 6(count) + "  " + name_w = name_w + 10 */
        int name_w = tcols - 12;
        if (name_w < 20) name_w = 20;

        /* Fixed overhead: 1 title + 1 blank + 1 col-header + 1 separator + 1 statusbar = 5.
         * Remaining rows are shared between items and section-separator lines. */
        int avail = (trows > 6) ? trows - 5 : 5;

        if (cursor >= lbl_count) cursor = lbl_count - 1;
        if (cursor < 0) cursor = 0;
        /* Never land on a section header: header rows are drawn without the
         * selection highlight, so a cursor parked there would be invisible. */
        while (cursor < lbl_count - 1 && lbl_hdr[cursor]) cursor++;
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

        /* The table's rows: every label, a section heading for a group header
         * and a rule where a group starts after other labels. */
        int *vrow = malloc(((size_t)lbl_count * 2 + 1) * sizeof(int));
        int nvrow = 0, cursor_vrow = 0;
        for (int i = 0; vrow && i < lbl_count; i++) {
            if (!lbl_hdr[i] && lbl_seps[i] && nvrow > 0 &&
                !(vrow[nvrow - 1] >= 0 && lbl_hdr[vrow[nvrow - 1]]))
                vrow[nvrow++] = -1;
            if (i == cursor) cursor_vrow = nvrow;
            vrow[nvrow++] = i;
        }
        LabelModel lmod = { lbl_ids, lbl_names, lbl_hdr, vrow };
        static const tui_table_col lcols[] = {
            { "Count",  6, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
            { "Label", 20, 1, TUI_ALIGN_LEFT,  0, 0, 0 },
        };
        tui_table_model lt = { labels_row, labels_cell, &lmod };
        tui_table ltb;
        tui_table_init(&ltb, lcols, 2, &lt);
        ltb.flags |= TUI_TABLE_RULER;
        tui_table_set_count(&ltb, nvrow);
        tui_table_select(&ltb, cursor_vrow);
        {
            char cl[512];
            snprintf(cl, sizeof(cl), "  Labels \u2014 %s  (%d)",
                     cfg->user ? cfg->user : "?", lbl_count);
            tui_frame_draw_table(cl, &ltb, trows, tcols, 0, NULL, NULL);
        }
        free(vrow);
        fflush(stdout);

        {
            tui_statusline_hint h[6];
            int nh = 0;
            #define HINT(k, l, p) do { h[nh].key = (k); h[nh].label = (l); h[nh].priority = (p); nh++; } while (0)
            HINT("j/k", "select", 60);
            HINT("Enter", "open", 90);
            HINT(":", "actions", 80);
            HINT("Esc/q/BS", "accounts", SL_PRIO_KEEP);
#undef HINT
            status_line_draw(trows, tcols, h, nh, "?", cursor + 1, lbl_count,
                             lbl_flash[0] ? lbl_flash : NULL);
        }
        lbl_flash[0] = '\0';           /* the notice lives until the next key */

        EmailAction lbl_deferred = lbl_defer;
        lbl_defer = EA_NONE;
        TermKey key = lbl_deferred ? TERM_KEY_IGNORE : terminal_read_key();
        fprintf(stderr, "\r\033[K"); fflush(stderr);
        if (key == TERM_KEY_IGNORE && !lbl_deferred && terminal_last_printable() == 'q')
            key = TERM_KEY_ESC;      /* q is Esc: back, never quit */
        if (key == TERM_KEY_IGNORE && !lbl_deferred && terminal_last_printable() == 'j')
            key = TERM_KEY_NEXT_LINE;    /* j and k move (EMAIL-175) */
        if (key == TERM_KEY_IGNORE && terminal_last_printable() == 'k')
            key = TERM_KEY_PREV_LINE;

        switch (key) {
        case TERM_KEY_BACK:
        case TERM_KEY_ESC:
            if (go_up) *go_up = 1;
            goto labels_done;
        case TERM_KEY_QUIT:          /* ctrl-c: out at once */
            goto labels_done;
        case TERM_KEY_HOME:
            cursor = 0; wstart = 0;
            while (cursor < lbl_count - 1 && lbl_hdr[cursor]) cursor++;
            break;
        case TERM_KEY_END:
            cursor = lbl_count > 0 ? lbl_count - 1 : 0;
            while (cursor > 0 && lbl_hdr[cursor]) cursor--;
            break;
        case TERM_KEY_NEXT_LINE: {
            /* Move to the next selectable row; stay put if there is none. */
            int p = cursor + 1;
            while (p < lbl_count && lbl_hdr[p]) p++;
            if (p < lbl_count) cursor = p;
            break;
        }
        case TERM_KEY_PREV_LINE: {
            /* Move to the previous selectable row; stay put if there is none —
             * without this the cursor lands on the leading section header. */
            int p = cursor - 1;
            while (p >= 0 && lbl_hdr[p]) p--;
            if (p >= 0) cursor = p;
            break;
        }
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
            EmailAction lact = EA_NONE;
            if (lbl_deferred) { ch = 0; lact = lbl_deferred; }
            if (ch == '?')
                ekeys_help(EKV_LABELS, "Label browser shortcuts");
            if (ch == ':') {
                /* Creating and deleting a label are in the menu (EMAIL-175);
                 * deleting names the label and asks, No being the default. */
                const char *tt[EA_COUNT] = { NULL };
                if (lbl_count > 0 && !lbl_hdr[cursor]) tt[EA_LABEL_DELETE] = lbl_names[cursor];
                lact = ekeys_menu(EKV_LABELS, tt);
                if (lact == EA_LABEL_CREATE) {
                    lbl_defer = lact;       /* draw the labels, then ask for the name */
                    lact = EA_NONE;
                }
            }
            if (lact == EA_LABEL_CREATE) {
                /* Create new label */
                {
                    if (label_create_prompt(cfg, trows - 2, lbl_flash, sizeof(lbl_flash))) {
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
            if (lact == EA_LABEL_DELETE && lbl_count > 0 && !lbl_hdr[cursor]) {
                /* Gmail deletes a label by its id, which differs from the
                 * name shown for a user label (EMAIL-135). */
                const char *del_id = lbl_ids[cursor];
                if (del_id) {
                    if (email_service_delete_label(cfg, del_id) != 0)
                        snprintf(lbl_flash, sizeof(lbl_flash),
                                 "  Could not delete the label %s.",
                                 lbl_names[cursor]);
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

/* The Server and Send-via texts of an account row.  *smtp_configured is 0
 * when an IMAP account has no SMTP host (the cell is shown dim). */
static void account_server_texts(const Config *cfg, char *server, size_t server_sz,
                                 char *smtp, size_t smtp_sz, int *smtp_configured) {
    /* Server: Gmail shows "Gmail API", IMAP shows host:port */
    if (cfg->gmail_mode) {
        snprintf(server, server_sz, "Gmail API");
        snprintf(smtp, smtp_sz, "Gmail API");
        *smtp_configured = 1;
        return;
    }
    fmt_url_with_port(cfg->host, 993, server, server_sz);
    *smtp_configured = cfg->smtp_host && cfg->smtp_host[0];
    if (!*smtp_configured) {
        snprintf(smtp, smtp_sz, "\u2014");
    } else if (cfg->smtp_port) {
        const char *proto_end = strstr(cfg->smtp_host, "://");
        const char *smtp_host_part = proto_end ? proto_end + 3 : cfg->smtp_host;
        if (strchr(smtp_host_part, ':'))
            snprintf(smtp, smtp_sz, "%s", cfg->smtp_host);
        else
            snprintf(smtp, smtp_sz, "%s:%d", cfg->smtp_host, cfg->smtp_port);
    } else {
        int defport = (strncmp(cfg->smtp_host, "smtps://", 8) == 0) ? 465 : 587;
        fmt_url_with_port(cfg->smtp_host, defport, smtp, smtp_sz);
    }
}

/* Model of the accounts table (tui_table): the rows are the accounts. */
typedef struct {
    const AccountEntry *accounts;
    const int          *unseen;
    const int          *flagged;
} AccountsModel;

static void accounts_cell(void *ctx, int i, int col, tui_table_cell *cell) {
    const AccountsModel *m = ctx;
    const Config *cfg = m->accounts[i].cfg;
    char buf[256], server[256], smtp[256];
    int smtp_configured;
    account_server_texts(cfg, server, sizeof(server), smtp, sizeof(smtp),
                         &smtp_configured);
    switch (col) {
    case 0: fmt_thou(buf, sizeof(buf), m->unseen[i]);  break;
    case 1: fmt_thou(buf, sizeof(buf), m->flagged[i]); break;
    case 2: snprintf(buf, sizeof(buf), "%s", cfg->user ? cfg->user : "(unknown)"); break;
    case 3: snprintf(buf, sizeof(buf), "%s", cfg->gmail_mode ? "Gmail" : "IMAP"); break;
    case 4: snprintf(buf, sizeof(buf), "%s", server); break;
    default: snprintf(buf, sizeof(buf), "%s", smtp);
        tui_table_cell_put(cell, buf,
            smtp_configured ? TUI_STYLE_DEFAULT : tui_style_attrs(TUI_ATTR_DIM));
        return;
    }
    tui_table_cell_put(cell, buf, TUI_STYLE_DEFAULT);
}

int email_service_account_interactive(Config **cfg_out, int *cursor_inout,
                                      const char *flash_msg) {
    *cfg_out = NULL;
    RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();
    (void)tui_raw;

    int cursor = (cursor_inout && *cursor_inout > 0) ? *cursor_inout : 0;
    char flash_note[600] = "";

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

        static const tui_table_col acc_cols[] = {
            { "Unread",   6, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
            { "Flagged",  7, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
            { "Account", 32, 0, TUI_ALIGN_LEFT,  0, 0, 0 },
            { "Type",     5, 0, TUI_ALIGN_LEFT,  0, 0, 0 },
            /* host:port is cut at the start, so the port stays visible */
            { "Server",  10, 1, TUI_ALIGN_LEFT,  0, 0, TUI_COL_CUT_START },
            { "Send via", 8, 1, TUI_ALIGN_LEFT,  0, 0, TUI_COL_CUT_START },
        };
        AccountsModel am = { accounts, acc_unseen, acc_flagged };
        tui_table_model model = { NULL, accounts_cell, &am };
        tui_table tbl;
        tui_table_init(&tbl, acc_cols, 6, &model);
        tbl.flags |= TUI_TABLE_RULER;
        tui_table_set_count(&tbl, count);
        tui_table_select(&tbl, cursor);
        if (count == 0)
            tui_table_set_message(&tbl, "No accounts configured.", TUI_STYLE_DEFAULT);

        char cl[128];
        snprintf(cl, sizeof(cl), "  Email Accounts (%d)", count);
        tui_frame_draw_table(cl, &tbl, trows, tcols, 0, NULL, NULL);

        {
            tui_statusline_hint h[8];
            int nh = 0;
            #define HINT(k, l, p) do { h[nh].key = (k); h[nh].label = (l); h[nh].priority = (p); nh++; } while (0)
            HINT("j/k", "select", 60);
            HINT("Enter", "open", 90);
            HINT("n", "add", 50);
            HINT("d", "delete", 45);
            HINT("i", "IMAP", 40);
            HINT("e", "SMTP", 40);
            if (count > 0 && accounts[cursor].cfg->gmail_mode)
                HINT(":", "actions", 40);
            HINT("ESC", "quit", SL_PRIO_KEEP);
#undef HINT
            status_line_draw(trows, tcols, h, nh, "?", count > 0 ? cursor + 1 : 0,
                             count, flash_msg);
        }

        TermKey key = terminal_read_key();
        fprintf(stderr, "\r\033[K"); fflush(stderr);
        if (key != TERM_KEY_IGNORE || terminal_last_printable())
            flash_msg = NULL;          /* the notice lives until the next key */

        int ch = terminal_last_printable();
        if (key == TERM_KEY_IGNORE && ch == 'j') key = TERM_KEY_NEXT_LINE;  /* EMAIL-175 */
        if (key == TERM_KEY_IGNORE && ch == 'k') key = TERM_KEY_PREV_LINE;

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
        if (key == TERM_KEY_NEXT_LINE) {
            if (cursor < count - 1) cursor++;
            ACC_FREE(); continue;
        }
        if (key == TERM_KEY_PREV_LINE) {
            if (cursor > 0) cursor--;
            ACC_FREE(); continue;
        }
        /* EMAIL-159: PgDn/PgUp move by the visible rows, as in the other lists */
        if (key == TERM_KEY_NEXT_PAGE) {
            int page = (trows > 5) ? trows - 5 : 3;
            cursor += page;
            if (cursor > count - 1) cursor = count > 0 ? count - 1 : 0;
            ACC_FREE(); continue;
        }
        if (key == TERM_KEY_PREV_PAGE) {
            int page = (trows > 5) ? trows - 5 : 3;
            cursor -= page;
            if (cursor < 0) cursor = 0;
            ACC_FREE(); continue;
        }
        if (key == TERM_KEY_ENTER && count > 0) {
            if (cursor_inout) *cursor_inout = cursor;
            *cfg_out = accounts[cursor].cfg;
            accounts[cursor].cfg = NULL; /* transfer ownership */
            ACC_FREE(); return 1;
        }

        /* Printable keys */
        if (ch == '?') {
            static const char *help[][2] = {
                { "j / k",           "Move cursor down / up (also the arrows)" },
                { "PgUp / PgDn",      "Move cursor up / down"       },
                { "Home / End",       "First / last account"        },
                { "Enter",            "Open selected account"       },
                { "n",               "Add new account"             },
                { "d",               "Delete selected account"     },
                { "i",               "Edit IMAP for account"       },
                { "e",               "Edit SMTP for account"       },
                { "Esc",             "Quit"                        },
                { "?",               "Show this help"              },
                { ":",               "Actions of a Gmail account"  },
            };
            int nhelp = (int)(sizeof(help)/sizeof(help[0]));
            if (!(count > 0 && accounts[cursor].cfg->gmail_mode)) nhelp--;
            show_help_popup("Accounts shortcuts", help, nhelp);
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
        if (ch == ':' && count > 0) {
            if (!accounts[cursor].cfg->gmail_mode) {
                snprintf(flash_note, sizeof(flash_note),
                         "  No actions for an IMAP account here (i, e edit its settings).");
                flash_msg = flash_note;
                ACC_FREE(); continue;
            }
            const char *tt[EA_COUNT] = { NULL };
            if (ekeys_menu(EKV_ACCOUNTS_GMAIL, tt) == EA_REAUTH) {
                if (cursor_inout) *cursor_inout = cursor;
                *cfg_out = accounts[cursor].cfg;
                accounts[cursor].cfg = NULL;
                ACC_FREE(); return 5;  /* caller renews the authorization */
            }
            ACC_FREE(); continue;
        }
        if (ch == 'n') {
            ACC_FREE(); return 3;  /* caller runs setup wizard */
        }
        if (ch == 'd' && count > 0) {
            const char *name = accounts[cursor].name;

            /* Deleting an account takes a confirmation: y or Y deletes, any
             * other key keeps it. */
            {
                char ask[512];
                snprintf(ask, sizeof(ask), "  Delete account %s? (y/N)",
                         name ? name : "?");
                print_infoline(trows, tcols, ask);
                TermKey ck = terminal_read_key();
                int cch = terminal_last_printable();
                fprintf(stderr, "\r\033[K"); fflush(stderr);
                if (!(ck == TERM_KEY_IGNORE && (cch == 'y' || cch == 'Y'))) {
                    snprintf(flash_note, sizeof(flash_note),
                             "  Account %s kept.", name ? name : "?");
                    flash_msg = flash_note;
                    ACC_FREE();
                    continue;
                }
            }

            /* Compute local data directory (NOT deleted) */
            const char *data_base = platform_data_dir();
            char data_path[2048] = "";
            if (data_base && name && name[0])
                snprintf(data_path, sizeof(data_path),
                         "%s/email-cli/accounts/%s", data_base, name);

            config_delete_account(name);
            ACC_FREE();
            if (cursor > 0) cursor--;

            /* Preservation notice: the next redraw owns the last row, so the
             * notice goes out as the status line message (EMAIL-141). */
            if (data_path[0]) {
                snprintf(flash_note, sizeof(flash_note),
                         "  Account removed. Local messages preserved: %.500s", data_path);
                flash_msg = flash_note;
            }
            continue;  /* re-render */
        }

        ACC_FREE();
    }
#undef ACC_FREE
}

/* Load one message: local store first, server on a miss.
 * Returns a heap-allocated RFC 2822 message, or NULL (error already reported). */
/* Locate the folder holding UID when the caller named none.
 *
 * IMAP UIDs are unique per mailbox, not per account, so the same UID routinely
 * names a different message in every folder — an unqualified UID is genuinely
 * under-specified.  "prefer" (the configured folder) is the user's implicit
 * context and wins when it holds a copy, but the other candidates are named on
 * stderr so the choice is never silent: picking one quietly is what made a
 * search hit in one folder open a different message from another.
 *
 * Returns a heap-allocated folder name, or NULL.  *fatal is set when the
 * caller must stop: the UID is ambiguous and no copy sits in "prefer". */
static char *resolve_folder_for_uid(const char *uid, const char *prefer,
                                    int *fatal) {
    *fatal = 0;
    char **folders = NULL;
    int n = 0;
    local_msg_find_folders(uid, &folders, &n);

    if (n == 1) {
        char *only = folders[0];
        folders[0] = NULL;
        local_folder_list_free(folders, n);
        return only;
    }
    if (n > 1) {
        int pick = -1;
        if (prefer && prefer[0])
            for (int i = 0; i < n; i++)
                if (strcasecmp(folders[i], prefer) == 0) { pick = i; break; }

        fprintf(stderr, "%s: UID %s exists in %d folders:",
                pick >= 0 ? "Warning" : "Error", uid, n);
        for (int i = 0; i < n; i++)
            fprintf(stderr, "%s %s", i ? "," : "", folders[i]);
        if (pick >= 0)
            fprintf(stderr, "\n         Showing the copy in %s; "
                            "use --folder <name> to pick another.\n",
                    folders[pick]);
        else
            fprintf(stderr, "\n       Re-run with --folder <name> "
                            "to say which one.\n");

        char *chosen = NULL;
        if (pick >= 0) { chosen = folders[pick]; folders[pick] = NULL; }
        else           { *fatal = 1; }
        local_folder_list_free(folders, n);
        return chosen;
    }
    local_folder_list_free(folders, n);
    return NULL;
}

static char *load_message(const Config *cfg, const char *folder, const char *uid,
                          char **used_folder_out) {
    char *raw = NULL;
    RAII_STRING char *located = NULL;
    /* Kept for diagnostics: Gmail's storage folder is the empty string, which
     * would read as "in folder ''" in an error message. */
    const char *named_folder = (folder && folder[0]) ? folder : "INBOX";

    if (cfg && cfg->gmail_mode) {
        /* Gmail keeps one copy of each message in a flat store keyed by UID
         * alone — a folder there is a label, not a location.  Resolving a
         * folder from the manifests cannot work (gmail_sync writes none), and
         * asking for the message under a label name looks up a path that was
         * never written: in cron mode that produced "Could not load message"
         * for messages sitting in the store. */
        folder = "";
    } else if (!folder || !folder[0] || is_virtual_folder_name(folder)) {
        /* No folder given, or a virtual view that stores nothing: resolve it. */
        int fatal = 0;
        located = resolve_folder_for_uid(uid, cfg ? cfg->folder : NULL, &fatal);
        if (fatal) return NULL;
        if (located) folder = located;
        else         folder = cfg->folder;   /* not cached — try the default */
        named_folder = (folder && folder[0]) ? folder : "INBOX";
    }

    if (local_msg_exists(folder, uid)) {
        logger_log(LOG_DEBUG, "Cache hit for UID %s in %s", uid, folder);
        raw = local_msg_load(folder, uid);
    } else if (cfg->sync_interval > 0) {
        /* cron/offline mode: serve only from local cache; do not connect */
        fprintf(stderr, "Could not load message UID %s in folder '%s'.\n",
                uid, named_folder);
        return NULL;
    } else {
        raw = fetch_uid_content_in(cfg, folder, uid, 0);
        if (raw) {
            local_msg_save(folder, uid, raw, strlen(raw));
            local_index_update(folder, uid, raw);
        }
    }

    if (!raw)
        fprintf(stderr, "Could not load message UID %s in folder '%s'.\n",
                uid, named_folder);
    else if (used_folder_out)
        *used_folder_out = strdup(folder);
    return raw;
}

/* Report a charset problem once, on stderr, so the caller sees that the text
 * below is not what the sender wrote.  The core layer records the fact; saying
 * it out loud belongs here, in the layer that owns the user interaction. */
static void warn_charset(const MimeTextInfo *info, const char *uid) {
    if (!info) return;
    if (info->unknown_charset)
        fprintf(stderr,
                "Warning: UID %s declares charset '%s', which iconv does not "
                "know — showing undecoded bytes.\n",
                uid ? uid : "?", info->declared_charset);
    else if (info->invalid_sequence)
        fprintf(stderr,
                "Warning: UID %s is not valid %s — conversion failed, showing "
                "undecoded bytes.\n",
                uid ? uid : "?", info->declared_charset);
}

int email_service_read_raw(const Config *cfg, const char *folder, const char *uid) {
    char *raw = load_message(cfg, folder, uid, NULL);
    if (!raw) return -1;

    /* Write the message exactly as stored: headers and body, no MIME parsing,
     * no charset conversion, no rendering.  This is the view needed to debug
     * encoding problems, where the point is what the sender actually sent. */
    fwrite(raw, 1, strlen(raw), stdout);
    if (strlen(raw) > 0 && raw[strlen(raw) - 1] != '\n') putchar('\n');
    fflush(stdout);

    free(raw);
    return 0;
}

int email_service_read(const Config *cfg, const char *folder, const char *uid, int pager, int page_size) {
    /* load_message resolves the folder when none was given; take its answer so
     * that everything below (the File: line, flag updates) refers to the
     * mailbox the message actually came from. */
    RAII_STRING char *used_folder = NULL;
    char *raw = load_message(cfg, folder, uid, &used_folder);
    if (!raw) return -1;
    if (used_folder) folder = used_folder;

    char *from_raw = mime_get_header(raw, "From");
    char *from     = from_raw ? mime_decode_words(from_raw) : NULL;
    free(from_raw);
    char *subj_raw = mime_get_header(raw, "Subject");
    char *subject  = subj_raw ? mime_decode_words(subj_raw) : NULL;
    free(subj_raw);
    char *date_raw = mime_get_header(raw, "Date");
    char *date     = date_raw ? mime_format_date(date_raw) : NULL;
    free(date_raw);
    char *ro_labels  = cfg->gmail_mode ? local_hdr_get_labels("", uid) : NULL;
    char *ro_path    = local_msg_path(folder, uid);
    char *ro_ar      = mime_get_header(raw, "Authentication-Results");
    char *ro_dmarc   = mime_describe_dmarc(ro_ar);
    free(ro_ar);
    char *ro_to_raw  = mime_get_header(raw, "To");
    char *ro_to      = ro_to_raw ? mime_decode_words(ro_to_raw) : NULL;
    free(ro_to_raw);
    /* Build attachment summary */
    int ro_att_count = 0;
    MimeAttachment *ro_atts = mime_list_attachments(raw, &ro_att_count);
    char ro_attach_buf[256] = "";
    if (ro_att_count > 0) {
        int pos = snprintf(ro_attach_buf, sizeof(ro_attach_buf),
                           "%d file%s: ", ro_att_count, ro_att_count == 1 ? "" : "s");
        for (int _i = 0; _i < ro_att_count && pos < (int)sizeof(ro_attach_buf) - 2; _i++) {
            const char *fn = ro_atts[_i].filename ? ro_atts[_i].filename : "?";
            if (_i > 0) { ro_attach_buf[pos++] = ','; ro_attach_buf[pos++] = ' '; ro_attach_buf[pos] = '\0'; }
            if (pos + (int)strlen(fn) > SHOW_WIDTH - 12) {
                snprintf(ro_attach_buf + pos, sizeof(ro_attach_buf) - (size_t)pos,
                         "(+%d more)", ro_att_count - _i);
                break;
            }
            size_t av = sizeof(ro_attach_buf) - (size_t)pos - 1;
            strncpy(ro_attach_buf + pos, fn, av);
            ro_attach_buf[sizeof(ro_attach_buf) - 1] = '\0';
            pos += (int)strlen(fn);
        }
    }
    mime_free_attachments(ro_atts, ro_att_count);

    if (!g_json_out)
        print_show_headers(from, ro_to, subject, date, uid, ro_labels, ro_path, ro_dmarc, ro_attach_buf);

    int term_cols_show = pager ? terminal_cols() : SHOW_WIDTH;
    int wrap_cols = term_cols_show > SHOW_WIDTH ? SHOW_WIDTH : term_cols_show;

    char *body = NULL;
    char *html_raw = mime_get_html_part(raw);
    if (html_raw) {
        body = html_render(html_raw, wrap_cols, pager ? 1 : 0);
        free(html_raw);
    }
    if (!body) {
        MimeTextInfo tinfo;
        char *plain = mime_get_text_body_ex(raw, &tinfo);
        if (plain) {
            warn_charset(&tinfo, uid);
            body = word_wrap(plain, wrap_cols);
            if (!body) body = plain;
            else free(plain);
        }
    }
    const char *body_text = body ? body : "(no readable text body)";

    if (g_json_out) {
        /* the complete record of the message: headers, attachment names, body */
        printf("{\"uid\": ");     print_json_string(uid);
        printf(",\n \"folder\": "); print_json_string(folder);
        printf(",\n \"from\": ");   print_json_string(from);
        printf(",\n \"to\": ");     print_json_string(ro_to);
        printf(",\n \"subject\": "); print_json_string(subject);
        printf(",\n \"date\": ");   print_json_string(date);
        printf(",\n \"labels\": "); print_json_string(ro_labels);
        printf(",\n \"dmarc\": ");  print_json_string(ro_dmarc);
        int jn = 0;
        MimeAttachment *jat = mime_list_attachments(raw, &jn);
        printf(",\n \"attachments\": [");
        for (int i = 0; i < jn; i++) {
            printf("%s", i ? ", " : "");
            print_json_string(jat[i].filename);
        }
        mime_free_attachments(jat, jn);
        printf("],\n \"body\": ");
        print_json_string(body ? body : "");
        printf("}\n");
        free(ro_path); free(ro_dmarc); free(ro_to);
        free(body); free(from); free(subject); free(date); free(ro_labels); free(raw);
        return 0;
    }

#define SHOW_HDR_LINES 8
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
                print_show_headers(from, ro_to, subject, date, uid, ro_labels, ro_path, ro_dmarc, ro_attach_buf);
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

    free(ro_path); free(ro_dmarc); free(ro_to);
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
        emc_client *gc = mail_connect(cfg);
        if (!gc) {
            fprintf(stderr, "sync: could not connect to Gmail API.\n");
            if (pid_path[0]) unlink(pid_path);
            return -1;
        }
        int rc = emc_sync(gc, force_reconcile) == EMC_OK ? 0 : -1;
        emc_close(gc);
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
            EMC_AUTO_CLOSE emc_client *append_mc = make_mail(cfg);
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
                    if (emc_append(append_mc, pa[i].folder, append_msg, append_len) == 0) {
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
    EMC_AUTO_CLOSE emc_client *sync_mc = make_mail(cfg);
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

        emc_select_result sel = {0};
        if (emc_select_ext(sync_mc, folder,
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
        if (emc_search(sync_mc, EMC_SEARCH_ALL, &uids, &uid_count) != 0) {
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
                    emc_move_to_folder(sync_mc,
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
                    emc_set_flag(sync_mc, pending[pi].uid,
                                        pending[pi].flag_name, pending[pi].add);
                local_pending_flag_clear(folder);
            }
            free(pending);
        }

        /* Evict deleted messages from manifest */
        manifest_retain(manifest, (const char (*)[17])uids, uid_count);

        /* ── Flag acquisition ─────────────────────────────────────────── */
        emc_flag_update *change_updates = NULL;
        int             change_count   = 0;

        char (*unseen_uids)[17]  = NULL; int unseen_count  = 0;
        char (*flagged_uids)[17] = NULL; int flagged_count = 0;
        char (*done_uids)[17]    = NULL; int done_count    = 0;

        if (incremental) {
            /* CONDSTORE path: CHANGEDSINCE replaces three SEARCH commands */
            emc_fetch_flags_changedsince(sync_mc,
                                                 saved_state.highestmodseq,
                                                 &change_updates, &change_count);
            /* Apply flag updates to existing manifest entries now */
            for (int ui = 0; ui < change_count; ui++) {
                ManifestEntry *me = manifest_find(manifest, change_updates[ui].uid);
                if (me) me->flags = change_updates[ui].flags;
            }
        } else {
            /* Full path: three SEARCH commands */
            if (emc_search(sync_mc, EMC_SEARCH_UNREAD,
                                   &unseen_uids, &unseen_count) != 0)
                unseen_count = 0;
            emc_search(sync_mc, EMC_SEARCH_FLAGGED,
                               &flagged_uids, &flagged_count);
            emc_search(sync_mc, EMC_SEARCH_DONE,
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
                emc_set_progress(sync_mc, sync_progress_cb, &pctx);
                char *raw = emc_fetch_body(sync_mc, uid);
                emc_set_progress(sync_mc, NULL, NULL);
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
        {
            emc_account ra;
            mail_account_from_config(accounts[i].cfg, &ra);
            if (emc_rebuild_indexes(&ra) != EMC_OK)
                errors++;
        }
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
static void print_rule_matches(FILE *out, const MailRules *rules,
                                const char *from, const char *subject,
                                const char *to, const char *labels,
                                const char *uid, int dry_run) {
    for (int r = 0; r < rules->count; r++) {
        if (!mail_rule_matches(&rules->rules[r], from, subject, to, labels,
                               NULL, (time_t)0))
            continue;
        const MailRule *mr = &rules->rules[r];
        fprintf(out, "  %s \"%s\" \xe2\x86\x92 uid:%s",
               dry_run ? "[dry-run]" : "[rule]",
               mr->name ? mr->name : "?", uid);
        for (int j = 0; j < mr->then_add_count; j++)
            fprintf(out, "  +%s", mr->then_add_label[j]);
        for (int j = 0; j < mr->then_rm_count; j++)
            fprintf(out, "  -%s", mr->then_rm_label[j]);
        if (mr->then_move_folder)
            fprintf(out, "  \xe2\x86\x92%s", mr->then_move_folder);
        fprintf(out, "\n");
    }
}

int email_service_apply_rules_to(FILE *out, const char *only_account,
                                 int dry_run, int verbose) {
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

        fprintf(out, "=== %s rules: %s ===\n",
               dry_run ? "Dry-run" : "Applying", accounts[i].name);
        local_store_init(accounts[i].cfg->host, accounts[i].cfg->user);

        MailRules *rules = mail_rules_load(accounts[i].name);
        if (!rules || rules->count == 0) {
            fprintf(out, "  No rules found for %s.\n", accounts[i].name);
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
                            print_rule_matches(out, rules, fields[0], fields[1],
                                               NULL, fields[3], uid, dry_run);
                        fired_total++;
                        if (!dry_run) {
                            local_hdr_update_labels("", uid,
                                                     (const char **)add_out, add_count,
                                                     (const char **)rm_out,  rm_count);
                            for (int j = 0; j < add_count; j++) {
                                label_idx_add(add_out[j], uid);
                                local_pending_label_add(uid, add_out[j], 1);
                            }
                            for (int j = 0; j < rm_count;  j++) {
                                label_idx_remove(rm_out[j], uid);
                                local_pending_label_add(uid, rm_out[j], 0);
                            }
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
                fprintf(out, "  No messages found in folder %s.\n", imap_folder);
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
                            print_rule_matches(out, rules,
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
            fprintf(out, "  Rules dry-run: %d message(s) would be modified.\n", fired_total);
        else
            fprintf(out, "  Rules applied: %d message(s) modified.\n", fired_total);
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

int email_service_apply_rules(const char *only_account, int dry_run, int verbose) {
    return email_service_apply_rules_to(stdout, only_account, dry_run, verbose);
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
        printf("Contacts rebuilt for %s\n", accounts[i].name);
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

/* ── Credential check ────────────────────────────────────────────────── */

int email_service_check_credential(const Config *cfg) {
    if (!cfg) return -1;

    const char *who   = cfg->user ? cfg->user : "(no username set)";
    const char *where = cfg->gmail_mode ? "the Gmail API"
                                        : (cfg->host ? cfg->host : "(no host set)");

    printf("Checking the connection to %s...\n", where);
    fflush(stdout);

    EMC_AUTO_CLOSE emc_client *mc = make_mail(cfg);
    if (!mc) {
        /* Why it failed is reported by the layer that knows: imap_connect()
         * names a rejected login, gmail_auth_refresh() names a revoked
         * authorization.  Here we only say which answer we got. */
        fprintf(stderr, "The server did not accept the connection for %s.\n", who);
        return -1;
    }

    printf("Connected: %s accepted by %s.\n", who, where);
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
    if (g_json_out) {
        printf("[");
        for (int i = 0; i < count; i++) {
            printf("%s\n  {\"filename\": ", i ? "," : "");
            print_json_string(atts[i].filename);
            printf(", \"size\": %zu}", atts[i].size);
        }
        printf("%s]\n", count ? "\n" : "");
        mime_free_attachments(atts, count);
        return 0;
    }
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
    } else if (flag_bit == MSG_FLAG_FORWARDED) {
        flag_name = "$Forwarded";
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

    /* Gmail has no pending_flags drain: its sync path flushes
     * pending_labels.tsv instead.  Queue the change there, and only when the
     * synchronous push below did not get through. */
    const char *gmail_label = NULL;
    if (cfg->gmail_mode) {
        if (flag_bit == MSG_FLAG_UNSEEN)       gmail_label = "UNREAD";
        else if (flag_bit == MSG_FLAG_FLAGGED) gmail_label = "STARRED";
    } else {
        /* Enqueue to pending flag queue */
        local_pending_flag_add(use_folder, uid, flag_name, imap_add);
    }

    /* Synchronous server push */
    int pushed = 0;
    emc_client *mc = make_mail(cfg);
    const int connected = mc != NULL;
    if (mc) {
        if (emc_select(mc, use_folder) == 0 &&
            emc_set_flag(mc, uid, flag_name, imap_add) == 0)
            pushed = 1;
        emc_close(mc);
    }

    if (cfg->gmail_mode) {
        if (pushed) return 0;
        if (!gmail_label) {
            fprintf(stderr, "Warning: Could not reach Gmail. The %s change "
                    "has no Gmail counterpart and was kept locally only.\n",
                    flag_name);
        } else if (local_pending_label_add(uid, gmail_label, add) == 0) {
            fprintf(stderr, "Warning: Could not reach Gmail. Change queued "
                    "for next sync.\n");
        } else {
            fprintf(stderr, "Error: Could not reach Gmail and could not "
                    "queue the change; it exists only locally.\n");
            return -1;
        }
    } else if (!connected) {
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

    emc_client *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect to server.\n");
        return -1;
    }
    int rc = emc_modify_label(mc, uid, label, add);
    emc_close(mc);

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
    emc_client *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect.\n");
        return -1;
    }

    char **names = NULL, **ids = NULL;
    int count = 0;
    int rc = emc_list_with_ids(mc, &names, &ids, &count);
    emc_close(mc);

    if (rc == 0 && g_json_out) {
        printf("[");
        for (int i = 0; i < count; i++) {
            printf("%s\n  {\"name\": ", i ? "," : "");
            print_json_string(names[i]);
            printf(", \"id\": ");
            print_json_string(ids[i]);
            printf("}");
        }
        printf("%s]\n", count ? "\n" : "");
        for (int i = 0; i < count; i++) { free(names[i]); free(ids[i]); }
        free(names);
        free(ids);
        return 0;
    }
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
    emc_client *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect.\n");
        return -1;
    }
    char *new_id = NULL;
    int rc = emc_create_label(mc, name, &new_id);
    emc_close(mc);

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
    emc_client *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect.\n");
        return -1;
    }
    int rc = emc_delete_label(mc, label_id);
    emc_close(mc);

    if (rc == 0)
        printf("Label '%s' deleted.\n", label_id);
    return rc;
}

int email_service_mark_junk(const Config *cfg, const char *uid) {
    local_store_init(cfg->host, cfg->user);
    emc_client *mc = make_mail(cfg);
    if (!mc) { fprintf(stderr, "Error: Could not connect.\n"); return -1; }
    int rc = emc_mark_junk(mc, uid);
    emc_close(mc);
    if (rc == 0) printf("Message %s marked as junk.\n", uid);
    return rc;
}

int email_service_mark_notjunk(const Config *cfg, const char *uid) {
    local_store_init(cfg->host, cfg->user);
    emc_client *mc = make_mail(cfg);
    if (!mc) { fprintf(stderr, "Error: Could not connect.\n"); return -1; }
    int rc = emc_mark_notjunk(mc, uid);
    emc_close(mc);
    if (rc == 0) printf("Message %s marked as not-junk.\n", uid);
    return rc;
}

int email_service_move(const Config *cfg, const char *uid,
                       const char *folder, const char *target) {
    const char *src = folder ? folder : (cfg->folder ? cfg->folder : "INBOX");
    if (strcmp(src, target) == 0) {
        fprintf(stderr, "Error: the message is already in '%s'.\n", target);
        return -1;
    }
    local_store_init(cfg->host, cfg->user);
    emc_client *mc = make_mail(cfg);
    if (!mc) { fprintf(stderr, "Error: Could not connect.\n"); return -1; }

    int rc;
    if (cfg->gmail_mode) {
        rc = emc_modify_label(mc, uid, target, 1);
        if (rc == 0) rc = emc_modify_label(mc, uid, src, 0);
        if (rc == 0) {
            label_idx_add(target, uid);
            local_hdr_update_labels("", uid, &target, 1, NULL, 0);
            label_idx_remove(src, uid);
            local_hdr_update_labels("", uid, NULL, 0, &src, 1);
        }
    } else {
        rc = emc_select(mc, src);
        if (rc == 0) rc = emc_move_to_folder(mc, uid, target);
        if (rc == 0) {
            Manifest *m = manifest_load(src);
            if (m) {
                manifest_remove(m, uid);
                manifest_save(src, m);
                manifest_free(m);
            }
        }
    }
    emc_close(mc);
    if (rc == 0) printf("Message %s moved to '%s'.\n", uid, target);
    else fprintf(stderr, "Error: could not move message %s to '%s'.\n", uid, target);
    return rc == 0 ? 0 : -1;
}

/* ── trash and archive from the command line (EMAIL-132) ─────────────── */

/** True when @p folder names the Gmail trash (the label id or the local index). */
static int gmail_folder_is_trash(const char *folder) {
    return folder && (strcmp(folder, "TRASH") == 0 || strcmp(folder, "_trash") == 0);
}

/** Drop @p uid from the local manifest of @p folder. */
static void manifest_drop_uid(const char *folder, const char *uid) {
    Manifest *m = manifest_load(folder);
    if (!m) return;
    manifest_remove(m, uid);
    manifest_save(folder, m);
    manifest_free(m);
}

int email_service_trash(const Config *cfg, const char *uid, const char *folder) {
    const char *use_folder = folder ? folder
                           : (cfg->folder ? cfg->folder : "INBOX");
    local_store_init(cfg->host, cfg->user);
    emc_client *mc = make_mail(cfg);
    if (!mc) { fprintf(stderr, "Error: Could not connect.\n"); return -1; }
    int rc;
    int permanent;
    if (cfg->gmail_mode) {
        permanent = gmail_folder_is_trash(use_folder);
        rc = permanent ? emc_delete(mc, uid) : emc_trash(mc, uid);
    } else {
        const char *trf = cfg->trash_folder ? cfg->trash_folder : "Trash";
        permanent = strcmp(use_folder, trf) == 0;
        rc = emc_select(mc, use_folder);
        if (rc == 0)
            rc = permanent ? emc_delete(mc, uid) : emc_move_to_folder(mc, uid, trf);
    }
    emc_close(mc);
    if (rc != 0) {
        fprintf(stderr, "Error: Could not %s message %s.\n",
                permanent ? "delete" : "move to the trash", uid);
        return -1;
    }

    if (cfg->gmail_mode) {
        char **all_labels = NULL; int all_count = 0;
        label_idx_list(&all_labels, &all_count);
        for (int j = 0; j < all_count; j++) {
            label_idx_remove(all_labels[j], uid);
            free(all_labels[j]);
        }
        free(all_labels);
        if (!permanent) label_idx_add("_trash", uid);
    } else {
        manifest_drop_uid(use_folder, uid);
    }
    if (permanent) printf("Message %s deleted for good.\n", uid);
    else           printf("Message %s moved to the trash.\n", uid);
    return 0;
}

int email_service_archive(const Config *cfg, const char *uid, const char *folder) {
    const char *use_folder = folder ? folder
                           : (cfg->folder ? cfg->folder : "INBOX");
    local_store_init(cfg->host, cfg->user);

    if (!cfg->gmail_mode) {
        const char *arf = cfg->archive_folder ? cfg->archive_folder : "Archive";
        if (strcmp(use_folder, arf) == 0) {
            printf("Message %s is already in %s \xe2\x80\x94 no change.\n", uid, arf);
            return 0;
        }
        emc_client *mc = make_mail(cfg);
        if (!mc) { fprintf(stderr, "Error: Could not connect.\n"); return -1; }
        int rc = emc_select(mc, use_folder);
        if (rc == 0) rc = emc_move_to_folder(mc, uid, arf);
        emc_close(mc);
        if (rc != 0) {
            fprintf(stderr, "Error: Could not move message %s to %s.\n", uid, arf);
            return -1;
        }
        manifest_drop_uid(use_folder, uid);
        printf("Message %s archived (moved to %s).\n", uid, arf);
        return 0;
    }

    /* Gmail: remove every user-visible label (the TUI's 'a'), mark read. */
    if (strcmp(use_folder, "_nolabel") == 0) {
        printf("Message %s is already in the archive \xe2\x80\x94 no change.\n", uid);
        return 0;
    }
    emc_client *mc = make_mail(cfg);
    if (!mc) { fprintf(stderr, "Error: Could not connect.\n"); return -1; }
    int rc = 0;
    char *lbl_str = local_hdr_get_labels("", uid);
    char *copy = lbl_str ? strdup(lbl_str) : NULL;
    int n = 1;
    if (lbl_str) for (const char *p = lbl_str; *p; p++) if (*p == ',') n++;
    const char **rm = malloc((size_t)n * sizeof(char *));
    int rm_n = 0;
    if (copy && rm) {
        char *tok = copy, *sep;
        while (tok && *tok) {
            sep = strchr(tok, ',');
            if (sep) *sep = '\0';
            if (tok[0] && tok[0] != '_') {
                rm[rm_n++] = tok;
                if (strcmp(tok, "IMPORTANT") != 0 &&
                    strncmp(tok, "CATEGORY_", 9) != 0 &&
                    emc_modify_label(mc, uid, tok, 0) != 0)
                    rc = -1;
            }
            tok = sep ? sep + 1 : NULL;
        }
    }
    if (rc == 0 && emc_set_flag(mc, uid, "\\Seen", 1) != 0) rc = -1;
    emc_close(mc);
    if (rc != 0) {
        fprintf(stderr, "Error: Could not archive message %s.\n", uid);
        free(copy); free(rm); free(lbl_str);
        return -1;
    }
    for (int i = 0; i < rm_n; i++) label_idx_remove(rm[i], uid);
    if (rm_n > 0) local_hdr_update_labels("", uid, NULL, 0, rm, rm_n);
    label_idx_remove("UNREAD", uid);
    label_idx_add("_nolabel", uid);
    free(copy); free(rm); free(lbl_str);
    printf("Message %s archived.\n", uid);
    return 0;
}

/* Re-reads the folder list from the server into the local cache, so that
 * the lists the programs show (cached after the first read) follow a folder
 * that was just created or deleted (EMAIL-129). */
static void refresh_folder_cache(emc_client *mc) {
    char **names = NULL;
    int n = 0;
    char sep = '.';
    if (emc_list_names(mc, &names, &n, &sep) != 0) return;
    if (n > 0) local_folder_list_save((const char **)names, n, sep);
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* Creates (@p create = 1) or deletes the IMAP folder @p name.  @p quiet:
 * no text on stdout (the TUI shows its own message). */
static int change_folder(const Config *cfg, const char *name, int create, int quiet) {
    const char *verb = create ? "create-folder" : "delete-folder";
    if (cfg->gmail_mode) {
        fprintf(stderr, "Error: '%s' is IMAP-only. Use '%s' for Gmail.\n", verb,
                create ? "create-label" : "delete-label");
        return -1;
    }
    emc_client *mc = make_mail(cfg);
    if (!mc) {
        fprintf(stderr, "Error: Could not connect.\n");
        return -1;
    }
    int rc = create ? emc_create_folder(mc, name) : emc_delete_folder(mc, name);
    if (rc == 0) {
        local_store_init(cfg->host, cfg->user);
        refresh_folder_cache(mc);
    }
    emc_close(mc);

    if (rc == 0 && !quiet)
        printf("Folder '%s' %s.\n", name, create ? "created" : "deleted");
    return rc;
}

int email_service_create_folder(const Config *cfg, const char *name) {
    return change_folder(cfg, name, 1, 0);
}

int email_service_delete_folder(const Config *cfg, const char *name) {
    return change_folder(cfg, name, 0, 0);
}

/* The text of a message as the reply quote and the forward show it: the
 * rendered HTML part when there is one, else the plain text.  free()able. */
static char *original_text(const char *raw) {
    char *body_text = NULL;
    char *html_raw = mime_get_html_part(raw);
    if (html_raw) {
        body_text = html_render(html_raw, 72, 0);
        free(html_raw);
    }
    if (!body_text) body_text = mime_get_text_body(raw);
    return body_text;
}

/* ── Forward (the TUI's 'F', EMAIL-134) ───────────────────────────────── */

int email_service_forward_compose(const char *raw, const char *note,
                                  char **subject_out, char **body_out) {
    if (!raw || !subject_out || !body_out) return -1;
    *subject_out = NULL;
    *body_out = NULL;

    char *subj_raw = mime_get_header(raw, "Subject");
    char *subj_dec = subj_raw ? mime_decode_words(subj_raw) : strdup("");
    free(subj_raw);
    size_t slen = strlen("Fwd: ") + (subj_dec ? strlen(subj_dec) : 0) + 1;
    char *subject = malloc(slen);
    if (!subject) { free(subj_dec); return -1; }
    snprintf(subject, slen, "Fwd: %s", subj_dec ? subj_dec : "");
    free(subj_dec);

    char *body_text = original_text(raw);

    char *from_raw = mime_get_header(raw, "From");
    char *from_dec = from_raw ? mime_decode_words(from_raw) : NULL;
    free(from_raw);
    char *date_raw = mime_get_header(raw, "Date");
    char *date_fmt = date_raw ? mime_format_date(date_raw) : NULL;
    free(date_raw);

    const char *n = note ? note : "";
    size_t qcap = 4096 + strlen(n) + (body_text ? strlen(body_text) * 2 : 0);
    char *quoted = malloc(qcap);
    if (quoted) {
        int off = snprintf(quoted, qcap,
                           "%s\n\n--- Forwarded message ---\nFrom: %s\nDate: %s\n\n",
                           n, from_dec ? from_dec : "?",
                           date_fmt ? date_fmt : "?");
        if (body_text && off > 0) {
            int rest = (int)strlen(body_text);
            if (off + rest < (int)qcap - 1) {
                memcpy(quoted + off, body_text, (size_t)rest);
                off += rest;
                quoted[off] = '\0';
            }
        }
    }
    free(from_dec);
    free(date_fmt);
    free(body_text);
    if (!quoted) { free(subject); return -1; }
    *subject_out = subject;
    *body_out = quoted;
    return 0;
}

int email_service_forward(const Config *cfg, const char *uid,
                          const char *folder, const char *to, const char *cc,
                          const char *note) {
    if (!to || !to[0]) {
        fprintf(stderr, "Error: forward requires a recipient (--to).\n");
        return -1;
    }
    const char *use_folder = folder ? folder
                           : (cfg->folder ? cfg->folder : "INBOX");
    local_store_init(cfg->host, cfg->user);

    char *raw = NULL;
    if (local_msg_exists(use_folder, uid)) {
        raw = local_msg_load(use_folder, uid);
    } else {
        Config tmp = *cfg;
        tmp.folder = (char *)use_folder;
        raw = load_raw_message(&tmp, uid);
    }
    if (!raw) {
        fprintf(stderr, "Error: Could not load message UID %s.\n", uid);
        return -1;
    }

    char *subject = NULL, *body = NULL;
    int rc = email_service_forward_compose(raw, note, &subject, &body);
    free(raw);
    if (rc != 0) {
        fprintf(stderr, "Error: Failed to build the forwarded message.\n");
        return -1;
    }

    const char *from = cfg->smtp_user ? cfg->smtp_user : cfg->user;
    emc_message p = {.from = from, .to = to, .cc = cc,
                     .subject = subject, .body = body};
    char *msg = NULL;
    size_t msg_len = 0;
    if (mail_compose(&p, &msg, &msg_len) != 0) {
        fprintf(stderr, "Error: Failed to build message.\n");
        free(subject); free(body);
        return -1;
    }
    free(subject); free(body);

    /* The envelope carries every recipient, To and Cc; the library sends to
     * all of them in one session (EMAIL-133). */
    char rcpt[2048];
    snprintf(rcpt, sizeof(rcpt), "%s%s%s", to,
             (cc && cc[0]) ? "," : "", (cc && cc[0]) ? cc : "");
    rc = mail_send_wire(cfg, from, rcpt, msg, msg_len);
    if (rc == 0) {
        printf("Message forwarded.\n");
        if (email_service_save_sent(cfg, msg, msg_len) == 0)
            printf("Saved.\n");
        else
            fprintf(stderr, "(Could not save to Sent folder - "
                    "check EMAIL_SENT_FOLDER in config.)\n");
        email_service_set_flag(cfg, uid, use_folder, MSG_FLAG_FORWARDED, 1);
    }
    free(msg);
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

/* ── Reply composition, shared by email-cli and email-tui (EMAIL-133) ─── */

char *email_service_reply_quote(const char *raw) {
    char *body_text = original_text(raw);

    char *from_raw = mime_get_header(raw, "From");
    char *from_dec = from_raw ? mime_decode_words(from_raw) : NULL;
    free(from_raw);
    char *date_raw = mime_get_header(raw, "Date");
    char *date_fmt = date_raw ? mime_format_date(date_raw) : NULL;
    free(date_raw);

    /* "On <date>, <from> wrote:\n> line1\n> line2\n..." */
    size_t qcap = 4096 + (body_text ? strlen(body_text) * 2 : 0);
    char *quoted = malloc(qcap);
    if (quoted) {
        int off = snprintf(quoted, qcap, "On %s, %s wrote:\n",
                           date_fmt ? date_fmt : "?", from_dec ? from_dec : "?");
        if (body_text) {
            const char *p = body_text;
            while (*p && off < (int)qcap - 4) {
                const char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                size_t advance = len + (nl ? 1 : 0);
                if (len > 0 && p[len - 1] == '\r') len--;
                if (off + 2 + (int)len + 1 < (int)qcap) {
                    quoted[off++] = '>';
                    quoted[off++] = ' ';
                    memcpy(quoted + off, p, len);
                    off += (int)len;
                    quoted[off++] = '\n';
                    quoted[off]   = '\0';
                }
                p += advance;
                if (!nl) break;
            }
        }
    }
    free(from_dec);
    free(date_fmt);
    free(body_text);
    return quoted;
}

char *email_service_reply_all_cc(const char *raw, const char *own) {
    char *orig_to = mime_get_header(raw, "To");
    char *orig_cc = mime_get_header(raw, "Cc");
    char *dec_to  = orig_to ? mime_decode_words(orig_to) : NULL;
    char *dec_cc  = orig_cc ? mime_decode_words(orig_cc) : NULL;
    free(orig_to); free(orig_cc);

    char combined[2048] = "";
    if (dec_to && dec_to[0])
        strncpy(combined, dec_to, sizeof(combined) - 1);
    if (dec_cc && dec_cc[0]) {
        if (combined[0])
            strncat(combined, ", ", sizeof(combined) - strlen(combined) - 1);
        strncat(combined, dec_cc, sizeof(combined) - strlen(combined) - 1);
    }
    free(dec_to); free(dec_cc);

    char cc_buf[1024] = "";
    int cc_len = 0;
    char *p = combined;
    while (p && *p) {
        while (*p == ' ') p++;
        char *sep = strchr(p, ',');
        int elen = sep ? (int)(sep - p) : (int)strlen(p);
        while (elen > 0 && p[elen - 1] == ' ') elen--;
        /* only this entry is searched for the own address, not the rest of the list */
        char entry[512];
        snprintf(entry, sizeof(entry), "%.*s", elen, p);
        if (elen > 0 && !(own && own[0] && strcasestr(entry, own))) {
            if (cc_len + elen + 3 < (int)sizeof(cc_buf) - 1) {
                if (cc_len > 0) {
                    cc_buf[cc_len++] = ';';
                    cc_buf[cc_len++] = ' ';
                }
                memcpy(cc_buf + cc_len, p, (size_t)elen);
                cc_len += elen;
                cc_buf[cc_len] = '\0';
            }
        }
        if (!sep) break;
        p = sep + 1;
    }
    return strdup(cc_buf);
}

int email_service_mark_answered(const Config *cfg, const char *folder, const char *uid) {
    local_store_init(cfg->host, cfg->user);
    Manifest *mf = manifest_load(folder);
    if (!mf) return -1;
    int rc = -1;
    ManifestEntry *me = manifest_find(mf, uid);
    if (me) {
        me->flags |= MSG_FLAG_ANSWERED;
        manifest_save(folder, mf);
        rc = 0;
    }
    manifest_free(mf);
    return rc;
}

int email_service_reply(const Config *cfg, const char *uid, const char *folder,
                        int all, const char *body) {
    const char *use_folder = folder ? folder
                           : (cfg->folder ? cfg->folder : "INBOX");
    local_store_init(cfg->host, cfg->user);
    char *raw = local_msg_exists(use_folder, uid)
                    ? local_msg_load(use_folder, uid)
                    : fetch_uid_content_in(cfg, use_folder, uid, 0);
    if (!raw) {
        fprintf(stderr, "Error: Could not load message UID %s.\n", uid);
        return -1;
    }
    char *reply_to = NULL, *subject = NULL, *msg_id = NULL;
    if (emc_reply_meta(raw, &reply_to, &subject, &msg_id) != EMC_OK) {
        free(raw);
        fprintf(stderr, "Error: Could not parse message headers.\n");
        return -1;
    }
    const char *from = cfg->smtp_user ? cfg->smtp_user : cfg->user;
    char *cc = all ? email_service_reply_all_cc(raw, from) : NULL;
    char *quoted = email_service_reply_quote(raw);
    free(raw);

    size_t blen = (body ? strlen(body) : 0) + (quoted ? strlen(quoted) : 0) + 4;
    char *text = malloc(blen);
    char *rcpt = malloc(strlen(reply_to) + (cc ? strlen(cc) : 0) + 4);
    if (!text || !rcpt) {
        free(text); free(rcpt); free(cc); free(quoted);
        free(reply_to); free(subject); free(msg_id);
        return -1;
    }
    snprintf(text, blen, "%s%s%s", body ? body : "", body && body[0] ? "\n\n" : "",
             quoted ? quoted : "");
    snprintf(rcpt, strlen(reply_to) + (cc ? strlen(cc) : 0) + 4, "%s%s%s",
             reply_to, cc && cc[0] ? "; " : "", cc ? cc : "");

    int rc = -1;
    /* the header list is comma separated (the dialog's "; " is for the eye) */
    char *cc_hdr = cc && cc[0] ? strdup(cc) : NULL;
    for (char *q = cc_hdr; q && *q; q++)
        if (*q == ';') *q = ',';
    emc_message p = {.from = from, .to = reply_to,
                     .cc = cc_hdr,
                     .subject = subject, .body = text,
                     .in_reply_to = msg_id};
    char *msg = NULL;
    size_t msg_len = 0;
    if (mail_compose(&p, &msg, &msg_len) != 0) {
        fprintf(stderr, "Error: Failed to build message.\n");
    } else if (mail_send_wire(cfg, from, rcpt, msg, msg_len) == 0) {
        printf("Reply sent.\n");
        if (email_service_save_sent(cfg, msg, msg_len) == 0)
            printf("Saved.\n");
        else
            fprintf(stderr, "(Could not save to Sent folder \xe2\x80\x94 "
                    "check EMAIL_SENT_FOLDER in config.)\n");
        if (email_service_mark_answered(cfg, use_folder, uid) == 0)
            printf("Message %s marked as answered (local until the next sync).\n", uid);
        rc = 0;
    }
    free(msg); free(text); free(rcpt); free(cc); free(cc_hdr); free(quoted);
    free(reply_to); free(subject); free(msg_id);
    return rc;
}

void email_service_help_popup(const char *title, const char *rows[][2], int n) {
    show_help_popup(title, rows, n);
}

/* ── JSON for the account and rule listings (EMAIL-122) ──────────────── */

int email_service_list_accounts_json(void) {
    int count = 0;
    AccountEntry *accs = config_list_accounts(&count);
    printf("[");
    for (int i = 0; i < count; i++) {
        printf("%s\n  {\"account\": ", i ? "," : "");
        print_json_string(accs[i].name);
        printf(", \"type\": \"%s\", \"server\": ",
               (accs[i].cfg && accs[i].cfg->gmail_mode) ? "Gmail" : "IMAP");
        print_json_string(accs[i].cfg ? accs[i].cfg->host : NULL);
        printf("}");
    }
    printf("%s]\n", count ? "\n" : "");
    if (accs) config_free_account_list(accs, count);
    return 0;
}

int email_service_rules_json(const char *only_account) {
    int count = 0;
    AccountEntry *accs = config_list_accounts(&count);
    int printed = 0;
    printf("[");
    for (int i = 0; i < count; i++) {
        if (only_account && only_account[0] && strcmp(accs[i].name, only_account) != 0)
            continue;
        MailRules *rules = mail_rules_load(accs[i].name);
        printf("%s\n  {\"account\": ", printed ? "," : "");
        print_json_string(accs[i].name);
        printf(", \"rules\": [");
        for (int r = 0; rules && r < rules->count; r++) {
            const MailRule *rule = &rules->rules[r];
            printf("%s\n    {\"name\": ", r ? "," : "");
            print_json_string(rule->name);
            printf(", \"when\": ");
            print_json_string(rule->when);
            printf(", \"then_add_label\": [");
            for (int j = 0; j < rule->then_add_count; j++) {
                printf("%s", j ? ", " : "");
                print_json_string(rule->then_add_label[j]);
            }
            printf("], \"then_remove_label\": [");
            for (int j = 0; j < rule->then_rm_count; j++) {
                printf("%s", j ? ", " : "");
                print_json_string(rule->then_rm_label[j]);
            }
            printf("], \"then_move_folder\": ");
            print_json_string(rule->then_move_folder);
            printf("}");
        }
        printf("]}");
        mail_rules_free(rules);
        printed++;
    }
    printf("%s]\n", printed ? "\n" : "");
    if (accs) config_free_account_list(accs, count);
    return printed;
}
