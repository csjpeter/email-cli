#include "email_service.h"
#include "imap_client.h"
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
static void print_statusbar(int trows, int width, const char *text) {
    fprintf(stderr, "\033[%d;1H\033[7m", trows);
    fputs(text, stderr);
    int used = visible_line_cols(text, text + strlen(text));
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

/* ── IMAP helpers ────────────────────────────────────────────────────── */

static ImapClient *make_imap(const Config *cfg) {
    return imap_connect(cfg->host, cfg->user, cfg->pass, !cfg->ssl_no_verify);
}

/* ── Folder status ───────────────────────────────────────────────────── */

typedef struct { int messages; int unseen; int flagged; } FolderStatus;

/** Read total, unseen and flagged counts for each folder from their local manifests.
 *  Instant — no server connection needed.
 *  Returns heap-allocated array; caller must free(). */
static FolderStatus *fetch_all_folder_statuses(const Config *cfg __attribute__((unused)),
                                                char **folders, int count) {
    FolderStatus *st = calloc((size_t)count, sizeof(FolderStatus));
    if (!st || count == 0) return st;
    for (int i = 0; i < count; i++)
        manifest_count_folder(folders[i], &st[i].messages,
                              &st[i].unseen, &st[i].flagged);
    return st;
}

/** Fetches headers or full message for a UID in <folder>.  Caller must free.
 *  Opens a new IMAP connection each call.  For bulk fetching (sync), use
 *  the imap_client API directly with a shared connection. */
static char *fetch_uid_content_in(const Config *cfg, const char *folder,
                                  int uid, int headers_only) {
    RAII_IMAP ImapClient *imap = make_imap(cfg);
    if (!imap) return NULL;
    if (imap_select(imap, folder) != 0) return NULL;
    return headers_only ? imap_uid_fetch_headers(imap, uid)
                        : imap_uid_fetch_body(imap, uid);
}

/* ── Cached header fetch ─────────────────────────────────────────────── */

/** Fetches headers for uid/folder, using the header cache. Caller must free. */
static char *fetch_uid_headers_cached(const Config *cfg, const char *folder,
                                       int uid) {
    if (local_hdr_exists(folder, uid))
        return local_hdr_load(folder, uid);
    char *hdrs = fetch_uid_content_in(cfg, folder, uid, 1);
    if (hdrs)
        local_hdr_save(folder, uid, hdrs, strlen(hdrs));
    return hdrs;
}

/**
 * Like fetch_uid_headers_cached but uses an already-connected and folder-selected
 * ImapClient instead of opening a new connection.  Falls back to the cache first.
 * Caller must free the returned string.
 */
static char *fetch_uid_headers_via(ImapClient *imap, const char *folder, int uid) {
    if (local_hdr_exists(folder, uid))
        return local_hdr_load(folder, uid);
    char *hdrs = imap_uid_fetch_headers(imap, uid);
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
                                const char *date) {
    /* label = 9 chars ("From:    "), remaining = SHOW_WIDTH - 9 = 71 */
    printf("From:    "); print_clean(from,    "(none)", SHOW_WIDTH - 9); putchar('\n');
    printf("Subject: "); print_clean(subject, "(none)", SHOW_WIDTH - 9); putchar('\n');
    printf("Date:    "); print_clean(date,    "(none)", SHOW_WIDTH - 9); putchar('\n');
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

/* Interactive attachment picker.
 * Shows attachment list, lets user navigate with arrows and press Enter to
 * pick one (or ESC/Backspace to cancel).
 * Returns selected index (0-based), or -1 if cancelled. */
static int show_attachment_picker(const MimeAttachment *atts, int count,
                                  int tcols, int trows) {
    int cursor = 0;
    for (;;) {
        printf("\033[0m\033[H\033[2J");
        printf("  Attachments (%d):\n\n", count);
        for (int i = 0; i < count; i++) {
            const char *name  = atts[i].filename     ? atts[i].filename     : "(no name)";
            const char *ctype = atts[i].content_type ? atts[i].content_type : "";
            /* Format decoded size */
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
                 "  \u2191\u2193=select  Enter=save to ~/Downloads"
                 "  Backspace=back  ESC=quit");
        print_statusbar(trows, tcols, sb);

        TermKey key = terminal_read_key();
        switch (key) {
        case TERM_KEY_BACK:
            return -1;   /* back to show view */
        case TERM_KEY_ESC:
        case TERM_KEY_QUIT:
            return -2;   /* quit program */
        case TERM_KEY_ENTER:
            return cursor;
        case TERM_KEY_NEXT_LINE:
        case TERM_KEY_NEXT_PAGE:
            if (cursor < count - 1) cursor++;
            break;
        case TERM_KEY_PREV_LINE:
        case TERM_KEY_PREV_PAGE:
            if (cursor > 0) cursor--;
            break;
        default:
            break;
        }
    }
}

/**
 * Show a message in interactive pager mode.
 * Returns 0 = ESC (go back to list), 1 = q/quit entirely, -1 = error.
 */
static int show_uid_interactive(const Config *cfg, const char *folder,
                                int uid, int page_size) {
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
        fprintf(stderr, "Could not load UID %d.\n", uid);
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

/* Bottom two rows: row (trows-1) = info line, row trows = shortcut hints.
 * Reserve one extra row compared to the previous single-line footer. */
#define SHOW_HDR_LINES_INT 6
    int rows_avail  = (page_size > SHOW_HDR_LINES_INT)
                      ? page_size - SHOW_HDR_LINES_INT : 1;
    int body_vrows  = count_visual_rows(body_text, term_cols);
    int total_pages = (body_vrows + rows_avail - 1) / rows_avail;
    if (total_pages < 1) total_pages = 1;

    /* Persistent info message — stays until replaced by a newer one */
    char info_msg[2048] = "";

    int result = 0;
    for (int cur_line = 0;;) {
        printf("\033[0m\033[H\033[2J");     /* reset attrs + clear screen */
        print_show_headers(from, subject, date);
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
                         "  a=attach(%d)  Backspace=list  ESC=quit --",
                         cur_page, total_pages, att_count);
            } else {
                snprintf(sb, sizeof(sb),
                         "-- [%d/%d] PgDn/\u2193=scroll  PgUp/\u2191=back"
                         "  Backspace=list  ESC=quit --",
                         cur_page, total_pages);
            }
            print_statusbar(term_rows, wrap_cols, sb);
        }

        TermKey key = terminal_read_key();
        fprintf(stderr, "\r\033[K");
        fflush(stderr);

        switch (key) {
        case TERM_KEY_BACK:
            result = 0;          /* back to list */
            goto show_int_done;
        case TERM_KEY_ESC:
        case TERM_KEY_QUIT:
            result = 1;          /* quit entirely */
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
        case TERM_KEY_IGNORE: {
            int ch = terminal_last_printable();
            if (ch == 'a' && att_count > 0) {
                int sel = show_attachment_picker(atts, att_count,
                                                 term_cols, term_rows);
                if (sel == -2) {
                    result = 1;  /* ESC/q in picker → quit program */
                    goto show_int_done;
                }
                if (sel >= 0) {
                    char *dir = attachment_save_dir();
                    char *fname = safe_filename_for_path(atts[sel].filename);
                    char dest[1024];
                    snprintf(dest, sizeof(dest), "%s/%s",
                             dir ? dir : ".", fname ? fname : "attachment");
                    int r = mime_save_attachment(&atts[sel], dest);
                    if (r == 0)
                        snprintf(info_msg, sizeof(info_msg),
                                 "  Saved: %.1900s", dest);
                    else
                        snprintf(info_msg, sizeof(info_msg),
                                 "  Save FAILED: %.1900s", dest);
                    free(dir);
                    free(fname);
                }
            }
            break;
        }
        }
    }
