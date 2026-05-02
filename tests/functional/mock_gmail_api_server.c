/**
 * @file mock_gmail_api_server.c
 * @brief Minimal plain-HTTP mock Gmail REST API server for functional testing.
 *
 * Simulates the Gmail API endpoints used by email-cli's gmail_client.c.
 * Single-threaded: accepts one connection at a time.
 *
 * Environment variables:
 *   MOCK_GMAIL_PORT  - TCP port to listen on (default 9997)
 *   MOCK_GMAIL_COUNT - Number of messages to simulate (default 200)
 *
 * Message layout (N = message number, 1-indexed):
 *   ID:      zero-padded 16-char hex, e.g. "0000000000000001"
 *   Subject: "Message N"
 *   From:    "Sender N <senderN@test.gmail.com>"
 *   Date:    "Mon, 01 Jan 2024 00:00:00 +0000" (fixed for simplicity)
 *   Body:    "Body of message N."
 *
 *   N % 10 == 0: multipart/mixed with notes_N.txt attachment
 *
 * Label assignment:
 *   N  1-40   → INBOX + UNREAD
 *   N 41-50   → INBOX + Work
 *   N 51-100  → INBOX + Personal
 *   N 101-190 → INBOX
 *   N 191-200 → INBOX + STARRED
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

/* ── Configuration ────────────────────────────────────────────────── */

static int  g_port    = 9997;
static int  g_count   = 200;
static char g_prefix[64]  = "";   /* MOCK_GMAIL_SUBJECT_PREFIX  */
static char g_email[128]  = "test@gmail.com"; /* MOCK_GMAIL_EMAIL */

/* Incremental-sync control (Phase 32 tests) */
static char g_histid[32]     = "5000"; /* MOCK_GMAIL_HISTID           */
static int  g_extra_count    = 0;      /* MOCK_GMAIL_EXTRA_COUNT       */
static int  g_history_expired = 0;     /* MOCK_GMAIL_HISTORY_EXPIRED=1 */
static int  g_profile_fail   = 0;      /* MOCK_GMAIL_PROFILE_FAIL=1    */

/* ── Base64url encoding ───────────────────────────────────────────── */

static const char b64url_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/**
 * Encode data as base64url (no padding).
 * Returns heap-allocated NUL-terminated string; caller must free.
 */
