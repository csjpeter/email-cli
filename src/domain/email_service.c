#include "email_service.h"
#include "curl_adapter.h"
#include "cache_store.h"
#include "mime_util.h"
#include "html_render.h"
#include "imap_util.h"
#include "raii.h"
#include "logger.h"
#include "platform/terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* ── Internal buffer ─────────────────────────────────────────────────── */

typedef struct { char *data; size_t size; } Buffer;

static size_t buffer_append(char *ptr, size_t size, size_t nmemb, void *ud) {
    Buffer *buf = ud;
    size_t n = size * nmemb;
    char *tmp = realloc(buf->data, buf->size + n + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, n);
    buf->size += n;
    buf->data[buf->size] = '\0';
    return n;
}

/* ── UID list parser ─────────────────────────────────────────────────── */

static int parse_uid_list(const char *resp, int **uids_out) {
    *uids_out = NULL;
    if (!resp) return 0;
    const char *p = strstr(resp, "* SEARCH");
    if (!p) return 0;
    p += strlen("* SEARCH");

    int cap = 32, cnt = 0;
    int *uids = malloc((size_t)cap * sizeof(int));
    if (!uids) return 0;
    while (*p) {
        char *end;
        long uid = strtol(p, &end, 10);
        if (end == p) { p++; continue; }
        if (uid > 0) {
            if (cnt == cap) {
                cap *= 2;
                int *tmp = realloc(uids, (size_t)cap * sizeof(int));
                if (!tmp) { free(uids); return 0; }
                uids = tmp;
            }
            uids[cnt++] = (int)uid;
        }
        p = end;
    }
    *uids_out = uids;
    return cnt;
}

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

/* ── Interactive pager helpers ───────────────────────────────────────── */

/**
 * Pager prompt for the standalone `show` command.
 * Returns scroll delta: 0 = quit, positive = forward N lines, negative = back N.
 */