show_int_done:
#undef SHOW_HDR_LINES_INT
    mime_free_attachments(atts, att_count);
    free(body); free(body_wrapped); free(from); free(subject); free(date); free(raw);
    return result;
}

/* ── List helpers ────────────────────────────────────────────────────── */

typedef struct { int uid; int flags; time_t epoch; } MsgEntry;

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
    FILE *pf = fopen(pid_path, "r");
    if (!pf) return 0;
    int pid = 0;
    if (fscanf(pf, "%d", &pid) != 1) pid = 0;
    fclose(pf);
    if (pid <= 0) return 0;
    return platform_pid_is_program((pid_t)pid, "email-cli");
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
    return eb->uid - ea->uid;
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
        int depth = 0;
        for (const char *p = names[i]; *p; p++)
            if (*p == sep) depth++;
        for (int lv = 0; lv < depth; lv++) {
            int last = ancestor_is_last(names, count, i, lv, sep);
            printf("%s", last ? "    " : "\u2502   ");
        }
        int last = is_last_sibling(names, count, i, sep);
        printf("%s", last ? "\u2514\u2500\u2500 " : "\u251c\u2500\u2500 ");
        const char *comp = strrchr(names[i], sep);
        printf("%s", comp ? comp + 1 : names[i]);
        /* Inline counts for tree mode */
        char u[16] = "", f[16] = "", t[16] = "";
        if (unseen   > 0) snprintf(u, sizeof(u), "%d", unseen);
        if (flagged  > 0) snprintf(f, sizeof(f), "%d", flagged);
        if (messages > 0) snprintf(t, sizeof(t), "%d", messages);
        if (u[0] || f[0] || t[0])
            printf("  %s/%s/%s", u, f, t);
    } else {
        /* Flat mode: Unread | Flagged | Folder | Total */
        const char *comp    = strrchr(names[i], sep);
        const char *display = comp ? comp + 1 : names[i];
        char name_buf[256];
        snprintf(name_buf, sizeof(name_buf), "%s%s", display, has_kids ? "/" : "");
        char u[16] = "", f[16] = "";
        if (unseen  > 0) snprintf(u, sizeof(u), "%d", unseen);
        if (flagged > 0) snprintf(f, sizeof(f), "%d", flagged);
        if (messages > 0)
            printf("  %6s  %6s  %-*s  %7d", u, f, name_w, name_buf, messages);
        else
            printf("  %6s  %6s  %-*s  %7s", u, f, name_w, name_buf, "");
    }

    if (selected) printf("\033[K\033[0m");
    else if (messages == 0) printf("\033[0m");
    printf("\n");
}