static char *b64url_encode(const unsigned char *data, size_t len) {
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

/* Standard base64 for attachment content (RFC 2045) */
static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *data, size_t len) {
    size_t alloc = ((len + 2) / 3) * 4 + 1;
    char *out = malloc(alloc);
    if (!out) return NULL;

    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = ((unsigned int)data[i]) << 16;
        if (i + 1 < len) n |= ((unsigned int)data[i + 1]) << 8;
        if (i + 2 < len) n |= ((unsigned int)data[i + 2]);

        out[o++] = b64_chars[(n >> 18) & 0x3F];
        out[o++] = b64_chars[(n >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? b64_chars[(n >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? b64_chars[n & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}

/* ── Message helpers ──────────────────────────────────────────────── */

/**
 * Convert 1-indexed message number N to zero-padded 16-char hex ID.
 * e.g. N=1 → "0000000000000001", N=200 → "00000000000000c8"
 */
static void msg_id(int n, char buf[17]) {
    snprintf(buf, 17, "%016x", (unsigned int)n);
}

/**
 * Convert a hex message ID back to integer N.
 * Returns 0 on failure.
 */
static int id_to_n(const char *id) {
    if (!id || !id[0]) return 0;
    unsigned int n = 0;
    if (sscanf(id, "%x", &n) != 1) return 0;
    return (int)n;
}

/**
 * Return labels JSON array for message N.
 * Returns heap-allocated string like ["INBOX","UNREAD"]
 */
static char *labels_for(int n) {
    if (n >= 1 && n <= 40)
        return strdup("[\"INBOX\",\"UNREAD\"]");
    if (n >= 41 && n <= 50)
        return strdup("[\"INBOX\",\"Work\"]");
    if (n >= 51 && n <= 100)
        return strdup("[\"INBOX\",\"Personal\"]");
    if (n >= 191)
        return strdup("[\"INBOX\",\"STARRED\"]");
    return strdup("[\"INBOX\"]");
}

/**
 * Build RFC 822 raw email for message N.
 * Returns heap-allocated string; caller must free.
 */
static char *build_raw_email(int n) {
    char *raw = NULL;
    int has_attachment = (n % 10 == 0);

    if (!has_attachment) {
        if (asprintf(&raw,
                "From: Sender %d <%ssender%d@test.gmail.com>\r\n"
                "To: %s\r\n"
                "Subject: %sMessage %d\r\n"
                "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n"
                "MIME-Version: 1.0\r\n"
                "Content-Type: text/plain; charset=UTF-8\r\n"
                "\r\n"
                "%sBody of message %d.\r\n",
                n, g_prefix, n, g_email, g_prefix, n, g_prefix, n) == -1)
            return NULL;
        return raw;
    }

    /* Multipart with attachment */
    char boundary[32];
    snprintf(boundary, sizeof(boundary), "GM%04d", n);

    char attach_name[128];
    snprintf(attach_name, sizeof(attach_name), "%snotes_%d.txt", g_prefix, n);

    char attach_content[128];
    snprintf(attach_content, sizeof(attach_content), "%sAttachment for msg %d", g_prefix, n);
    char *attach_b64 = b64_encode(
        (const unsigned char *)attach_content, strlen(attach_content));
    if (!attach_b64) return NULL;

    if (asprintf(&raw,
            "From: Sender %d <%ssender%d@test.gmail.com>\r\n"
            "To: %s\r\n"
            "Subject: %sMessage %d\r\n"
            "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: multipart/mixed; boundary=\"%s\"\r\n"
            "\r\n"
            "--%s\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "\r\n"
            "%sBody of message %d.\r\n"
            "--%s\r\n"
            "Content-Type: text/plain; name=\"%s\"\r\n"
            "Content-Disposition: attachment; filename=\"%s\"\r\n"
            "Content-Transfer-Encoding: base64\r\n"
            "\r\n"
            "%s\r\n"
            "--%s--\r\n",
            n, g_prefix, n, g_email, g_prefix, n,
            boundary,
            boundary,
            g_prefix, n,
            boundary,
            attach_name, attach_name,
            attach_b64,
            boundary) == -1) {
        free(attach_b64);
        return NULL;
    }
    free(attach_b64);
    return raw;
}

/* ── HTTP helpers ─────────────────────────────────────────────────── */

/**
 * Send an HTTP 200 JSON response over the plain socket.
 */
static void send_json(int fd, const char *body) {
    size_t body_len = strlen(body);
    char header[256];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             body_len);
    ssize_t r;
    r = write(fd, header, strlen(header)); (void)r;
    r = write(fd, body, body_len);        (void)r;
}

static void send_404(int fd) {
    const char *resp =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{}";
    ssize_t r = write(fd, resp, strlen(resp)); (void)r;
}

/* ── Request parsing ──────────────────────────────────────────────── */

/**
 * Read an HTTP request into buf.  Returns total bytes read or -1.
 * Reads until double CRLF (end of headers) since we only need the
 * request line + query string.
 */
static int read_request(int fd, char *buf, int bufsz) {
    int total = 0;
    while (total < bufsz - 1) {
        ssize_t n = read(fd, buf + total, (size_t)(bufsz - total - 1));
        if (n <= 0) break;
        total += (int)n;
        buf[total] = '\0';
        /* Stop reading once we see end of headers */
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[total] = '\0';
    return total;
}

/**
 * Extract value of a query parameter from a URL string.
 * Returns heap-allocated string or NULL if not found.
 */
static char *query_param(const char *url, const char *param) {
    /* Build "param=" to search for */
    size_t plen = strlen(param);
    char key[128];
    snprintf(key, sizeof(key), "%s=", param);

    const char *p = strstr(url, key);
    if (!p) return NULL;
    p += plen + 1; /* skip "param=" */

    /* Value ends at '&', ' ', or end */
    const char *end = p;
    while (*end && *end != '&' && *end != ' ' && *end != '\r' && *end != '\n')
        end++;
    if (end == p) return NULL;
    return strndup(p, (size_t)(end - p));
}

/* ── Response builders ────────────────────────────────────────────── */

/** Build the /profile response */
static void handle_profile(int fd) {
    if (g_profile_fail) {
        send_404(fd);
        return;
    }
    char body[256];
    snprintf(body, sizeof(body),
        "{\"historyId\":\"%s\","
        "\"emailAddress\":\"%s\"}", g_histid, g_email);
    send_json(fd, body);
}

/** Build the /labels response */
static void handle_labels(int fd) {
    const char *body =
        "{\"labels\":["
        "{\"id\":\"INBOX\",\"name\":\"INBOX\",\"type\":\"system\"},"
        "{\"id\":\"SENT\",\"name\":\"SENT\",\"type\":\"system\"},"
        "{\"id\":\"UNREAD\",\"name\":\"UNREAD\",\"type\":\"system\"},"
        "{\"id\":\"STARRED\",\"name\":\"STARRED\",\"type\":\"system\"},"
        "{\"id\":\"SPAM\",\"name\":\"SPAM\",\"type\":\"system\"},"
        "{\"id\":\"TRASH\",\"name\":\"TRASH\",\"type\":\"system\"},"
        "{\"id\":\"IMPORTANT\",\"name\":\"IMPORTANT\",\"type\":\"system\"},"
        "{\"id\":\"Work\",\"name\":\"Work\",\"type\":\"user\"},"
        "{\"id\":\"Personal\",\"name\":\"Personal\",\"type\":\"user\"}"
        "]}";
    send_json(fd, body);
}

/**
 * Build a paginated messages list response.
 * Returns heap-allocated JSON; caller must free.
 */
static char *build_messages_json(int first, int last, const char *next_token) {
    /* Estimate max size: each ID entry ~60 chars */
    int count = last - first + 1;
    if (count < 0) count = 0;
    size_t cap = (size_t)count * 64 + 256;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    int off = 0;
    off += snprintf(buf + off, cap - (size_t)off, "{\"messages\":[");
    for (int i = first; i <= last; i++) {
        char id[17];
        msg_id(i, id);
        if (i > first)
            off += snprintf(buf + off, cap - (size_t)off, ",");
        off += snprintf(buf + off, cap - (size_t)off,
                        "{\"id\":\"%s\",\"threadId\":\"%s\"}", id, id);
    }
    off += snprintf(buf + off, cap - (size_t)off, "]");
    if (next_token)
        off += snprintf(buf + off, cap - (size_t)off,
                        ",\"nextPageToken\":\"%s\"", next_token);
    off += snprintf(buf + off, cap - (size_t)off,
                    ",\"resultSizeEstimate\":%d"
                    ",\"historyId\":\"%s\"}", count, g_histid);
    (void)off;
    return buf;
}

/** Handle GET /messages?... */
static void handle_messages_list(int fd, const char *url) {
    char *label_id   = query_param(url, "labelIds");
    char *page_token = query_param(url, "pageToken");

    /* Determine which messages match the label filter */
    int first = 1, last = 0;

    if (!label_id || strcmp(label_id, "INBOX") == 0) {
        /* All messages, paginated (100 per page) */
        if (page_token && strcmp(page_token, "page2") == 0) {
            first = 101;
            last  = g_count;
            /* Build response with no next token */
            char *body = build_messages_json(first, last, NULL);
            if (body) { send_json(fd, body); free(body); }
            else send_404(fd);
        } else {
            /* First page */
            int page_last = (g_count > 100) ? 100 : g_count;
            const char *next = (g_count > 100) ? "page2" : NULL;
            char *body = build_messages_json(1, page_last, next);
            if (body) { send_json(fd, body); free(body); }
            else send_404(fd);
        }
    } else if (strcmp(label_id, "UNREAD") == 0) {
        /* First 40 messages */
        last = (g_count >= 40) ? 40 : g_count;
        char *body = build_messages_json(1, last, NULL);
        if (body) { send_json(fd, body); free(body); }
        else send_404(fd);
    } else if (strcmp(label_id, "STARRED") == 0) {
        /* Messages 191-200 */
        first = 191; last = (g_count >= 200) ? 200 : g_count;
        if (first > last) {
            send_json(fd, "{\"messages\":[],\"resultSizeEstimate\":0}");
        } else {
            char *body = build_messages_json(first, last, NULL);
            if (body) { send_json(fd, body); free(body); }
            else send_404(fd);
        }
    } else if (strcmp(label_id, "Work") == 0) {
        first = 41; last = (g_count >= 50) ? 50 : g_count;
        if (first > last) {
            send_json(fd, "{\"messages\":[],\"resultSizeEstimate\":0}");
        } else {
            char *body = build_messages_json(first, last, NULL);
            if (body) { send_json(fd, body); free(body); }
            else send_404(fd);
        }
    } else if (strcmp(label_id, "Personal") == 0) {
        first = 51; last = (g_count >= 100) ? 100 : g_count;
        if (first > last) {
            send_json(fd, "{\"messages\":[],\"resultSizeEstimate\":0}");
        } else {
            char *body = build_messages_json(first, last, NULL);
            if (body) { send_json(fd, body); free(body); }
            else send_404(fd);
        }
    } else {
        /* SPAM, TRASH, unknown: empty */
        send_json(fd, "{\"messages\":[],\"resultSizeEstimate\":0}");
    }

    free(label_id);
    free(page_token);
}

/** Handle GET /messages/{id}?format=raw */
static void handle_message_get(int fd, const char *msg_id_str) {
    int n = id_to_n(msg_id_str);
    if (n < 1 || n > g_count + g_extra_count) {
        send_404(fd);
        return;
    }

    char *raw_email = build_raw_email(n);
    if (!raw_email) { send_404(fd); return; }

    char *raw_b64url = b64url_encode(
        (const unsigned char *)raw_email, strlen(raw_email));
    free(raw_email);
    if (!raw_b64url) { send_404(fd); return; }

    char *labels_json = labels_for(n);
    if (!labels_json) { free(raw_b64url); send_404(fd); return; }

    char id_buf[17];
    msg_id(n, id_buf);

    char *body = NULL;
    if (asprintf(&body,
            "{\"id\":\"%s\","
            "\"threadId\":\"%s\","
            "\"labelIds\":%s,"
            "\"raw\":\"%s\"}",
            id_buf, id_buf, labels_json, raw_b64url) == -1) {
        free(raw_b64url);
        free(labels_json);
        send_404(fd);
        return;
    }
    free(raw_b64url);
    free(labels_json);

    send_json(fd, body);
    free(body);
}

/** Handle GET /history?startHistoryId=... */
static void handle_history(int fd) {
    if (g_history_expired) {
        send_404(fd);
        return;
    }

    if (g_extra_count > 0) {
        /* Return messagesAdded for messages g_count+1 .. g_count+g_extra_count */
        int new_hid = atoi(g_histid) + 1;
        size_t cap = (size_t)g_extra_count * 64 + 256;
        char *buf = malloc(cap);
        if (!buf) { send_404(fd); return; }

        int off = 0;
        off += snprintf(buf + off, cap - (size_t)off,
                        "{\"historyId\":\"%d\",\"messagesAdded\":[", new_hid);
        for (int i = 0; i < g_extra_count; i++) {
            int n = g_count + 1 + i;
            char id[17];
            msg_id(n, id);
            if (i > 0) off += snprintf(buf + off, cap - (size_t)off, ",");
            off += snprintf(buf + off, cap - (size_t)off,
                            "{\"id\":\"%s\",\"threadId\":\"%s\"}", id, id);
        }
        off += snprintf(buf + off, cap - (size_t)off, "]}");
        (void)off;
        send_json(fd, buf);
        free(buf);
        return;
    }

    /* No changes — return empty history with current historyId */
    char body[128];
    snprintf(body, sizeof(body),
             "{\"historyId\":\"%s\",\"history\":[]}", g_histid);
    send_json(fd, body);
}

/* ── Request dispatcher ───────────────────────────────────────────── */

/**
 * Parse a single HTTP request from fd, dispatch, and respond.
 */
static void handle_connection(int fd) {
    char buf[8192];
    if (read_request(fd, buf, (int)sizeof(buf)) <= 0) return;

    /* Extract request line: "GET /path?query HTTP/1.x" */
    char method[8] = {0};
    char path[4096] = {0};
    if (sscanf(buf, "%7s %4095s", method, path) != 2) return;

    printf("Mock Gmail: %s %s\n", method, path);

    /* Route by path prefix */
    /* /gmail/v1/users/me/profile */
    if (strstr(path, "/profile")) {
        handle_profile(fd);
        return;
    }

    /* /gmail/v1/users/me/labels */
    if (strstr(path, "/labels")) {
        handle_labels(fd);
        return;
    }

    /* /gmail/v1/users/me/history */
    if (strstr(path, "/history")) {
        handle_history(fd);
        return;
    }

    /* /gmail/v1/users/me/messages/{id}[/modify|/trash|/untrash]?... */
    /* Must check specific message before the list endpoint */
    const char *msgs_prefix = strstr(path, "/messages/");
    if (msgs_prefix) {
        /* Extract the message ID — it comes right after "/messages/" */
        const char *id_start = msgs_prefix + 10; /* skip "/messages/" */
        /* ID ends at '/', '?' or end of string */
        const char *id_end = id_start;
        while (*id_end && *id_end != '/' && *id_end != '?' && *id_end != ' ')
            id_end++;
        if (id_end > id_start) {
            /* Check for sub-path (modify / trash / untrash) */
            if (*id_end == '/') {
                /* POST .../modify, .../trash, .../untrash → accept, return {} */
                send_json(fd, "{}");
                return;
            }
            char id_buf[64] = {0};
            size_t id_len = (size_t)(id_end - id_start);
            if (id_len >= sizeof(id_buf)) id_len = sizeof(id_buf) - 1;
            strncpy(id_buf, id_start, id_len);
            id_buf[id_len] = '\0';
            handle_message_get(fd, id_buf);
            return;
        }
    }

    /* /gmail/v1/users/me/messages?... (list) */
    if (strstr(path, "/messages")) {
        handle_messages_list(fd, path);
        return;
    }

    send_404(fd);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    const char *port_env   = getenv("MOCK_GMAIL_PORT");
    if (port_env  && atoi(port_env)  > 0) g_port  = atoi(port_env);
    const char *count_env  = getenv("MOCK_GMAIL_COUNT");
    if (count_env && atoi(count_env) > 0) g_count = atoi(count_env);
    const char *prefix_env = getenv("MOCK_GMAIL_SUBJECT_PREFIX");
    if (prefix_env && prefix_env[0]) {
        strncpy(g_prefix, prefix_env, sizeof(g_prefix) - 1);
        g_prefix[sizeof(g_prefix) - 1] = '\0';
    }
    const char *email_env  = getenv("MOCK_GMAIL_EMAIL");
    if (email_env && email_env[0]) {
        strncpy(g_email, email_env, sizeof(g_email) - 1);
        g_email[sizeof(g_email) - 1] = '\0';
    }
    const char *histid_env = getenv("MOCK_GMAIL_HISTID");
    if (histid_env && histid_env[0]) {
        strncpy(g_histid, histid_env, sizeof(g_histid) - 1);
        g_histid[sizeof(g_histid) - 1] = '\0';
    }
    const char *extra_env  = getenv("MOCK_GMAIL_EXTRA_COUNT");
    if (extra_env && atoi(extra_env) > 0) g_extra_count = atoi(extra_env);
    const char *expired_env = getenv("MOCK_GMAIL_HISTORY_EXPIRED");
    if (expired_env && strcmp(expired_env, "1") == 0) g_history_expired = 1;
    const char *profail_env = getenv("MOCK_GMAIL_PROFILE_FAIL");
    if (profail_env && strcmp(profail_env, "1") == 0) g_profile_fail = 1;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
               &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)g_port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 8) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("Mock Gmail API server listening on port %d (count: %d, prefix: '%s', email: %s, histid: %s, extra: %d, expired: %d)\n",
           g_port, g_count, g_prefix, g_email, g_histid, g_extra_count, g_history_expired);
    fflush(stdout);

    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    int client_fd;
    while ((client_fd = accept(server_fd, (struct sockaddr *)&cli, &cli_len)) >= 0) {
        /* Set a short recv timeout */
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_connection(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
