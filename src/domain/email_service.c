#include "email_service.h"
#include "curl_adapter.h"
#include "cache_store.h"
#include "mime_util.h"
#include "raii.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal buffer for accumulating curl response data ─────────────── */

typedef struct {
    char  *data;
    size_t size;
} Buffer;

static size_t buffer_append(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Buffer *buf = (Buffer *)userdata;
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

/**
 * Parses "* SEARCH 1 2 3 ..." from a raw IMAP SEARCH response.
 * Returns the number of UIDs found; caller must free *uids_out.
 */
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

/* ── Helpers ─────────────────────────────────────────────────────────── */

static CURL *make_curl(const Config *cfg) {
    return curl_adapter_init(cfg->user, cfg->pass, !cfg->ssl_no_verify);
}

/** Issues a UID SEARCH with the given criteria. Caller must free *uids_out. */
static int search_uids(const Config *cfg, const char *criteria, int **uids_out) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return -1;

    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/%s", cfg->host, cfg->folder) == -1) return -1;

    RAII_STRING char *cmd = NULL;
    if (asprintf(&cmd, "UID SEARCH %s", criteria) == -1) return -1;

    Buffer buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cmd);
    CURLcode res = curl_adapter_fetch(curl, url, &buf, buffer_append);

    int count = 0;
    if (res != CURLE_OK) {
        logger_log(LOG_WARN, "UID SEARCH %s failed: %s", criteria, curl_easy_strerror(res));
    } else {
        count = parse_uid_list(buf.data, uids_out);
    }
    free(buf.data);
    return count;
}

/**
 * Fetches the raw header block for a single UID via BODY.PEEK[HEADER].
 * Returns a heap-allocated NUL-terminated IMAP response, or NULL. Caller must free.
 */
static char *fetch_raw_imap_response(const Config *cfg, const char *cmd_fmt, int uid) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return NULL;

    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/%s", cfg->host, cfg->folder) == -1) return NULL;

    RAII_STRING char *cmd = NULL;
    if (asprintf(&cmd, cmd_fmt, uid) == -1) return NULL;

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, cmd);

    Buffer buf = {NULL, 0};
    CURLcode res = curl_adapter_fetch(curl, url, &buf, buffer_append);
    if (res != CURLE_OK) {
        logger_log(LOG_WARN, "FETCH UID %d failed: %s", uid, curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data; /* caller owns */
}

/* ── Public API ──────────────────────────────────────────────────────── */

int email_service_list_unseen(const Config *cfg) {
    printf("--- Fetching recent emails: %s @ %s/%s ---\n",
           cfg->user, cfg->host, cfg->folder);

    int *uids = NULL;
    int total = search_uids(cfg, "UNSEEN", &uids);

    if (total < 0) {
        fprintf(stderr, "Failed to search mailbox.\n");
        return -1;
    }
    if (total == 0) {
        printf("No unread messages in %s.\n", cfg->folder);
        return 0;
    }

    printf("%d unread message(s) in %s.\n\n", total, cfg->folder);

    printf("  %5s  %-30s  %-30s  %s\n",
           "UID", "From", "Subject", "Date");
    printf("  %s  %s  %s  %s\n",
           "═════",
           "══════════════════════════════",
           "══════════════════════════════",
           "═══════════════════════════");

    for (int i = 0; i < total; i++) {
        char *imap_resp = fetch_raw_imap_response(
            cfg, "UID FETCH %d BODY.PEEK[HEADER]", uids[i]);

        char *hdrs    = imap_resp ? mime_extract_imap_literal(imap_resp) : NULL;
        char *from    = hdrs ? mime_get_header(hdrs, "From")    : NULL;
        char *subject = hdrs ? mime_get_header(hdrs, "Subject") : NULL;
        char *date    = hdrs ? mime_get_header(hdrs, "Date")    : NULL;

        printf("  %5d  %-30.30s  %-30.30s  %s\n",
               uids[i],
               from    ? from    : "(no from)",
               subject ? subject : "(no subject)",
               date    ? date    : "");

        free(imap_resp);
        free(hdrs);
        free(from);
        free(subject);
        free(date);
    }

    free(uids);
    return 0;
}

int email_service_read(const Config *cfg, int uid) {
    char *raw = NULL;

    if (cache_exists(cfg->folder, uid)) {
        logger_log(LOG_DEBUG, "Cache hit for UID %d in %s", uid, cfg->folder);
        raw = cache_load(cfg->folder, uid);
    } else {
        char *imap_resp = fetch_raw_imap_response(
            cfg, "UID FETCH %d RFC822", uid);
        if (!imap_resp) {
            fprintf(stderr, "Failed to fetch message UID %d.\n", uid);
            return -1;
        }

        raw = mime_extract_imap_literal(imap_resp);
        if (!raw) {
            /* Server delivered bare content (no IMAP literal framing) */
            raw = imap_resp;
            imap_resp = NULL;
        }
        free(imap_resp);

        if (raw)
            cache_save(cfg->folder, uid, raw, strlen(raw));
    }

    if (!raw) {
        fprintf(stderr, "Could not load message UID %d.\n", uid);
        return -1;
    }

    char *from    = mime_get_header(raw, "From");
    char *subject = mime_get_header(raw, "Subject");
    char *date    = mime_get_header(raw, "Date");

    printf("From:    %s\n", from    ? from    : "(none)");
    printf("Subject: %s\n", subject ? subject : "(none)");
    printf("Date:    %s\n", date    ? date    : "(none)");
    printf("─────────────────────────────────────────────────────────────\n");

    char *body = mime_get_text_body(raw);
    printf("%s\n", body ? body : "(no readable text body)");

    free(body);
    free(from);
    free(subject);
    free(date);
    free(raw);
    return 0;
}
