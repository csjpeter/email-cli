#include "email_service.h"
#include "curl_adapter.h"
#include "raii.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Maximum number of most-recent messages to fetch. */
#define EMAIL_FETCH_RECENT 10

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

static size_t write_to_stdout(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)userdata;
    return fwrite(ptr, size, nmemb, stdout);
}

/* ── UID list parser ─────────────────────────────────────────────────── */

/**
 * Parses "* SEARCH 1 2 3 ..." from a raw IMAP response.
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

/**
 * Sends "UID SEARCH ALL" to the mailbox.
 * Returns the number of UIDs found, or -1 on error.
 * Caller must free *uids_out.
 */
static int search_all_uids(const Config *cfg, int **uids_out) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return -1;

    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/%s", cfg->host, cfg->folder) == -1) return -1;

    Buffer buf = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "UID SEARCH ALL");
    CURLcode res = curl_adapter_fetch(curl, url, &buf, buffer_append);

    if (res != CURLE_OK) {
        logger_log(LOG_WARN, "UID SEARCH failed: %s", curl_easy_strerror(res));
        free(buf.data);
        return -1;
    }

    int count = parse_uid_list(buf.data, uids_out);
    free(buf.data);
    return count;
}

/**
 * Fetches and prints one message identified by UID.
 */
static int fetch_and_print(const Config *cfg, int uid, int index, int of_total) {
    RAII_CURL CURL *curl = make_curl(cfg);
    if (!curl) return -1;

    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/%s/;UID=%d", cfg->host, cfg->folder, uid) == -1) return -1;

    printf("\n══════════════════════════════════════════\n");
    printf(" Message %d/%d  (UID %d)\n", index, of_total, uid);
    printf("══════════════════════════════════════════\n");

    CURLcode res = curl_adapter_fetch(curl, url, NULL, write_to_stdout);
    if (res != CURLE_OK) {
        logger_log(LOG_WARN, "Fetch UID %d failed: %s", uid, curl_easy_strerror(res));
        fprintf(stderr, "Error fetching UID %d: %s\n", uid, curl_easy_strerror(res));
        return -1;
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int email_service_fetch_recent(const Config *cfg) {
    printf("--- Fetching recent emails from %s/%s ---\n", cfg->host, cfg->folder);

    int *uids = NULL;
    int total = search_all_uids(cfg, &uids);

    if (total < 0) {
        fprintf(stderr, "Failed to search mailbox.\n");
        return -1;
    }
    if (total == 0) {
        printf("No messages found in %s.\n", cfg->folder);
        return 0;
    }

    int start = (total > EMAIL_FETCH_RECENT) ? total - EMAIL_FETCH_RECENT : 0;
    int show  = total - start;

    printf("Showing %d most recent of %d message(s).\n", show, total);

    int result = 0;
    for (int i = total - 1; i >= start; i--) {
        if (fetch_and_print(cfg, uids[i], total - i, show) != 0)
            result = -1;
    }

    free(uids);
    printf("\nFetch complete. Success.\n");
    return result;
}