static int pager_prompt(int cur_page, int total_pages, int page_size, int term_rows) {
    for (;;) {
        fprintf(stderr, "\033[%d;1H\033[7m\033[2K", term_rows); /* last row + reverse + erase */
        fprintf(stderr,
                "-- [%d/%d] PgDn/\u2193=scroll  PgUp/\u2191=back  ESC=quit --\033[0m",
                cur_page, total_pages);
        fflush(stderr);
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
static int count_lines(const char *s) {
    if (!s || !*s) return 0;
    int n = 1;
    for (const char *p = s; *p; p++)
        if (*p == '\n') n++;
    return n;
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
 * Print lines [from_line, from_line+line_count) from body.
 * Lines are 0-indexed.  Replays any ANSI state accumulated in skipped lines
 * so that multi-line styled spans remain correct across page boundaries.
 */
static void print_body_page(const char *body, int from_line, int line_count) {
    int line = 0;
    const char *p = body;
    while (*p && line < from_line) {   /* skip to from_line */
        if (*p++ == '\n') line++;
    }
    /* Restore ANSI state that was active at the start of from_line */
    if (p > body) {
        AnsiState st = {0};
        ansi_scan(body, p, &st);
        ansi_replay(&st);
    }
    int printed = 0;
    while (*p && printed < line_count) {
        const char *nl = strchr(p, '\n');
        if (nl) {
            printf("%.*s\n", (int)(nl - p), p);
            p = nl + 1;
        } else {
            printf("%s\n", p);
            break;
        }
        printed++;
    }
}

/* ── IMAP helpers ────────────────────────────────────────────────────── */

static CURL *make_curl(const Config *cfg) {
    return curl_adapter_init(cfg->user, cfg->pass, !cfg->ssl_no_verify);
}

/**
 * Extract the message literal from a raw IMAP FETCH response.
 *
 * IMAP literals use the format: {SIZE}\r\n<SIZE bytes>
 * This helper finds the first literal marker and returns a heap-allocated copy
 * of exactly SIZE bytes of content, NUL-terminated.  Returns NULL on failure.
 *
 * Used when fetching via CURLOPT_CUSTOMREQUEST (folder URL), which delivers the
 * full IMAP response — including the "* N FETCH (BODY[] {N}" envelope — to the
 * write callback.  Callers using a message URL in download mode do not need this.
 */
static char *extract_fetch_literal(const char *response, size_t response_len)
    __attribute__((unused));
static char *extract_fetch_literal(const char *response, size_t response_len) {
    if (!response) return NULL;
    const char *brace = strchr(response, '{');
    if (!brace) return NULL;
    char *end;
    long size = strtol(brace + 1, &end, 10);
    if (*end != '}' || size <= 0) return NULL;
    const char *data = end + 1;
    if (*data == '\r') data++;
    if (*data == '\n') data++;
    size_t offset = (size_t)(data - response);
    if (offset >= response_len) return NULL;
    size_t avail = response_len - offset;
    if ((size_t)size > avail) size = (long)avail;
    char *result = malloc((size_t)size + 1);
    if (!result) return NULL;
    memcpy(result, data, (size_t)size);
    result[size] = '\0';
    return result;
}

/** Issues UID SEARCH <criteria> in <folder>. Caller must free *uids_out. */
static int search_uids_in(const Config *cfg, const char *folder,
                          const char *criteria, int **uids_out) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return -1;

    RAII_STRING char *url = NULL;
    RAII_STRING char *cmd = NULL;
    if (asprintf(&url, "%s/%s", cfg->host, folder) == -1) return -1;
    if (asprintf(&cmd, "UID SEARCH %s", criteria) == -1) return -1;

    Buffer buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cmd);
    CURLcode res = curl_adapter_fetch(curl, url, &buf, buffer_append);
    int count = 0;
    if (res != CURLE_OK)
        logger_log(LOG_WARN, "UID SEARCH %s failed: %s", criteria, curl_easy_strerror(res));
    else
        count = parse_uid_list(buf.data, uids_out);
    free(buf.data);
    return count;
}

/**
 * State for the BODY.PEEK[] literal capture via the CURLINFO_DATA_IN debug hook.
 *
 * When CURLOPT_CUSTOMREQUEST is active, libcurl delivers the entire raw IMAP
 * server response (envelope + literal bytes) to CURLINFO_DATA_IN but does NOT
 * route it to WRITEFUNCTION.  We intercept DATA_IN to extract the literal.
 */
typedef struct {
    Buffer  literal;      /* accumulated literal bytes */
    int     in_literal;   /* currently reading a literal */
    long    bytes_left;   /* bytes still needed for the current literal */
} PeekCapture;

/** Debug callback that combines normal IMAP logging with literal capture. */
static int peek_debug_cb(CURL *handle, curl_infotype type,
                         char *data, size_t size, void *userp) {
    (void)handle;
    PeekCapture *cap = (PeekCapture *)userp;

    /* Normal IMAP traffic logging */
    if (type == CURLINFO_HEADER_IN || type == CURLINFO_HEADER_OUT ||
        type == CURLINFO_DATA_IN   || type == CURLINFO_DATA_OUT) {
        char buf[4096];
        size_t n = size < sizeof(buf) - 1 ? size : sizeof(buf) - 1;
        memcpy(buf, data, n);
        buf[n] = '\0';
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
            buf[--n] = '\0';
        if (n > 0)
            logger_log(LOG_DEBUG, "IMAP [%s] %s",
                       (type == CURLINFO_HEADER_OUT ||
                        type == CURLINFO_DATA_OUT) ? "OUT" : " IN", buf);
    }

    /* Literal capture from HEADER_IN: with CUSTOMREQUEST+folder URL, libcurl
     * routes the entire IMAP response (envelope + literal) through HEADER_IN.
     * DATA_IN only carries the FETCH envelope echo — not the literal bytes. */
    if (!cap || type != CURLINFO_HEADER_IN || size == 0) return 0;

    const char *p   = data;
    const char *end = data + size;

    while (p < end) {
        if (!cap->in_literal) {
            /* Scan for the IMAP literal size marker: {N}\r\n */
            const char *brace = memchr(p, '{', (size_t)(end - p));
            if (!brace) break;
            char *numend;
            long sz = strtol(brace + 1, &numend, 10);
            if (*numend != '}' || sz <= 0) { p = brace + 1; continue; }
            cap->in_literal  = 1;
            cap->bytes_left  = sz;
            /* Advance past }\r\n to the start of the literal bytes */
            p = numend + 1;
            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;
            /* Fall through to accumulate any bytes already in this chunk */
        }

        if (cap->in_literal && cap->bytes_left > 0) {
            size_t avail   = (size_t)(end - p);
            size_t to_copy = (size_t)cap->bytes_left < avail
                             ? (size_t)cap->bytes_left : avail;
            char *tmp = realloc(cap->literal.data,
                                cap->literal.size + to_copy + 1);
            if (tmp) {
                cap->literal.data = tmp;
                memcpy(cap->literal.data + cap->literal.size, p, to_copy);
                cap->literal.size += to_copy;
                cap->literal.data[cap->literal.size] = '\0';
            }
            cap->bytes_left -= (long)to_copy;
            p               += to_copy;
            if (cap->bytes_left <= 0) {
                cap->in_literal = 0;
                break;  /* literal complete — stop processing this chunk */
            }
        } else {
            break;
        }
    }

    return 0;
}

/** Fetches headers or full message for a UID in <folder>.  Caller must free.
 *
 *  Issues BODY.PEEK[HEADER] or BODY.PEEK[] — both NEVER set the \Seen flag
 *  per RFC 3501 §6.4.5.  When CURLOPT_CUSTOMREQUEST is active, libcurl routes
 *  all server data to CURLINFO_DATA_IN rather than WRITEFUNCTION.  We capture
 *  the message literal via peek_debug_cb which intercepts CURLINFO_DATA_IN. */
static char *fetch_uid_content_in(const Config *cfg, const char *folder,
                                  int uid, int headers_only) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return NULL;

    RAII_STRING char *url = NULL;
    RAII_STRING char *cmd = NULL;
    if (asprintf(&url, "%s/%s", cfg->host, folder) == -1) return NULL;
    if (headers_only) {
        if (asprintf(&cmd, "UID FETCH %d BODY.PEEK[HEADER]", uid) == -1) return NULL;
    } else {
        if (asprintf(&cmd, "UID FETCH %d BODY.PEEK[]", uid) == -1) return NULL;
    }

    PeekCapture cap = {{NULL, 0}, 0, 0};
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, peek_debug_cb);
    curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &cap);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cmd);

    /* WRITEFUNCTION is set but receives only the FETCH envelope (ignored). */
    Buffer envelope = {NULL, 0};
    CURLcode res = curl_adapter_fetch(curl, url, &envelope, buffer_append);
    free(envelope.data);

    if (res != CURLE_OK) {
        logger_log(LOG_WARN, "Fetch UID %d failed: %s", uid, curl_easy_strerror(res));
        free(cap.literal.data);
        return NULL;
    }
    if (!cap.literal.data || cap.literal.size == 0) {
        logger_log(LOG_WARN, "BODY.PEEK fetch UID %d: no literal captured", uid);
        free(cap.literal.data);
        return NULL;
    }
    return cap.literal.data;
}

