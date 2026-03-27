#include "email_service.h"
#include "curl_adapter.h"
#include "cache_store.h"
#include "mime_util.h"
#include "imap_util.h"
#include "raii.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

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

/* ── Interactive pager helpers ───────────────────────────────────────── */

/* Logical key codes returned by read_pager_key(). */
enum PKey {
    PKEY_QUIT      = 0,  /* q / Q / Ctrl-C — quit entirely            */
    PKEY_NEXT_PAGE = 1,  /* PgDn / Space                              */
    PKEY_PREV_PAGE = 2,  /* PgUp / p / P / b                          */
    PKEY_NEXT_LINE = 3,  /* Down-arrow / j                            */
    PKEY_PREV_LINE = 4,  /* Up-arrow / k                              */
    PKEY_IGNORE    = 5,  /* Left/Right arrows and unknown sequences   */
    PKEY_ENTER     = 6,  /* Enter / Return                            */
    PKEY_ESC       = 7   /* Bare ESC — "go back / close current view" */
};

/**
 * Read one byte from STDIN_FILENO via read(2) — NOT via getchar()/stdio.
 *
 * Rationale: getchar() uses the C stdio buffer.  When getchar() calls
 * read(2) with VMIN=0 VTIME=1 and gets a 0-byte return (timeout for bare
 * ESC), stdio marks the FILE* EOF flag.  All subsequent getchar() calls
 * then return EOF immediately without blocking, causing an infinite
 * redraw loop in the TUI.  Using read(2) directly bypasses the stdio
 * layer entirely and avoids the EOF flag problem.
 *
 * Returns the byte value [0..255], or -1 on error/timeout.
 */
static int read_byte(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    return (n == 1) ? (int)c : -1;
}

/**
 * Reads one keypress in raw (non-canonical) mode and returns a PKey code.
 * Multi-byte escape sequences are fully consumed before returning.
 */
static enum PKey read_pager_key(void) {
    struct termios old, raw;
    if (tcgetattr(STDIN_FILENO, &old) != 0) return PKEY_QUIT;
    raw = old;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    int c = read_byte();
    enum PKey result = PKEY_IGNORE;   /* unknown input → silent no-op */

    if (c == '\033') {
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 1;   /* 100 ms timeout for escape sequence drain */
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);

        int c2 = read_byte();
        if (c2 == '[') {
            int c3 = read_byte();
            switch (c3) {
            case 'A': result = PKEY_PREV_LINE; break;  /* ESC[A — Up arrow    */
            case 'B': result = PKEY_NEXT_LINE; break;  /* ESC[B — Down arrow  */
            case 'C': result = PKEY_IGNORE;    break;  /* ESC[C — Right arrow */
            case 'D': result = PKEY_IGNORE;    break;  /* ESC[D — Left arrow  */
            case '5': read_byte(); result = PKEY_PREV_PAGE; break; /* ESC[5~ PgUp */
            case '6': read_byte(); result = PKEY_NEXT_PAGE; break; /* ESC[6~ PgDn */
            default:
                if (c3 != -1) {
                    int ch;
                    while ((ch = read_byte()) != -1) {
                        if ((ch >= 'A' && ch <= 'Z') ||
                            (ch >= 'a' && ch <= 'z') || ch == '~') break;
                    }
                }
                result = PKEY_IGNORE;
                break;
            }
        } else {
            result = PKEY_ESC;   /* bare ESC — go back */
        }
    } else if (c == '\n' || c == '\r') {
        result = PKEY_ENTER;
    } else if (c == 'q' || c == 'Q' || c == 3 /* Ctrl-C */) {
        result = PKEY_QUIT;
    } else if (c == 'p' || c == 'P' || c == 'b') {
        result = PKEY_PREV_PAGE;
    } else if (c == ' ') {
        result = PKEY_NEXT_PAGE;
    } else if (c == 'j') {
        result = PKEY_NEXT_LINE;
    } else if (c == 'k') {
        result = PKEY_PREV_LINE;
    }
    /* c == -1 (read error/timeout) → result stays PKEY_IGNORE */

    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    return result;
}

