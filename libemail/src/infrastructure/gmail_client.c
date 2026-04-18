#include "gmail_client.h"
#include "gmail_auth.h"
#include "json_util.h"
#include "logger.h"
#include "raii.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ────────────────────────────────────────────────────── */

#define GMAIL_API "https://gmail.googleapis.com/gmail/v1/users/me"

/* ── Client struct ────────────────────────────────────────────────── */

struct GmailClient {
    char   *access_token;
    Config *cfg;             /* borrowed, not owned */
    GmailProgressFn progress_fn;
    void   *progress_ctx;
};

/* ── libcurl write callback ───────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} CurlBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuf *buf = userdata;
    size_t bytes = size * nmemb;
    if (buf->len + bytes + 1 > buf->cap) {
        size_t newcap = (buf->cap ? buf->cap * 2 : 4096);
        while (newcap < buf->len + bytes + 1) newcap *= 2;
        char *tmp = realloc(buf->data, newcap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap = newcap;
    }
    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    buf->data[buf->len] = '\0';
    return bytes;
}

/* ── HTTP helpers ─────────────────────────────────────────────────── */

/**
 * Perform an authenticated GET request.
 * Returns heap-allocated response body; sets *http_code.
 */
static char *api_get(GmailClient *c, const char *url, long *http_code) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    CurlBuf buf = {0};
    char auth_hdr[2048];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", c->access_token);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_hdr);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        logger_log(LOG_ERROR, "gmail: GET %s failed: %s", url, curl_easy_strerror(res));
        free(buf.data);
        curl_easy_cleanup(curl);
        return NULL;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_easy_cleanup(curl);
    return buf.data;
}

/**
 * Perform an authenticated POST with JSON body.
 * json_body may be NULL for empty-body POSTs (e.g. trash).
 */
static char *api_post_json(GmailClient *c, const char *url,
                           const char *json_body, long *http_code) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    CurlBuf buf = {0};
    char auth_hdr[2048];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", c->access_token);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_hdr);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (json_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    } else {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        logger_log(LOG_ERROR, "gmail: POST %s failed: %s", url, curl_easy_strerror(res));
        free(buf.data);
        curl_easy_cleanup(curl);
        return NULL;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_easy_cleanup(curl);
    return buf.data;
}

/**
 * Wrapper that auto-retries once on HTTP 401 (token expired).
 */
static char *api_get_retry(GmailClient *c, const char *url, long *http_code) {
    char *resp = api_get(c, url, http_code);
    if (resp && *http_code == 401) {
        free(resp);
        char *new_token = gmail_auth_refresh(c->cfg);
        if (!new_token) return NULL;
        free(c->access_token);
        c->access_token = new_token;
        resp = api_get(c, url, http_code);
    }
    return resp;
}

static char *api_post_retry(GmailClient *c, const char *url,
                            const char *json_body, long *http_code) {
    char *resp = api_post_json(c, url, json_body, http_code);
    if (resp && *http_code == 401) {
        free(resp);
        char *new_token = gmail_auth_refresh(c->cfg);
        if (!new_token) return NULL;
        free(c->access_token);
        c->access_token = new_token;
        resp = api_post_json(c, url, json_body, http_code);
    }
    return resp;
}

/* ── Base64url decode ─────────────────────────────────────────────── */

static const signed char b64url_table[256] = {
    ['A']=0,  ['B']=1,  ['C']=2,  ['D']=3,  ['E']=4,  ['F']=5,
    ['G']=6,  ['H']=7,  ['I']=8,  ['J']=9,  ['K']=10, ['L']=11,
    ['M']=12, ['N']=13, ['O']=14, ['P']=15, ['Q']=16, ['R']=17,
    ['S']=18, ['T']=19, ['U']=20, ['V']=21, ['W']=22, ['X']=23,
    ['Y']=24, ['Z']=25,
    ['a']=0+26, ['b']=1+26, ['c']=2+26, ['d']=3+26, ['e']=4+26,
    ['f']=5+26, ['g']=6+26, ['h']=7+26, ['i']=8+26, ['j']=9+26,
    ['k']=10+26,['l']=11+26,['m']=12+26,['n']=13+26,['o']=14+26,
    ['p']=15+26,['q']=16+26,['r']=17+26,['s']=18+26,['t']=19+26,
    ['u']=20+26,['v']=21+26,['w']=22+26,['x']=23+26,['y']=24+26,
    ['z']=25+26,
    ['0']=52, ['1']=53, ['2']=54, ['3']=55, ['4']=56,
    ['5']=57, ['6']=58, ['7']=59, ['8']=60, ['9']=61,
    ['-']=62, ['_']=63,
};