/* ── Cached header fetch ─────────────────────────────────────────────── */

/** Fetches headers for uid/folder, using the header cache. Caller must free. */
static char *fetch_uid_headers_cached(const Config *cfg, const char *folder,
                                       int uid) {
    if (hcache_exists(folder, uid))
        return hcache_load(folder, uid);
    char *hdrs = fetch_uid_content_in(cfg, folder, uid, 1);
    if (hdrs)
        hcache_save(folder, uid, hdrs, strlen(hdrs));
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

/**
 * Show a message in interactive pager mode.
 * Returns 0 = ESC (go back to list), 1 = q/quit entirely, -1 = error.
 */
static int show_uid_interactive(const Config *cfg, int uid, int page_size) {
    char *raw = NULL;
    if (cache_exists(cfg->folder, uid)) {
        raw = cache_load(cfg->folder, uid);
    } else {
        raw = fetch_uid_content_in(cfg, cfg->folder, uid, 0);
        if (raw)
            cache_save(cfg->folder, uid, raw, strlen(raw));
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
    int wrap_cols = terminal_cols();
    if (wrap_cols > SHOW_WIDTH) wrap_cols = SHOW_WIDTH;
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

#define SHOW_HDR_LINES_INT 5
    int rows_avail  = (page_size > SHOW_HDR_LINES_INT)
                      ? page_size - SHOW_HDR_LINES_INT : 1;
    int body_lines  = count_lines(body_text);
    int total_pages = (body_lines + rows_avail - 1) / rows_avail;
    if (total_pages < 1) total_pages = 1;

    int result = 0;
    for (int cur_line = 0;;) {
        printf("\033[0m\033[H\033[2J");     /* reset attrs + clear screen */
        print_show_headers(from, subject, date);
        print_body_page(body_text, cur_line, rows_avail);
        printf("\033[0m");                  /* close any open ANSI from body */
        fflush(stdout);

        int cur_page = cur_line / rows_avail + 1;
        fprintf(stderr, "\033[%d;1H\033[7m\033[2K", page_size); /* last row + reverse + erase */
        fprintf(stderr,
                "-- [%d/%d] PgDn/\u2193=scroll  PgUp/\u2191=back"
                "  Backspace=list  ESC=quit --\033[0m",
                cur_page, total_pages);
        fflush(stderr);

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
            if (next < body_lines) cur_line = next;
            /* else: already on last page — stay */
        }
            break;
        case TERM_KEY_ENTER:
            break;               /* Enter does not scroll */
        case TERM_KEY_PREV_PAGE:
            cur_line -= rows_avail;
            if (cur_line < 0) cur_line = 0;
            break;
        case TERM_KEY_NEXT_LINE:
            if (cur_line < body_lines - 1) cur_line++;
            break;
        case TERM_KEY_PREV_LINE:
            if (cur_line > 0) cur_line--;
            break;
        case TERM_KEY_IGNORE:
            break;
        }
    }
show_int_done:
#undef SHOW_HDR_LINES_INT
    free(body); free(body_wrapped); free(from); free(subject); free(date); free(raw);
    return result;
}