/**
 * Pager prompt for the standalone `show` command.
 * PKEY_ENTER and PKEY_ESC are treated the same as next/quit respectively.
 * Returns scroll delta: 0 = quit, positive = forward N lines, negative = back N.
 */
static int pager_prompt(int cur_page, int total_pages, int page_size) {
    for (;;) {
        fprintf(stderr,
                "\033[7m-- [%d/%d] PgDn/\u2193=scroll  PgUp/\u2191=back  q=quit --\033[0m",
                cur_page, total_pages);
        fflush(stderr);
        enum PKey key = read_pager_key();
        fprintf(stderr, "\r\033[K");
        fflush(stderr);

        switch (key) {
        case PKEY_QUIT:
        case PKEY_ESC:       return 0;           /* ESC = quit in standalone mode */
        case PKEY_ENTER:
        case PKEY_NEXT_PAGE: return  page_size;
        case PKEY_PREV_PAGE: return -page_size;
        case PKEY_NEXT_LINE: return  1;
        case PKEY_PREV_LINE: return -1;
        case PKEY_IGNORE:    continue;
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
 * Print lines [from_line, from_line+line_count) from body.
 * Lines are 0-indexed.
 */
static void print_body_page(const char *body, int from_line, int line_count) {
    int line = 0;
    const char *p = body;
    while (*p && line < from_line) {   /* skip to from_line */
        if (*p++ == '\n') line++;
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

/** Returns 1 if the message has the \Seen flag set, 0 otherwise. */
static int check_uid_seen(const Config *cfg, const char *folder, int uid) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return 0;
    RAII_STRING char *url = NULL;
    RAII_STRING char *cmd = NULL;
    if (asprintf(&url, "%s/%s", cfg->host, folder) == -1) return 0;
    if (asprintf(&cmd, "UID FETCH %d (FLAGS)", uid) == -1) return 0;
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cmd);
    Buffer buf = {NULL, 0};
    CURLcode res = curl_adapter_fetch(curl, url, &buf, buffer_append);
    int seen = (res == CURLE_OK && buf.data && strstr(buf.data, "\\Seen") != NULL);
    free(buf.data);
    return seen;
}

/** Removes the \Seen flag from a message (restore unread state). */
static void restore_uid_unseen(const Config *cfg, const char *folder, int uid) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return;
    RAII_STRING char *url = NULL;
    RAII_STRING char *cmd = NULL;
    if (asprintf(&url, "%s/%s", cfg->host, folder) == -1) return;
    if (asprintf(&cmd, "UID STORE %d -FLAGS (\\Seen)", uid) == -1) return;
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cmd);
    Buffer buf = {NULL, 0};
    CURLcode res = curl_adapter_fetch(curl, url, &buf, buffer_append);
    if (res != CURLE_OK)
        logger_log(LOG_WARN, "Failed to restore \\Seen for UID %d: %s",
                   uid, curl_easy_strerror(res));
    free(buf.data);
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

/** Fetches headers or full message for a UID in <folder>. Caller must free.
 *
 *  headers_only=1: URL-based BODY[HEADER] fetch — does NOT set \Seen (RFC 3501).
 *  headers_only=0: URL-based fetch (sets \Seen transiently); \Seen is restored
 *                  afterward if the message was not already read.
 *                  Note: CURLOPT_CUSTOMREQUEST cannot be used for full-body IMAP
 *                  FETCH because libcurl does not deliver the literal payload to
 *                  the write callback in that mode. */
static char *fetch_uid_content_in(const Config *cfg, const char *folder,
                                  int uid, int headers_only) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return NULL;

    RAII_STRING char *url = NULL;

    /* Check \Seen status before the fetch so we can restore it if needed. */
    int was_seen = 0;
    if (!headers_only)
        was_seen = check_uid_seen(cfg, folder, uid);

    if (headers_only) {
        if (asprintf(&url, "%s/%s/;UID=%d/;SECTION=HEADER",
                     cfg->host, folder, uid) == -1) return NULL;
    } else {
        /* Standard URL fetch — libcurl sends BODY[] and delivers the full
         * literal to the write callback.  This sets \Seen; we restore it below
         * when the message was not already read. */
        if (asprintf(&url, "%s/%s/;UID=%d", cfg->host, folder, uid) == -1) return NULL;
    }

    Buffer buf = {NULL, 0};
    CURLcode res = curl_adapter_fetch(curl, url, &buf, buffer_append);
    if (res != CURLE_OK) {
        logger_log(LOG_WARN, "Fetch UID %d failed: %s", uid, curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }

    if (!headers_only && !was_seen)
        restore_uid_unseen(cfg, folder, uid);

    return buf.data;
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

#define SHOW_SEPARATOR \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" \
    "\u2500\u2500\u2500\u2500\u2500\n"

static void print_show_headers(const char *from, const char *subject,
                                const char *date) {
    printf("From:    %s\n", from    ? from    : "(none)");
    printf("Subject: %s\n", subject ? subject : "(none)");
    printf("Date:    %s\n", date    ? date    : "(none)");
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

    char *from     = mime_get_header(raw, "From");
    char *subject  = mime_get_header(raw, "Subject");
    char *date_raw = mime_get_header(raw, "Date");
    char *date     = date_raw ? mime_format_date(date_raw) : NULL;
    free(date_raw);
    char *body = mime_get_text_body(raw);
    const char *body_text = body ? body : "(no readable text body)";

#define SHOW_HDR_LINES_INT 5
    int rows_avail  = (page_size > SHOW_HDR_LINES_INT)
                      ? page_size - SHOW_HDR_LINES_INT : 1;
    int body_lines  = count_lines(body_text);
    int total_pages = (body_lines + rows_avail - 1) / rows_avail;
    if (total_pages < 1) total_pages = 1;

    int result = 0;
    for (int cur_line = 0;;) {
        printf("\033[H\033[2J");
        print_show_headers(from, subject, date);
        print_body_page(body_text, cur_line, rows_avail);

        int cur_page = cur_line / rows_avail + 1;
        fprintf(stderr,
                "\033[7m-- [%d/%d] PgDn/\u2193=scroll  PgUp/\u2191=back"
                "  ESC=list  q=quit --\033[0m",
                cur_page, total_pages);
        fflush(stderr);

        enum PKey key = read_pager_key();
        fprintf(stderr, "\r\033[K");
        fflush(stderr);

        switch (key) {
        case PKEY_ESC:
            result = 0;
            goto show_int_done;
        case PKEY_QUIT:
            result = 1;
            goto show_int_done;
        case PKEY_ENTER:
        case PKEY_NEXT_PAGE:
        {
            int next = cur_line + rows_avail;
            if (next < body_lines) cur_line = next;
            /* else: already on last page — stay */
        }
            break;
        case PKEY_PREV_PAGE:
            cur_line -= rows_avail;
            if (cur_line < 0) cur_line = 0;
            break;
        case PKEY_NEXT_LINE:
            if (cur_line < body_lines - 1) cur_line++;
            break;
        case PKEY_PREV_LINE:
            if (cur_line > 0) cur_line--;
            break;
        case PKEY_IGNORE:
            break;
        }
    }
show_int_done:
#undef SHOW_HDR_LINES_INT
    free(body); free(from); free(subject); free(date); free(raw);
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

    printf("--- Fetching emails: %s @ %s/%s ---\n",
           cfg->user, cfg->host, folder);

    /* Always fetch UNSEEN set */
    int *unseen_uids = NULL;
    int unseen_count = search_uids_in(cfg, folder, "UNSEEN", &unseen_uids);
    if (unseen_count < 0) { fprintf(stderr, "Failed to search mailbox.\n"); return -1; }

    int *all_uids  = NULL;
    int  all_count = 0;
    if (opts->all) {
        all_count = search_uids_in(cfg, folder, "ALL", &all_uids);
        if (all_count < 0) {
            free(unseen_uids);
            fprintf(stderr, "Failed to search mailbox.\n");
            return -1;
        }
        /* Evict headers for messages deleted from the server */
        if (all_count > 0)
            hcache_evict_stale(folder, all_uids, all_count);
    }

    int  show_count = opts->all ? all_count : unseen_count;
    int *show_uids  = opts->all ? all_uids  : unseen_uids;

    if (show_count == 0) {
        printf(opts->all ? "No messages in %s.\n"
                         : "No unread messages in %s.\n", folder);
        free(unseen_uids);
        free(all_uids);
        return 0;
    }

    /* Build tagged entry array */
    UIDEntry *entries = malloc((size_t)show_count * sizeof(UIDEntry));
    if (!entries) { free(unseen_uids); free(all_uids); return -1; }

    for (int i = 0; i < show_count; i++) {
        entries[i].uid    = show_uids[i];
        entries[i].unseen = 0;
        if (!opts->all) {
            entries[i].unseen = 1; /* UNSEEN-only mode: all are unread */
        } else {
            for (int j = 0; j < unseen_count; j++) {
                if (unseen_uids[j] == show_uids[i]) { entries[i].unseen = 1; break; }
            }
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
     * Without this, each read_pager_key() call briefly restores cooked mode
     * between keystrokes, which causes the terminal to echo escape sequences
     * and to buffer input until Enter (cooked ICANON), making the interface
     * appear frozen.  Ctrl+C works in that state only because cooked mode
     * delivers ISIG signals — the exact symptom the user observed. */
    struct termios tui_saved;
    int tui_raw = 0;
    if (opts->pager && tcgetattr(STDIN_FILENO, &tui_saved) == 0) {
        struct termios raw = tui_saved;
        raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN]  = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        tui_raw = 1;
    }

    for (;;) {
        /* Scroll window to keep cursor visible */
        if (cursor < wstart)             wstart = cursor;
        if (cursor >= wstart + limit)    wstart = cursor - limit + 1;
        if (wstart < 0)                  wstart = 0;
        int wend = wstart + limit;
        if (wend > show_count)           wend = show_count;

        if (opts->pager) printf("\033[H\033[2J");

        /* Count / status line */
        if (opts->all) {
            printf("%d-%d of %d message(s) in %s (%d unread).\n\n",
                   wstart + 1, wend, show_count, folder, unseen_count);
            printf("  S  %5s  %-30s  %-30s  %s\n",
                   "UID", "From", "Subject", "Date");
            printf("  \u2550  %s  %s  %s  %s\n", "═════",
                   "══════════════════════════════",
                   "══════════════════════════════",
                   "═══════════════════════════");
        } else {
            printf("%d-%d of %d unread message(s) in %s.\n\n",
                   wstart + 1, wend, show_count, folder);
            printf("  %5s  %-30s  %-30s  %s\n",
                   "UID", "From", "Subject", "Date");
            printf("  %s  %s  %s  %s\n", "═════",
                   "══════════════════════════════",
                   "══════════════════════════════",
                   "═══════════════════════════");
        }

        /* Data rows */
        for (int i = wstart; i < wend; i++) {
            char *hdrs     = fetch_uid_headers_cached(cfg, folder, entries[i].uid);
            char *from     = hdrs ? mime_get_header(hdrs, "From")    : NULL;
            char *subject  = hdrs ? mime_get_header(hdrs, "Subject") : NULL;
            char *date_raw = hdrs ? mime_get_header(hdrs, "Date")    : NULL;
            char *date     = date_raw ? mime_format_date(date_raw)   : NULL;
            free(date_raw);

            int sel = opts->pager && (i == cursor);
            if (sel) printf("\033[7m");

            if (opts->all)
                printf("  %c  %5d  %-30.30s  %-30.30s  %-16.16s",
                       entries[i].unseen ? 'N' : ' ',
                       entries[i].uid,
                       from    ? from    : "(no from)",
                       subject ? subject : "(no subject)",
                       date    ? date    : "");
            else
                printf("  %5d  %-30.30s  %-30.30s  %-16.16s",
                       entries[i].uid,
                       from    ? from    : "(no from)",
                       subject ? subject : "(no subject)",
                       date    ? date    : "");

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
        printf("\n\033[2m  \u2191\u2193=step  PgDn/PgUp=page  Enter=open  q=quit"
               "  [%d/%d]\033[0m\n",
               cursor + 1, show_count);
        fflush(stdout);

        enum PKey key = read_pager_key();

        switch (key) {
        case PKEY_QUIT:
        case PKEY_ESC:
            goto list_done;
        case PKEY_ENTER:
            {
                int ret = show_uid_interactive(cfg, entries[cursor].uid, opts->limit);
                if (ret == 1) goto list_done;  /* user pressed q in show */
                /* ret == 0: ESC → back to list; ret == -1: error → stay */
            }
            break;
        case PKEY_NEXT_LINE:
            if (cursor < show_count - 1) cursor++;
            break;
        case PKEY_PREV_LINE:
            if (cursor > 0) cursor--;
            break;
        case PKEY_NEXT_PAGE:
            cursor += limit;
            if (cursor >= show_count) cursor = show_count - 1;
            break;
        case PKEY_PREV_PAGE:
            cursor -= limit;
            if (cursor < 0) cursor = 0;
            break;
        case PKEY_IGNORE:
            break;
        }
    }
list_done:
    if (tui_raw)
        tcsetattr(STDIN_FILENO, TCSANOW, &tui_saved);
    free(entries);
    return 0;
}

int email_service_list_folders(const Config *cfg, int tree) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return -1;

    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/", cfg->host) == -1) return -1;

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "LIST \"\" \"*\"");

    Buffer buf = {NULL, 0};
    CURLcode res = curl_adapter_fetch(curl, url, &buf, buffer_append);
    if (res != CURLE_OK) {
        logger_log(LOG_WARN, "LIST failed: %s", curl_easy_strerror(res));
        free(buf.data);
        return -1;
    }

    /* Parse folder names */
    char **folders = NULL;
    int    count   = 0, cap = 0;
    char   sep     = '.';

    const char *p = buf.data;
    while (p && *p) {
        char got_sep = '.';
        char *raw_name = parse_list_line(p, &got_sep);
        char *name     = raw_name ? imap_utf7_decode(raw_name) : NULL;
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

    if (count == 0) {
        printf("No folders found.\n");
        free(folders);
        return 0;
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

    char *from     = mime_get_header(raw, "From");
    char *subject  = mime_get_header(raw, "Subject");
    char *date_raw = mime_get_header(raw, "Date");
    char *date     = date_raw ? mime_format_date(date_raw) : NULL;
    free(date_raw);

    print_show_headers(from, subject, date);

    char *body = mime_get_text_body(raw);
    const char *body_text = body ? body : "(no readable text body)";

#define SHOW_HDR_LINES 5
    if (!pager || page_size <= SHOW_HDR_LINES) {
        printf("%s\n", body_text);
    } else {
        int body_lines  = count_lines(body_text);
        int rows_avail  = page_size - SHOW_HDR_LINES;
        int total_pages = (body_lines + rows_avail - 1) / rows_avail;
        if (total_pages < 1) total_pages = 1;

        for (int cur_line = 0, show_displayed = 0; ; ) {
            if (show_displayed) {
                printf("\033[H\033[2J");
                print_show_headers(from, subject, date);
            }
            show_displayed = 1;
            print_body_page(body_text, cur_line, rows_avail);

            if (cur_line == 0 && cur_line + rows_avail >= body_lines) break;

            int cur_page = cur_line / rows_avail + 1;
            int delta = pager_prompt(cur_page, total_pages, rows_avail);
            if (delta == 0) break;
            cur_line += delta;
            if (cur_line < 0) cur_line = 0;
            if (cur_line >= body_lines) break;
        }
    }
#undef SHOW_HDR_LINES

    free(body); free(from); free(subject); free(date); free(raw);
    return 0;
}