/**
 * Decode a base64url string (no padding) into a binary buffer.
 * Returns heap-allocated NUL-terminated buffer; sets *out_len.
 */
char *gmail_base64url_decode(const char *input, size_t in_len, size_t *out_len) {
    size_t alloc = (in_len / 4 + 1) * 3 + 1;
    char *out = malloc(alloc);
    if (!out) return NULL;

    size_t o = 0;
    unsigned int acc = 0;
    int bits = 0;

    for (size_t i = 0; i < in_len; i++) {
        unsigned char ch = (unsigned char)input[i];
        if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ') continue;
        int val = b64url_table[ch];
        /* Non-base64url chars have value 0 in the table; 'A' is also 0.
         * For safety, skip obvious non-alphabet chars. */
        if (val == 0 && ch != 'A') continue;
        acc = (acc << 6) | (unsigned int)val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (char)((acc >> bits) & 0xFF);
        }
    }

    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

/* ── Base64url encode ─────────────────────────────────────────────── */

static const char b64url_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/**
 * Encode binary data as base64url (no padding).
 * Returns heap-allocated NUL-terminated string.
 */
char *gmail_base64url_encode(const unsigned char *data, size_t len) {
    size_t alloc = ((len + 2) / 3) * 4 + 1;
    char *out = malloc(alloc);
    if (!out) return NULL;

    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = ((unsigned int)data[i]) << 16;
        if (i + 1 < len) n |= ((unsigned int)data[i + 1]) << 8;
        if (i + 2 < len) n |= ((unsigned int)data[i + 2]);

        out[o++] = b64url_chars[(n >> 18) & 0x3F];
        out[o++] = b64url_chars[(n >> 12) & 0x3F];
        if (i + 1 < len) out[o++] = b64url_chars[(n >> 6) & 0x3F];
        if (i + 2 < len) out[o++] = b64url_chars[n & 0x3F];
    }
    out[o] = '\0';
    return out;
}

/* ── Connect / Disconnect ─────────────────────────────────────────── */

GmailClient *gmail_connect(Config *cfg) {
    if (!cfg || !cfg->gmail_mode) {
        logger_log(LOG_ERROR, "gmail_connect: not a Gmail account");
        return NULL;
    }

    char *token = gmail_auth_refresh(cfg);
    if (!token) {
        logger_log(LOG_ERROR, "gmail_connect: failed to obtain access token");
        return NULL;
    }

    GmailClient *c = calloc(1, sizeof(*c));
    if (!c) { free(token); return NULL; }
    c->access_token = token;
    c->cfg = cfg;
    logger_log(LOG_DEBUG, "gmail_connect: connected for %s", cfg->user ? cfg->user : "(unknown)");
    return c;
}

void gmail_disconnect(GmailClient *c) {
    if (!c) return;
    free(c->access_token);
    free(c);
}

void gmail_set_progress(GmailClient *c, GmailProgressFn fn, void *ctx) {
    if (!c) return;
    c->progress_fn = fn;
    c->progress_ctx = ctx;
}

/* ── List labels ──────────────────────────────────────────────────── */

struct label_ctx {
    char **names;
    char **ids;
    int    count;
    int    cap;
};

static void collect_label(const char *obj, int index, void *ctx) {
    (void)index;
    struct label_ctx *lc = ctx;
    char *name = json_get_string(obj, "name");
    char *id   = json_get_string(obj, "id");
    if (!name || !id) { free(name); free(id); return; }

    if (lc->count == lc->cap) {
        int newcap = lc->cap ? lc->cap * 2 : 32;
        char **nn = realloc(lc->names, (size_t)newcap * sizeof(char *));
        char **ni = realloc(lc->ids,   (size_t)newcap * sizeof(char *));
        if (!nn || !ni) { free(name); free(id); free(nn); return; }
        lc->names = nn;
        lc->ids   = ni;
        lc->cap   = newcap;
    }
    lc->names[lc->count] = name;
    lc->ids[lc->count]   = id;
    lc->count++;
}

int gmail_list_labels(GmailClient *c, char ***names_out,
                      char ***ids_out, int *count_out) {
    *names_out = NULL;
    *ids_out   = NULL;
    *count_out = 0;

    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/labels", GMAIL_API) == -1) return -1;

    long code = 0;
    RAII_STRING char *resp = api_get_retry(c, url, &code);
    if (!resp || code != 200) {
        logger_log(LOG_ERROR, "gmail_list_labels: HTTP %ld", code);
        return -1;
    }

    struct label_ctx lc = {0};
    json_foreach_object(resp, "labels", collect_label, &lc);

    *names_out = lc.names;
    *ids_out   = lc.ids;
    *count_out = lc.count;
    return 0;
}