/* ── List helpers ────────────────────────────────────────────────────── */

typedef struct { int uid; int unseen; } UIDEntry;

static int cmp_uid_entry(const void *a, const void *b) {
    const UIDEntry *ea = a, *eb = b;
    if (ea->unseen != eb->unseen)
        return eb->unseen - ea->unseen;   /* unseen first */
    return eb->uid - ea->uid;             /* newer (higher UID) first */
}

/* ── Folder list helpers ─────────────────────────────────────────────── */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/**
 * Parses one "* LIST (\flags) sep mailbox" line.
 * Sets *sep_out to the hierarchy separator character.
 * Returns a heap-allocated mailbox name, or NULL.
 */
static char *parse_list_line(const char *line, char *sep_out) {
    if (strncmp(line, "* LIST", 6) != 0) return NULL;
    const char *p = line + 6;
    while (*p == ' ') p++;

    /* Skip flags (...) */
    if (*p == '(') { while (*p && *p != ')') p++; if (*p) p++; }
    while (*p == ' ') p++;

    /* Separator field */
    char sep = '.';
    if (*p == '"') {
        p++;
        if (*p != '"') sep = *p;
        while (*p && *p != '"') p++;
        if (*p) p++;
    } else {
        if (*p && *p != ' ') sep = *p++;
    }
    if (sep_out) *sep_out = sep;
    while (*p == ' ') p++;

    /* Mailbox name */
    if (*p == '"') {
        p++;
        const char *s = p;
        while (*p && *p != '"') p++;
        return strndup(s, (size_t)(p - s));
    }
    const char *s = p;
    while (*p && *p != '\r' && *p != '\n') p++;
    return strndup(s, (size_t)(p - s));
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
static void print_folder_item(char **names, int count, int i, char sep,
                               int tree_mode, int selected, int has_kids) {
    if (selected) printf("\033[7m");

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
    } else {
        const char *comp = strrchr(names[i], sep);
        const char *display = comp ? comp + 1 : names[i];
        printf("  %s%s", display, has_kids ? "/" : "");
    }

    if (selected) printf("\033[K\033[0m");
    printf("\n");
}

