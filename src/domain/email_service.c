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

/* ── IMAP helpers ────────────────────────────────────────────────────── */

static CURL *make_curl(const Config *cfg) {
    return curl_adapter_init(cfg->user, cfg->pass, !cfg->ssl_no_verify);
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

/** Fetches headers or full message for a UID in <folder>. Caller must free. */
static char *fetch_uid_content_in(const Config *cfg, const char *folder,
                                  int uid, int headers_only) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return NULL;

    RAII_STRING char *url = NULL;
    int rc = headers_only
        ? asprintf(&url, "%s/%s/;UID=%d/;SECTION=HEADER", cfg->host, folder, uid)
        : asprintf(&url, "%s/%s/;UID=%d", cfg->host, folder, uid);
    if (rc == -1) return NULL;

    Buffer buf = {NULL, 0};
    CURLcode res = curl_adapter_fetch(curl, url, &buf, buffer_append);
    if (res != CURLE_OK) {
        logger_log(LOG_WARN, "Fetch UID %d failed: %s", uid, curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data;
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

    /* Apply offset/limit */
    int start = (opts->offset > 1) ? opts->offset - 1 : 0;
    if (start >= show_count) {
        printf("No messages at offset %d (total: %d).\n", opts->offset, show_count);
        free(entries);
        return 0;
    }
    int end = show_count;
    if (opts->limit > 0 && start + opts->limit < end)
        end = start + opts->limit;

    /* Header */
    if (opts->all) {
        if (start > 0 || end < show_count)
            printf("%d-%d of %d message(s) in %s (%d unread).\n\n",
                   start + 1, end, show_count, folder, unseen_count);
        else
            printf("%d message(s) in %s (%d unread).\n\n",
                   show_count, folder, unseen_count);
        printf("  S  %5s  %-30s  %-30s  %s\n",
               "UID", "From", "Subject", "Date");
        printf("  \u2550  %s  %s  %s  %s\n", "═════",
               "══════════════════════════════",
               "══════════════════════════════",
               "═══════════════════════════");
    } else {
        if (start > 0 || end < show_count)
            printf("%d-%d of %d unread message(s) in %s.\n\n",
                   start + 1, end, show_count, folder);
        else
            printf("%d unread message(s) in %s.\n\n", show_count, folder);
        printf("  %5s  %-30s  %-30s  %s\n",
               "UID", "From", "Subject", "Date");
        printf("  %s  %s  %s  %s\n", "═════",
               "══════════════════════════════",
               "══════════════════════════════",
               "═══════════════════════════");
    }

    /* Rows */
    for (int i = start; i < end; i++) {
        char *hdrs    = fetch_uid_content_in(cfg, folder, entries[i].uid, 1);
        char *from    = hdrs ? mime_get_header(hdrs, "From")    : NULL;
        char *subject = hdrs ? mime_get_header(hdrs, "Subject") : NULL;
        char *date    = hdrs ? mime_get_header(hdrs, "Date")    : NULL;

        if (opts->all) {
            printf("  %c  %5d  %-30.30s  %-30.30s  %s\n",
                   entries[i].unseen ? 'N' : ' ',
                   entries[i].uid,
                   from    ? from    : "(no from)",
                   subject ? subject : "(no subject)",
                   date    ? date    : "");
        } else {
            printf("  %5d  %-30.30s  %-30.30s  %s\n",
                   entries[i].uid,
                   from    ? from    : "(no from)",
                   subject ? subject : "(no subject)",
                   date    ? date    : "");
        }

        free(hdrs); free(from); free(subject); free(date);
    }

    if (end < show_count)
        printf("\n  -- %d more message(s) --  use --offset %d for next page\n",
               show_count - end, end + 1);

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

int email_service_read(const Config *cfg, int uid) {
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

    char *from    = mime_get_header(raw, "From");
    char *subject = mime_get_header(raw, "Subject");
    char *date    = mime_get_header(raw, "Date");

    printf("From:    %s\n", from    ? from    : "(none)");
    printf("Subject: %s\n", subject ? subject : "(none)");
    printf("Date:    %s\n", date    ? date    : "(none)");
    printf("\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500"
           "\u2500\n");

    char *body = mime_get_text_body(raw);
    printf("%s\n", body ? body : "(no readable text body)");

    free(body); free(from); free(subject); free(date); free(raw);
    return 0;
}