/* ── List messages ────────────────────────────────────────────────── */

struct msg_id_ctx {
    char (*uids)[17];
    int   count;
    int   cap;
};

static void collect_msg_id(const char *obj, int index, void *ctx) {
    (void)index;
    struct msg_id_ctx *mc = ctx;
    char *id = json_get_string(obj, "id");
    if (!id) return;

    if (mc->count == mc->cap) {
        int newcap = mc->cap ? mc->cap * 2 : 256;
        char (*tmp)[17] = realloc(mc->uids, (size_t)newcap * sizeof(char[17]));
        if (!tmp) { free(id); return; }
        mc->uids = tmp;
        mc->cap  = newcap;
    }
    snprintf(mc->uids[mc->count], 17, "%s", id);
    mc->count++;
    free(id);
}

int gmail_list_messages(GmailClient *c, const char *label_id,
                        const char *query,
                        char (**uids_out)[17], int *count_out) {
    *uids_out  = NULL;
    *count_out = 0;

    struct msg_id_ctx mc = {0};
    char *page_token = NULL;

    for (;;) {
        /* Build URL with optional query parameters */
        char url_buf[2048];
        int n = snprintf(url_buf, sizeof(url_buf), "%s/messages?maxResults=500", GMAIL_API);
        if (label_id)
            n += snprintf(url_buf + n, sizeof(url_buf) - (size_t)n, "&labelIds=%s", label_id);
        if (query)
            n += snprintf(url_buf + n, sizeof(url_buf) - (size_t)n, "&q=%s", query);
        if (page_token)
            n += snprintf(url_buf + n, sizeof(url_buf) - (size_t)n, "&pageToken=%s", page_token);
        free(page_token);
        page_token = NULL;
        (void)n;

        long code = 0;
        char *resp = api_get_retry(c, url_buf, &code);
        if (!resp || code != 200) {
            free(resp);
            break;
        }

        json_foreach_object(resp, "messages", collect_msg_id, &mc);

        page_token = json_get_string(resp, "nextPageToken");
        free(resp);

        if (c->progress_fn)
            c->progress_fn((size_t)mc.count, 0, c->progress_ctx);

        if (!page_token) break;
    }
    free(page_token);

    *uids_out  = mc.uids;
    *count_out = mc.count;
    return mc.uids ? 0 : (mc.count == 0 ? 0 : -1);
}

/* ── Fetch message (raw + labels) ─────────────────────────────────── */

char *gmail_fetch_message(GmailClient *c, const char *uid,
                          char ***labels_out, int *label_count_out) {
    if (labels_out) *labels_out = NULL;
    if (label_count_out) *label_count_out = 0;

    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/messages/%s?format=raw", GMAIL_API, uid) == -1)
        return NULL;

    long code = 0;
    RAII_STRING char *resp = api_get_retry(c, url, &code);
    if (!resp || code != 200) {
        if (code == 404) {
            logger_log(LOG_WARN, "gmail: message %s not found (deleted?)", uid);
        } else {
            logger_log(LOG_ERROR, "gmail_fetch_message %s: HTTP %ld", uid, code);
        }
        return NULL;
    }

    /* Extract and decode raw message */
    RAII_STRING char *raw_b64 = json_get_string(resp, "raw");
    if (!raw_b64) {
        logger_log(LOG_ERROR, "gmail_fetch_message %s: no 'raw' field", uid);
        return NULL;
    }

    size_t decoded_len = 0;
    char *decoded = gmail_base64url_decode(raw_b64, strlen(raw_b64), &decoded_len);
    if (!decoded) return NULL;

    /* Extract labels if requested */
    if (labels_out && label_count_out) {
        json_get_string_array(resp, "labelIds", labels_out, label_count_out);
    }

    return decoded;
}

/* ── Modify labels ────────────────────────────────────────────────── */