static void render_folder_tree(char **names, int count, char sep,
                                const FolderStatus *statuses) {
    for (int i = 0; i < count; i++) {
        int depth = 0;
        for (const char *p = names[i]; *p; p++)
            if (*p == sep) depth++;

        /* Indent: one column per ancestor level */
        for (int lv = 0; lv < depth; lv++) {
            int last = ancestor_is_last(names, count, i, lv, sep);
            printf("%s", last ? "    " : "\u2502   "); /* │   */
        }

        /* Connector */
        int last = is_last_sibling(names, count, i, sep);
        printf("%s", last ? "\u2514\u2500\u2500 " : "\u251c\u2500\u2500 "); /* └── / ├── */

        /* Component name (last segment) */
        const char *comp = strrchr(names[i], sep);
        printf("%s", comp ? comp + 1 : names[i]);
        if (statuses && (statuses[i].messages > 0 || statuses[i].unseen > 0))
            printf(" (%d/%d)", statuses[i].unseen, statuses[i].messages);
        printf("\n");
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

int email_service_list(const Config *cfg, const EmailListOpts *opts) {
    const char *raw_folder = opts->folder ? opts->folder : cfg->folder;

    /* Normalise to the server-canonical name so the manifest key matches
     * what sync stored (e.g. config "Inbox" → server "INBOX"). */
    RAII_STRING char *folder_canonical = resolve_folder_name_dup(raw_folder);
    const char *folder = folder_canonical ? folder_canonical : raw_folder;

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

    /* Shared IMAP connection — populated in online mode, NULL in cron mode.
     * Kept alive for the full rendering loop so header fetches reuse it. */
    RAII_IMAP ImapClient *list_imap = NULL;

    if (cfg->sync_interval > 0) {
        /* ── Cron / cache-only mode: serve entirely from manifest ──────── */
        if (manifest->count == 0) {
            manifest_free(manifest);
            if (!opts->pager) {
                printf("No cached data for %s. Run 'email-cli sync' first.\n", folder);
                return 0;
            }
            RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();
            printf("\033[H\033[2J");
            printf("No cached data for %s.\n\n", folder);
            printf("Run 'email-cli sync' to download messages.\n\n");
            printf("\033[2m  Backspace=folders  ESC=quit\033[0m\n");
            fflush(stdout);
            for (;;) {
                TermKey key = terminal_read_key();
                if (key == TERM_KEY_BACK) return 1;
                if (key == TERM_KEY_QUIT || key == TERM_KEY_ESC) return 0;
            }
        }
        show_count = manifest->count;
        entries = malloc((size_t)show_count * sizeof(MsgEntry));
        if (!entries) { manifest_free(manifest); return -1; }
        for (int i = 0; i < show_count; i++) {
            entries[i].uid   = manifest->entries[i].uid;
            entries[i].flags = manifest->entries[i].flags;
            entries[i].epoch = parse_manifest_date(manifest->entries[i].date);
        }
        for (int i = 0; i < show_count; i++)
            if (entries[i].flags & MSG_FLAG_UNSEEN) unseen_count++;
    } else {
        /* ── Online mode: contact the server ───────────────────────────── */

        /* Fetch UNSEEN and ALL UID sets via a shared IMAP connection. */
        list_imap = make_imap(cfg);
        if (!list_imap) {
            manifest_free(manifest);
            fprintf(stderr, "Failed to connect.\n");
            return -1;
        }
        if (imap_select(list_imap, folder) != 0) {
            manifest_free(manifest);
            fprintf(stderr, "Failed to select folder %s.\n", folder);
            return -1;
        }

        int *unseen_uids = NULL;
        int  unseen_uid_count = 0;
        if (imap_uid_search(list_imap, "UNSEEN", &unseen_uids, &unseen_uid_count) != 0) {
            manifest_free(manifest);
            fprintf(stderr, "Failed to search mailbox.\n");
            return -1;
        }

        int *flagged_uids = NULL, flagged_count = 0;
        imap_uid_search(list_imap, "FLAGGED", &flagged_uids, &flagged_count);
        /* ignore errors — treat as 0 flagged */

        int *done_uids = NULL, done_count = 0;
        imap_uid_search(list_imap, "KEYWORD $Done", &done_uids, &done_count);
        /* ignore errors — treat as 0 done */

        int *all_uids  = NULL;
        int  all_count = 0;
        if (imap_uid_search(list_imap, "ALL", &all_uids, &all_count) != 0) {
            free(unseen_uids);
            free(flagged_uids);
            free(done_uids);
            manifest_free(manifest);
            fprintf(stderr, "Failed to search mailbox.\n");
            return -1;
        }
        /* Evict headers for messages deleted from the server */
        if (all_count > 0)
            local_hdr_evict_stale(folder, all_uids, all_count);

        /* Remove entries for UIDs deleted from the server */
        if (all_count > 0)
            manifest_retain(manifest, all_uids, all_count);

        show_count = all_count;

        if (show_count == 0) {
            manifest_free(manifest);
            free(unseen_uids);
            free(flagged_uids);
            free(done_uids);
            free(all_uids);
            if (!opts->pager) {
                printf("No messages in %s.\n", folder);
                return 0;
            }
            /* Interactive mode: show empty-folder screen and wait for input.
             * Returning immediately would drop the user back to the OS — instead
             * let them navigate away with Backspace (→ folder list) or ESC/^C. */
            RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();
            printf("\033[H\033[2J");
            printf("No messages in %s.\n\n", folder);
            printf("\033[2m  Backspace=folders  ESC=quit\033[0m\n");
            fflush(stdout);
            for (;;) {
                TermKey key = terminal_read_key();
                if (key == TERM_KEY_BACK)  return 1; /* go to folder list */
                if (key == TERM_KEY_QUIT || key == TERM_KEY_ESC) return 0;
            }
        }

        /* Build tagged entry array */
        entries = malloc((size_t)show_count * sizeof(MsgEntry));
        if (!entries) { free(unseen_uids); free(flagged_uids); free(done_uids); free(all_uids); manifest_free(manifest); return -1; }

        for (int i = 0; i < show_count; i++) {
            entries[i].uid   = all_uids[i];
            entries[i].flags = 0;
            for (int j = 0; j < unseen_uid_count;  j++)
                if (unseen_uids[j]  == all_uids[i]) { entries[i].flags |= MSG_FLAG_UNSEEN;  break; }
            for (int j = 0; j < flagged_count; j++)
                if (flagged_uids[j] == all_uids[i]) { entries[i].flags |= MSG_FLAG_FLAGGED; break; }
            for (int j = 0; j < done_count;    j++)
                if (done_uids[j]    == all_uids[i]) { entries[i].flags |= MSG_FLAG_DONE;    break; }
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
            printf("No messages in %s.\n", folder);
            return 0;
        }
        RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();
        printf("\033[H\033[2J");
        printf("No messages in %s.\n\n", folder);
        printf("\033[2m  Backspace=folders  ESC=quit\033[0m\n");
        fflush(stdout);
        for (;;) {
            TermKey key = terminal_read_key();
            if (key == TERM_KEY_BACK)  return 1;
            if (key == TERM_KEY_QUIT || key == TERM_KEY_ESC) return 0;
        }
    }

    /* Sort: unseen → flagged → rest, within each group newest (highest UID) first */
    qsort(entries, (size_t)show_count, sizeof(MsgEntry), cmp_uid_entry);

    int limit  = (opts->limit > 0) ? opts->limit : show_count;
    int cursor = (opts->offset > 1) ? opts->offset - 1 : 0;
    if (cursor >= show_count) cursor = 0;
    int wstart = cursor;   /* top of the visible window */

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
         * Fixed overhead per data row: "  UID  "(7) + "  DATE  "(18) + "  Sts   "(8) = 35
         * subj_w gets ~60% of remaining space, from_w ~40%. */
        int tcols    = terminal_cols();
        int overhead = 35;
        int avail    = tcols - overhead;
        if (avail < 40) avail = 40;
        int subj_w = avail * 3 / 5;
        int from_w = avail - subj_w;

        if (opts->pager) printf("\033[H\033[2J");

        /* Count / status line — reverse video, padded to full terminal width */
        {
            char cl[512];
            int sync = sync_is_running();
            snprintf(cl, sizeof(cl),
                     "  %d-%d of %d message(s) in %s (%d unread).%s",
                     wstart + 1, wend, show_count, folder, unseen_count,
                     sync ? "  \u21bb syncing..." : "");
            printf("\033[7m%s", cl);
            int used = visible_line_cols(cl, cl + strlen(cl));
            for (int p = used; p < tcols; p++) putchar(' ');
            printf("\033[0m\n\n");
        }
        printf("  %5s  %-16s  %-4s  %-*s  %s\n",
               "UID", "Date", "Sts", subj_w, "Subject", "From");
        printf("  \u2550\u2550\u2550\u2550\u2550  ");
        print_dbar(16); printf("  ");
        printf("\u2550\u2550\u2550\u2550  ");
        print_dbar(subj_w); printf("  ");
        print_dbar(from_w); printf("\n");

        /* Data rows: fetch-on-demand + immediate render per row */
        int manifest_dirty = 0;
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
                char *hdrs     = list_imap
                                 ? fetch_uid_headers_via(list_imap, folder, entries[i].uid)
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

            int sel = opts->pager && (i == cursor);
            if (sel) printf("\033[7m");

            char sts[5] = {
                (entries[i].flags & MSG_FLAG_UNSEEN)  ? 'N' : '-',
                (entries[i].flags & MSG_FLAG_FLAGGED) ? '*' : '-',
                (entries[i].flags & MSG_FLAG_DONE)    ? 'D' : '-',
                (entries[i].flags & MSG_FLAG_ATTACH)  ? 'A' : '-',
                '\0'
            };
            printf("  %5d  %-16.16s  %s  ", entries[i].uid, date, sts);
            print_padded_col(subject, subj_w);
            printf("  ");
            print_padded_col(from,    from_w);

            if (sel) printf("\033[K\033[0m");
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
            snprintf(sb, sizeof(sb),
                     "  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
                     "  n=new  f=flag  d=done  Backspace=folders  ESC=quit  [%d/%d]",
                     cursor + 1, show_count);
            print_statusbar(trows, tcols, sb);
        }

        TermKey key = terminal_read_key();
        fprintf(stderr, "\r\033[K"); fflush(stderr);

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
                /* ret == 0: Backspace → back to list; ret == -1: error → stay */
            }
            break;
        case TERM_KEY_IGNORE: {
            int ch = terminal_last_printable();
            if (ch == 'n' || ch == 'f' || ch == 'd') {
                int uid  = entries[cursor].uid;
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
                if (list_imap) {
                    if (ch == 'n') {
                        /* \Seen is the inverse of UNSEEN: UNSEEN set → mark read (add \Seen) */
                        imap_uid_set_flag(list_imap, uid, flag_name, currently ? 1 : 0);
                    } else {
                        imap_uid_set_flag(list_imap, uid, flag_name, !currently);
                    }
                }
                entries[cursor].flags ^= bit;
                ManifestEntry *me = manifest_find(manifest, uid);
                if (me) me->flags = entries[cursor].flags;
                manifest_save(folder, manifest);
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
    /* tui_raw / folder_canonical cleaned up automatically via RAII macros */
    manifest_free(manifest);
    free(entries);
    return list_result;
}

/** Fetch the folder list into a heap-allocated array; caller owns entries and array. */
static char **fetch_folder_list_from_server(const Config *cfg,
                                             int *count_out, char *sep_out) {
    RAII_IMAP ImapClient *imap = make_imap(cfg);
    if (!imap) return NULL;

    char **folders = NULL;
    int count = 0;
    char sep = '.';
    if (imap_list(imap, &folders, &count, &sep) != 0) return NULL;

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
        printf("  %6s  %6s  %-*s  %7s\n",
               "Unread", "Flagged", name_w, "Folder", "Total");
        printf("  \u2550\u2550\u2550\u2550\u2550\u2550  \u2550\u2550\u2550\u2550\u2550\u2550  ");
        print_dbar(name_w);
        printf("  \u2550\u2550\u2550\u2550\u2550\u2550\u2550\n");
        for (int i = 0; i < count; i++) {
            int unseen   = statuses ? statuses[i].unseen   : 0;
            int flagged  = statuses ? statuses[i].flagged  : 0;
            int messages = statuses ? statuses[i].messages : 0;
            char u[16] = "", f[16] = "";
            if (unseen  > 0) snprintf(u, sizeof(u), "%d", unseen);
            if (flagged > 0) snprintf(f, sizeof(f), "%d", flagged);
            if (messages == 0)
                printf("\033[2m  %6s  %6s  %-*s  %7s\033[0m\n",
                       u, f, name_w, folders[i], "");
            else
                printf("  %6s  %6s  %-*s  %7d\n",
                       u, f, name_w, folders[i], messages);
        }
    }

    free(statuses);
    for (int i = 0; i < count; i++) free(folders[i]);
    free(folders);
    return 0;
}

char *email_service_list_folders_interactive(const Config *cfg,
                                             const char *current_folder) {
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
        /* Fixed: "  " + 6 (unread) + "  " + 6 (flagged) + "  " + name_w + "  " + 7 (total) = name_w + 27; -1 safety */
        int name_w = tcols_f - 28;
        if (name_w < 20) name_w = 20;

        printf("\033[H\033[2J");
        if (!tree_mode && current_prefix[0])
            printf("Folders: %s/ (%d)\n\n", current_prefix, display_count);
        else
            printf("Folders (%d)\n\n", display_count);

        /* Column header and separator for flat mode */
        if (!tree_mode) {
            printf("  %6s  %6s  %-*s  %7s\n", "Unread", "Flagged", name_w, "Folder", "Total");
            printf("  \u2550\u2550\u2550\u2550\u2550\u2550  \u2550\u2550\u2550\u2550\u2550\u2550  ");
            print_dbar(name_w);
            printf("  \u2550\u2550\u2550\u2550\u2550\u2550\u2550\n");
        }

        for (int i = wstart; i < wend; i++) {
            if (tree_mode) {
                int msgs = statuses ? statuses[i].messages : 0;
                int unsn = statuses ? statuses[i].unseen   : 0;
                int flgd = statuses ? statuses[i].flagged  : 0;
                print_folder_item(folders, count, i, sep, 1, i == cursor, 0,
                                  msgs, unsn, flgd, 0);
            } else {
                int fi = vis[i];
                int hk = folder_has_children(folders, count, folders[fi], sep);
                int msgs = statuses ? statuses[fi].messages : 0;
                int unsn = statuses ? statuses[fi].unseen   : 0;
                int flgd = statuses ? statuses[fi].flagged  : 0;
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
                         "  t=%s  Backspace/ESC=quit  [%d/%d]",
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
            }
            /* at root (tree mode or flat root): ignore Backspace — use ESC */
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
        case TERM_KEY_IGNORE:
            if (terminal_last_printable() == 't') {
                tree_mode = !tree_mode;
                ui_pref_set_int("folder_view_mode", tree_mode);
                cursor = 0; wstart = 0;
                if (!tree_mode) current_prefix[0] = '\0';
            }
            break;
        }
    }
folders_int_done:
    free(statuses);
    free(vis);
    for (int i = 0; i < count; i++) free(folders[i]);
    free(folders);
    return selected;
}