static void render_folder_tree(char **names, int count, char sep) {
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
        printf("%s\n", comp ? comp + 1 : names[i]);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

int email_service_list(const Config *cfg, const EmailListOpts *opts) {
    const char *folder = opts->folder ? opts->folder : cfg->folder;
    int list_result = 0;

    printf("--- Fetching emails: %s @ %s/%s ---\n",
           cfg->user, cfg->host, folder);

    /* Fetch UNSEEN set (for N markers) and ALL messages */
    int *unseen_uids = NULL;
    int unseen_count = search_uids_in(cfg, folder, "UNSEEN", &unseen_uids);
    if (unseen_count < 0) { fprintf(stderr, "Failed to search mailbox.\n"); return -1; }

    int *all_uids  = NULL;
    int  all_count = search_uids_in(cfg, folder, "ALL", &all_uids);
    if (all_count < 0) {
        free(unseen_uids);
        fprintf(stderr, "Failed to search mailbox.\n");
        return -1;
    }
    /* Evict headers for messages deleted from the server */
    if (all_count > 0)
        hcache_evict_stale(folder, all_uids, all_count);

    int show_count = all_count;

    if (show_count == 0) {
        free(unseen_uids);
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
    UIDEntry *entries = malloc((size_t)show_count * sizeof(UIDEntry));
    if (!entries) { free(unseen_uids); free(all_uids); return -1; }

    for (int i = 0; i < show_count; i++) {
        entries[i].uid    = all_uids[i];
        entries[i].unseen = 0;
        for (int j = 0; j < unseen_count; j++) {
            if (unseen_uids[j] == all_uids[i]) { entries[i].unseen = 1; break; }
        }
    }
    free(unseen_uids);
    free(all_uids);

    /* Sort: unseen first, then descending UID */
    qsort(entries, (size_t)show_count, sizeof(UIDEntry), cmp_uid_entry);

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
         * Fixed overhead per data row: "  S  UID  " (12) + "  " (2) + "  DATE" (18) = 32
         * from_w and subj_w share the remaining space evenly (min 20 each). */
        int tcols    = terminal_cols();
        int overhead = 32;
        int avail    = tcols - overhead;
        if (avail < 40) avail = 40;
        int from_w = avail / 2;
        int subj_w = avail - from_w;

        if (opts->pager) printf("\033[H\033[2J");

        /* Count / status line */
        printf("%d-%d of %d message(s) in %s (%d unread).\n\n",
               wstart + 1, wend, show_count, folder, unseen_count);
        printf("  S  %5s  %-*s  %-*s  %s\n",
               "UID", from_w, "From", subj_w, "Subject", "Date");
        printf("  \u2550  \u2550\u2550\u2550\u2550\u2550  ");
        print_dbar(from_w); printf("  ");
        print_dbar(subj_w); printf("  ");
        print_dbar(16);     printf("\n");

        /* Data rows */
        for (int i = wstart; i < wend; i++) {
            char *hdrs     = fetch_uid_headers_cached(cfg, folder, entries[i].uid);
            char *from_raw = hdrs ? mime_get_header(hdrs, "From")    : NULL;
            char *from     = from_raw ? mime_decode_words(from_raw)  : NULL;
            free(from_raw);
            char *subj_raw = hdrs ? mime_get_header(hdrs, "Subject") : NULL;
            char *subject  = subj_raw ? mime_decode_words(subj_raw)  : NULL;
            free(subj_raw);
            char *date_raw = hdrs ? mime_get_header(hdrs, "Date")    : NULL;
            char *date     = date_raw ? mime_format_date(date_raw)   : NULL;
            free(date_raw);

            int sel = opts->pager && (i == cursor);
            if (sel) printf("\033[7m");

            printf("  %c  %5d  ", entries[i].unseen ? 'N' : ' ', entries[i].uid);
            print_padded_col(from    ? from    : "(no from)",    from_w);
            printf("  ");
            print_padded_col(subject ? subject : "(no subject)", subj_w);
            printf("  %-16.16s", date ? date : "");

            if (sel) printf("\033[K\033[0m");
            printf("\n");
            free(hdrs); free(from); free(subject); free(date);
        }

        if (!opts->pager) {
            if (wend < show_count)
                printf("\n  -- %d more message(s) --  use --offset %d for next page\n",
                       show_count - wend, wend + 1);
            break;
        }

        /* Navigation hint (status bar) */
        printf("\n\033[2m  \u2191\u2193=step  PgDn/PgUp=page  Enter=open"
               "  Backspace=folders  ESC=quit  [%d/%d]\033[0m\n",
               cursor + 1, show_count);
        fflush(stdout);

        TermKey key = terminal_read_key();

        switch (key) {
        case TERM_KEY_BACK:
            list_result = 1;
            goto list_done;
        case TERM_KEY_QUIT:
        case TERM_KEY_ESC:
            goto list_done;
        case TERM_KEY_ENTER:
            {
                int ret = show_uid_interactive(cfg, entries[cursor].uid, opts->limit);
                if (ret == 1) goto list_done;  /* user quit from show */
                /* ret == 0: Backspace → back to list; ret == -1: error → stay */
            }
            break;
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
        case TERM_KEY_IGNORE:
            break;
        }
    }
list_done:
    /* tui_raw is cleaned up automatically via RAII_TERM_RAW */
    free(entries);
    return list_result;
}

/** Fetch the folder list into a heap-allocated array; caller owns entries and array. */
static char **fetch_folder_list(const Config *cfg, int *count_out, char *sep_out) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return NULL;

    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/", cfg->host) == -1) return NULL;

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "LIST \"\" \"*\"");

    Buffer buf = {NULL, 0};
    CURLcode res = curl_adapter_fetch(curl, url, &buf, buffer_append);
    if (res != CURLE_OK) {
        logger_log(LOG_WARN, "LIST failed: %s", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }

    char **folders = NULL;
    int count = 0, cap = 0;
    char sep = '.';

    const char *p = buf.data;
    while (p && *p) {
        char got_sep = '.';
        char *raw_name = parse_list_line(p, &got_sep);
        char *name = raw_name ? imap_utf7_decode(raw_name) : NULL;
        free(raw_name);
        if (name) {
            sep = got_sep;
            if (count == cap) {
                cap = cap ? cap * 2 : 16;
                char **tmp = realloc(folders, (size_t)cap * sizeof(char *));
                if (!tmp) { free(name); break; }
                folders = tmp;
            }
            folders[count++] = name;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    free(buf.data);

    *count_out = count;
    if (sep_out) *sep_out = sep;
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

    if (tree)
        render_folder_tree(folders, count, sep);
    else
        for (int i = 0; i < count; i++) printf("%s\n", folders[i]);

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

    int *vis = malloc((size_t)count * sizeof(int));
    if (!vis) {
        for (int i = 0; i < count; i++) free(folders[i]);
        free(folders);
        return NULL;
    }

    int cursor = 0, wstart = 0;
    int tree_mode = ui_pref_get_int("folder_view_mode", 1);
    char current_prefix[512] = "";   /* flat mode: current navigation level */

    /* In flat mode, pre-navigate to the level that contains current_folder */
    if (!tree_mode && current_folder && *current_folder) {
        const char *last = strrchr(current_folder, sep);
        if (last) {
            size_t plen = (size_t)(last - current_folder);
            if (plen < sizeof(current_prefix)) {
                memcpy(current_prefix, current_folder, plen);
                current_prefix[plen] = '\0';
            }
        }
        /* position cursor on current_folder */
        int tmp_vis[1024];
        int tv = build_flat_view(folders, count, sep, current_prefix, tmp_vis);
        for (int i = 0; i < tv; i++) {
            if (strcmp(folders[tmp_vis[i]], current_folder) == 0) {
                cursor = i; break;
            }
        }
    }
    int vcount = 0;                  /* flat view: number of visible entries */
    char *selected = NULL;

    RAII_TERM_RAW TermRawState *tui_raw = terminal_raw_enter();

    for (;;) {
        int rows  = terminal_rows();
        int limit = (rows > 4) ? rows - 3 : 10;

        /* Rebuild flat view on each iteration */
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

        printf("\033[H\033[2J");
        if (!tree_mode && current_prefix[0])
            printf("Folders: %s/ (%d)\n\n", current_prefix, display_count);
        else
            printf("Folders (%d)\n\n", display_count);

        for (int i = wstart; i < wend; i++) {
            if (tree_mode) {
                print_folder_item(folders, count, i, sep, 1, i == cursor, 0);
            } else {
                int fi = vis[i];
                int hk = folder_has_children(folders, count, folders[fi], sep);
                print_folder_item(folders, count, fi, sep, 0, i == cursor, hk);
            }
        }

        if (!tree_mode && current_prefix[0])
            printf("\n\033[2m  \u2191\u2193=step  PgDn/PgUp=page  Enter=open/select"
                   "  t=tree  Backspace=up  ESC=quit  [%d/%d]\033[0m\n",
                   display_count > 0 ? cursor + 1 : 0, display_count);
        else
            printf("\n\033[2m  \u2191\u2193=step  PgDn/PgUp=page  Enter=open/select"
                   "  t=%s  Backspace/ESC=quit  [%d/%d]\033[0m\n",
                   tree_mode ? "flat" : "tree",
                   display_count > 0 ? cursor + 1 : 0, display_count);
        fflush(stdout);

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
    free(vis);
    for (int i = 0; i < count; i++) free(folders[i]);
    free(folders);
    return selected;
}

int email_service_read(const Config *cfg, int uid, int pager, int page_size) {
    char *raw = NULL;

    if (cache_exists(cfg->folder, uid)) {
        logger_log(LOG_DEBUG, "Cache hit for UID %d in %s", uid, cfg->folder);
        raw = cache_load(cfg->folder, uid);
    } else {
        raw = fetch_uid_content_in(cfg, cfg->folder, uid, 0);
        if (raw)
            cache_save(cfg->folder, uid, raw, strlen(raw));
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

    int wrap_cols = pager ? terminal_cols() : SHOW_WIDTH;
    if (wrap_cols > SHOW_WIDTH) wrap_cols = SHOW_WIDTH;

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
        int body_lines  = count_lines(body_text);
        int rows_avail  = page_size - SHOW_HDR_LINES;
        int total_pages = (body_lines + rows_avail - 1) / rows_avail;
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
            print_body_page(body_text, cur_line, rows_avail);
            printf("\033[0m");                     /* close any open ANSI from body */
            fflush(stdout);

            if (cur_line == 0 && cur_line + rows_avail >= body_lines) break;

            int cur_page = cur_line / rows_avail + 1;
            int delta = pager_prompt(cur_page, total_pages, rows_avail, page_size);
            if (delta == 0) break;
            cur_line += delta;
            if (cur_line < 0) cur_line = 0;
            if (cur_line >= body_lines) break;
        }
        (void)show_raw; /* cleaned up automatically via RAII_TERM_RAW */
    }
#undef SHOW_HDR_LINES

    free(body); free(from); free(subject); free(date); free(raw);
    return 0;
}