int gmail_modify_labels(GmailClient *c, const char *uid,
                        const char **add_labels, int add_count,
                        const char **remove_labels, int remove_count) {
    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/messages/%s/modify", GMAIL_API, uid) == -1)
        return -1;

    /* Build JSON body */
    /* Worst case: each label ~64 chars + quotes + commas + structure */
    size_t body_cap = 256 + (size_t)(add_count + remove_count) * 80;
    char *body = malloc(body_cap);
    if (!body) return -1;

    size_t off = 0;
    off += (size_t)snprintf(body + off, body_cap - off, "{");

    if (add_count > 0) {
        off += (size_t)snprintf(body + off, body_cap - off, "\"addLabelIds\":[");
        for (int i = 0; i < add_count; i++) {
            if (i > 0) off += (size_t)snprintf(body + off, body_cap - off, ",");
            off += (size_t)snprintf(body + off, body_cap - off, "\"%s\"", add_labels[i]);
        }
        off += (size_t)snprintf(body + off, body_cap - off, "]");
    }

    if (remove_count > 0) {
        if (add_count > 0) off += (size_t)snprintf(body + off, body_cap - off, ",");
        off += (size_t)snprintf(body + off, body_cap - off, "\"removeLabelIds\":[");
        for (int i = 0; i < remove_count; i++) {
            if (i > 0) off += (size_t)snprintf(body + off, body_cap - off, ",");
            off += (size_t)snprintf(body + off, body_cap - off, "\"%s\"", remove_labels[i]);
        }
        off += (size_t)snprintf(body + off, body_cap - off, "]");
    }

    snprintf(body + off, body_cap - off, "}");

    long code = 0;
    char *resp = api_post_retry(c, url, body, &code);
    free(body);
    free(resp);

    if (code != 200) {
        logger_log(LOG_ERROR, "gmail_modify_labels %s: HTTP %ld", uid, code);
        return -1;
    }
    return 0;
}

/* ── Trash / Untrash ──────────────────────────────────────────────── */

int gmail_trash(GmailClient *c, const char *uid) {
    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/messages/%s/trash", GMAIL_API, uid) == -1)
        return -1;

    long code = 0;
    RAII_STRING char *resp = api_post_retry(c, url, NULL, &code);
    if (code != 200) {
        logger_log(LOG_ERROR, "gmail_trash %s: HTTP %ld", uid, code);
        return -1;
    }
    return 0;
}

int gmail_untrash(GmailClient *c, const char *uid) {
    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/messages/%s/untrash", GMAIL_API, uid) == -1)
        return -1;

    long code = 0;
    RAII_STRING char *resp = api_post_retry(c, url, NULL, &code);
    if (code != 200) {
        logger_log(LOG_ERROR, "gmail_untrash %s: HTTP %ld", uid, code);
        return -1;
    }
    return 0;
}

/* ── Send ─────────────────────────────────────────────────────────── */

int gmail_send(GmailClient *c, const char *raw_msg, size_t len) {
    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/messages/send", GMAIL_API) == -1)
        return -1;

    /* Base64url encode the raw RFC 2822 message */
    char *encoded = gmail_base64url_encode((const unsigned char *)raw_msg, len);
    if (!encoded) return -1;

    /* Build JSON body: {"raw": "<base64url>"} */
    size_t body_len = strlen(encoded) + 32;
    char *body = malloc(body_len);
    if (!body) { free(encoded); return -1; }
    snprintf(body, body_len, "{\"raw\":\"%s\"}", encoded);
    free(encoded);

    long code = 0;
    char *resp = api_post_retry(c, url, body, &code);
    free(body);
    free(resp);

    if (code != 200) {
        logger_log(LOG_ERROR, "gmail_send: HTTP %ld", code);
        return -1;
    }
    logger_log(LOG_INFO, "gmail_send: message sent successfully");
    return 0;
}

/* ── Profile (historyId) ──────────────────────────────────────────── */

char *gmail_get_history_id(GmailClient *c) {
    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/profile", GMAIL_API) == -1) return NULL;

    long code = 0;
    RAII_STRING char *resp = api_get_retry(c, url, &code);
    if (!resp || code != 200) return NULL;

    return json_get_string(resp, "historyId");
}

/* ── History (incremental sync) ───────────────────────────────────── */

char *gmail_get_history(GmailClient *c, const char *history_id) {
    RAII_STRING char *url = NULL;
    if (asprintf(&url, "%s/history?startHistoryId=%s"
                 "&historyTypes=messageAdded,messageDeleted,labelAdded,labelRemoved",
                 GMAIL_API, history_id) == -1)
        return NULL;

    long code = 0;
    char *resp = api_get_retry(c, url, &code);
    if (!resp) return NULL;

    if (code == 404) {
        logger_log(LOG_WARN, "gmail: history %s expired — full resync needed", history_id);
        free(resp);
        return NULL;
    }
    if (code != 200) {
        logger_log(LOG_WARN, "gmail_get_history: HTTP %ld (will retry with full sync)", code);
        free(resp);
        return NULL;
    }

    return resp;
}