int email_service_read(const Config *cfg, int uid, int pager, int page_size) {
    char *raw = NULL;

    if (local_msg_exists(cfg->folder, uid)) {
        logger_log(LOG_DEBUG, "Cache hit for UID %d in %s", uid, cfg->folder);
        raw = local_msg_load(cfg->folder, uid);
    } else {
        raw = fetch_uid_content_in(cfg, cfg->folder, uid, 0);
        if (raw) {
            local_msg_save(cfg->folder, uid, raw, strlen(raw));
            local_index_update(cfg->folder, uid, raw);
        }
    }

    if (!raw) { fprintf(stderr, "Could not load message UID %d.\n", uid); return -1; }

    char *from_raw = mime_get_header(raw, "From");
    char *from     = from_raw ? mime_decode_words(from_raw) : NULL;
    free(from_raw);
    char *subj_raw = mime_get_header(raw, "Subject");
    char *subject  = subj_raw ? mime_decode_words(subj_raw) : NULL;
    free(subj_raw);
    char *date_raw = mime_get_header(raw, "Date");
    char *date     = date_raw ? mime_format_date(date_raw) : NULL;
    free(date_raw);

    print_show_headers(from, subject, date);

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
                print_show_headers(from, subject, date);
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

    free(body); free(from); free(subject); free(date); free(raw);
    return 0;
}

/* ── Sync progress callback ──────────────────────────────────────────────── */

typedef struct {
    int    loop_i;     /* 1-based index of current UID in the loop */
    int    loop_total; /* total UIDs in this folder */
    int    uid;
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
    printf("  [%d/%d] UID %d  %s / %s ...\r",
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
        FILE *pf = fopen(pid_path, "r");
        if (pf) {
            int other = 0;
            if (fscanf(pf, "%d", &other) != 1) other = 0;
            fclose(pf);
            if (other > 0 && (pid_t)other != platform_getpid() &&
                platform_pid_is_program((pid_t)other, "email-cli")) {
                fprintf(stderr,
                        "email-cli sync is already running (PID %d). Skipping.\n",
                        other);
                return 0;
            }
        }
        /* Write our own PID */
        pf = fopen(pid_path, "w");
        if (pf) { fprintf(pf, "%d\n", (int)platform_getpid()); fclose(pf); }
    }

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

    /* One shared IMAP connection for all folder operations */
    RAII_IMAP ImapClient *sync_imap = make_imap(cfg);
    if (!sync_imap) {
        fprintf(stderr, "sync: could not connect to IMAP server.\n");
        for (int i = 0; i < folder_count; i++) free(folders[i]);
        free(folders);
        if (pid_path[0]) unlink(pid_path);
        return -1;
    }

    for (int fi = 0; fi < folder_count; fi++) {
        const char *folder = folders[fi];
        printf("Syncing %s ...\n", folder);
        fflush(stdout);

        if (imap_select(sync_imap, folder) != 0) {
            fprintf(stderr, "  WARN: SELECT failed for %s\n", folder);
            errors++;
            continue;
        }

        int *uids = NULL;
        int  uid_count = 0;
        if (imap_uid_search(sync_imap, "ALL", &uids, &uid_count) != 0) {
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

        /* Get UNSEEN set to mark entries */
        int *unseen_uids = NULL;
        int  unseen_count = 0;
        if (imap_uid_search(sync_imap, "UNSEEN", &unseen_uids, &unseen_count) != 0)
            unseen_count = 0;

        int *flagged_uids = NULL, flagged_count = 0;
        imap_uid_search(sync_imap, "FLAGGED", &flagged_uids, &flagged_count);

        int *done_uids = NULL, done_count = 0;
        imap_uid_search(sync_imap, "KEYWORD $Done", &done_uids, &done_count);

        /* Evict deleted messages from manifest */
        manifest_retain(manifest, uids, uid_count);

        int fetched = 0, skipped = 0;
        for (int i = 0; i < uid_count; i++) {
            int uid = uids[i];
            int uid_flags = 0;
            for (int j = 0; j < unseen_count;  j++)
                if (unseen_uids[j]  == uid) { uid_flags |= MSG_FLAG_UNSEEN;  break; }
            for (int j = 0; j < flagged_count; j++)
                if (flagged_uids[j] == uid) { uid_flags |= MSG_FLAG_FLAGGED; break; }
            for (int j = 0; j < done_count;    j++)
                if (done_uids[j]    == uid) { uid_flags |= MSG_FLAG_DONE;    break; }

            /* Show progress BEFORE the potentially slow network fetch */
            printf("  [%d/%d] UID %d...\r", i + 1, uid_count, uid);
            fflush(stdout);

            /* Fetch full body if not cached */
            if (!local_msg_exists(folder, uid)) {
                SyncProgressCtx pctx = { i + 1, uid_count, uid };
                imap_set_progress(sync_imap, sync_progress_cb, &pctx);
                char *raw = imap_uid_fetch_body(sync_imap, uid);
                imap_set_progress(sync_imap, NULL, NULL);
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
                    fprintf(stderr, "  WARN: failed to fetch UID %d in %s\n", uid, folder);
                    errors++;
                    continue;
                }
            } else {
                skipped++;
            }

            /* Update manifest entry (headers from local cache — now always warm) */
            ManifestEntry *me = manifest_find(manifest, uid);
            if (!me) {
                char *hdrs   = fetch_uid_headers_via(sync_imap, folder, uid);
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

            printf("  [%d/%d] UID %d   \r", i + 1, uid_count, uid);
            fflush(stdout);
        }
        free(unseen_uids);
        free(flagged_uids);
        free(done_uids);
        manifest_save(folder, manifest);
        manifest_free(manifest);
        free(uids);

        printf("  %d fetched, %d already stored%s\n",
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

int email_service_cron_setup(const Config *cfg) {

    /* Find the path to this binary */
    char self_path[1024] = {0};
#ifdef __linux__
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n < 0) {
        fprintf(stderr, "Cannot determine binary path.\n");
        return -1;
    }
    self_path[n] = '\0';
#else
    /* fallback: use 'which email-cli' */
    FILE *wp = popen("which email-cli", "r");
    if (!wp || !fgets(self_path, sizeof(self_path), wp)) {
        if (wp) pclose(wp);
        fprintf(stderr, "Cannot determine binary path.\n");
        return -1;
    }
    if (wp) pclose(wp);
    /* trim newline */
    self_path[strcspn(self_path, "\n")] = '\0';
#endif

    /* Build the cron line */
    char cron_line[2048];
    snprintf(cron_line, sizeof(cron_line),
             "*/%d * * * * %s sync >> ~/.cache/email-cli/sync.log 2>&1",
             cfg->sync_interval, self_path);

    /* Read existing crontab */
    FILE *fp = popen("crontab -l 2>/dev/null", "r");
    char existing[65536] = {0};
    size_t total = 0;
    if (fp) {
        size_t n2;
        while ((n2 = fread(existing + total, 1, sizeof(existing) - total - 1, fp)) > 0)
            total += n2;
        pclose(fp);
    }
    existing[total] = '\0';

    /* Check if already present */
    if (strstr(existing, "email-cli sync")) {
        printf("Cron entry already exists:\n");
        char *p = existing;
        while (*p) {
            char *nl = strchr(p, '\n');
            char *end = nl ? nl : p + strlen(p);
            char saved = *end; *end = '\0';
            if (strstr(p, "email-cli sync"))
                printf("  %s\n", p);
            *end = saved;
            p = nl ? nl + 1 : end;
        }
        printf("Remove it first with: email-cli cron remove\n");
        return 0;
    }

    /* Append our line (ensure existing ends with newline) */
    if (total > 0 && existing[total - 1] != '\n')
        strncat(existing, "\n", sizeof(existing) - total - 1);
    strncat(existing, cron_line, sizeof(existing) - strlen(existing) - 1);
    strncat(existing, "\n", sizeof(existing) - strlen(existing) - 1);

    FILE *cp = popen("crontab -", "w");
    if (!cp) {
        fprintf(stderr, "Failed to update crontab.\n");
        return -1;
    }
    fputs(existing, cp);
    int rc = pclose(cp);
    if (rc != 0) {
        fprintf(stderr, "crontab update failed (exit %d).\n", rc);
        return -1;
    }

    printf("Cron entry added (every %d minutes):\n  %s\n",
           cfg->sync_interval, cron_line);
    return 0;
}

int email_service_cron_remove(void) {
    FILE *fp = popen("crontab -l 2>/dev/null", "r");
    char existing[65536] = {0};
    size_t total = 0;
    if (fp) {
        size_t n;
        while ((n = fread(existing + total, 1, sizeof(existing) - total - 1, fp)) > 0)
            total += n;
        pclose(fp);
    }
    existing[total] = '\0';

    if (!strstr(existing, "email-cli sync")) {
        printf("No email-cli cron entry found.\n");
        return 0;
    }

    /* Filter out lines containing "email-cli sync" */
    char filtered[65536] = {0};
    size_t flen = 0;
    char *p = existing;
    while (*p) {
        char *nl = strchr(p, '\n');
        char *end = nl ? nl : p + strlen(p);
        char saved = *end; *end = '\0';
        if (!strstr(p, "email-cli sync")) {
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

    FILE *cp = popen("crontab -", "w");
    if (!cp) {
        fprintf(stderr, "Failed to update crontab.\n");
        return -1;
    }
    fputs(filtered, cp);
    int rc = pclose(cp);
    if (rc != 0) {
        fprintf(stderr, "crontab update failed.\n");
        return -1;
    }

    printf("Cron entry removed.\n");
    return 0;
}

int email_service_cron_status(void) {
    FILE *fp = popen("crontab -l 2>/dev/null", "r");
    if (!fp) {
        printf("No crontab found for this user.\n");
        return 0;
    }
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "email-cli sync")) {
            if (!found) printf("Active sync cron entry:\n");
            printf("  %s", line);
            found = 1;
        }
    }
    pclose(fp);
    if (!found)
        printf("No email-cli sync cron entry is installed.\n");
    return 0;
}
