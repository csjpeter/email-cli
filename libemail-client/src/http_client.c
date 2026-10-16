/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* A small HTTP/1.1 client on OpenSSL: the REST calls of the library. */
#include "http_client.h"
#include "logger.h"
#include "net_tls.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/**
 * @file http_client.c
 * @brief See http_client.h.
 */

#define HTTP_DEFAULT_MAX (16u * 1024u * 1024u)
#define HTTP_MAX_HEADERS (64u * 1024u)

typedef struct { char *p; size_t len, cap; } Buf;

static int buf_add(Buf *b, const void *d, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *t = realloc(b->p, nc);
        if (!t) return -1;
        b->p = t;
        b->cap = nc;
    }
    if (n) memcpy(b->p + b->len, d, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static void set_err(char *err, size_t n, const char *msg)
{
    if (err && n) snprintf(err, n, "%s", msg);
}

/* ── URL ────────────────────────────────────────────────────────────── */

typedef struct {
    int  tls;
    char host[256];
    int  port;
    char path[4096];     /* path and query, begins with '/' */
} Url;

static int url_parse(const char *url, Url *u)
{
    const char *p;
    if (strncmp(url, "https://", 8) == 0) { u->tls = 1; u->port = 443; p = url + 8; }
    else if (strncmp(url, "http://", 7) == 0) { u->tls = 0; u->port = 80; p = url + 7; }
    else return -1;
    const char *end = p + strcspn(p, "/?#");
    const char *hostend = end;
    const char *colon = NULL;
    if (*p == '[') {                         /* [IPv6]:port */
        const char *rb = memchr(p, ']', (size_t)(end - p));
        if (!rb) return -1;
        size_t n = (size_t)(rb - p - 1);
        if (n == 0 || n >= sizeof(u->host)) return -1;
        memcpy(u->host, p + 1, n);
        u->host[n] = '\0';
        if (rb + 1 < end && rb[1] == ':') colon = rb + 1;
        else if (rb + 1 != end) return -1;
    } else {
        for (const char *q = p; q < end; q++) if (*q == ':') colon = q;
        if (colon) hostend = colon;
        size_t n = (size_t)(hostend - p);
        if (n == 0 || n >= sizeof(u->host)) return -1;
        memcpy(u->host, p, n);
        u->host[n] = '\0';
    }
    if (colon) {
        char *e = NULL;
        long port = strtol(colon + 1, &e, 10);
        if (e != end || port <= 0 || port > 65535) return -1;
        u->port = (int)port;
    }
    if (*end == '/' ) snprintf(u->path, sizeof(u->path), "%s", end);
    else if (*end == '?') snprintf(u->path, sizeof(u->path), "/%s", end);
    else snprintf(u->path, sizeof(u->path), "/");
    char *frag = strchr(u->path, '#');
    if (frag) *frag = '\0';
    return 0;
}

char *http_url_escape(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1);
    if (!out) return NULL;
    char *o = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_' || *p == '~') *o++ = (char)*p;
        else o += sprintf(o, "%%%02X", *p);
    }
    *o = '\0';
    return out;
}

/* ── Response reading ───────────────────────────────────────────────── */

/** Read a line (without CRLF) from the stream buffer; 1 line, 0 end, -1 error. */
typedef struct { NetConn *c; unsigned char buf[8192]; size_t pos, len; } Reader;

static int rd_fill(Reader *r)
{
    ssize_t n = net_conn_read(r->c, r->buf, sizeof(r->buf));
    if (n <= 0) return (int)n;
    r->pos = 0;
    r->len = (size_t)n;
    return 1;
}

static int rd_byte(Reader *r)
{
    if (r->pos >= r->len) {
        int f = rd_fill(r);
        if (f <= 0) return f == 0 ? -1 : -2;
    }
    return r->buf[r->pos++];
}

static int rd_line(Reader *r, char *line, size_t cap)
{
    size_t n = 0;
    for (;;) {
        int ch = rd_byte(r);
        if (ch == -2) return -1;
        if (ch == -1) { if (n == 0) return 0; break; }
        if (ch == '\n') break;
        if (n + 1 < cap) line[n++] = (char)ch;
    }
    if (n && line[n - 1] == '\r') n--;
    line[n] = '\0';
    return 1;
}

/** Read exactly @p want bytes into @p b (stops early at end of stream when @p allow_eof). */
static int rd_body_n(Reader *r, Buf *b, size_t want, size_t max)
{
    while (want > 0) {
        if (b->len + want > max) return -1;
        if (r->pos >= r->len) {
            int f = rd_fill(r);
            if (f <= 0) return -1;
        }
        size_t take = r->len - r->pos;
        if (take > want) take = want;
        if (buf_add(b, r->buf + r->pos, take) != 0) return -1;
        r->pos += take;
        want -= take;
    }
    return 0;
}

static int rd_until_close(Reader *r, Buf *b, size_t max)
{
    for (;;) {
        if (r->pos >= r->len) {
            int f = rd_fill(r);
            if (f < 0) return -1;
            if (f == 0) return 0;
        }
        size_t take = r->len - r->pos;
        if (b->len + take > max) return -1;
        if (buf_add(b, r->buf + r->pos, take) != 0) return -1;
        r->pos += take;
    }
}

static int rd_chunked(Reader *r, Buf *b, size_t max)
{
    char line[128];
    for (;;) {
        if (rd_line(r, line, sizeof(line)) <= 0) return -1;
        char *semi = strchr(line, ';');
        if (semi) *semi = '\0';
        char *e = NULL;
        unsigned long sz = strtoul(line, &e, 16);
        if (e == line) return -1;
        if (sz == 0) {
            while (rd_line(r, line, sizeof(line)) > 0 && line[0]) { /* trailers */ }
            return 0;
        }
        if (rd_body_n(r, b, sz, max) != 0) return -1;
        if (rd_line(r, line, sizeof(line)) <= 0) return -1;          /* CRLF after the chunk */
    }
}

static int header_is(const char *line, const char *name, const char **value)
{
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') return 0;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *value = v;
    return 1;
}

/* ── The request ────────────────────────────────────────────────────── */

int http_request(const HttpRequest *req, long *status, char **body_out,
                 size_t *len_out, char *err, size_t err_len)
{
    if (body_out) *body_out = NULL;
    if (len_out) *len_out = 0;
    if (status) *status = 0;
    if (err && err_len) err[0] = '\0';
    Url u;
    if (!req || !req->method || !req->url || url_parse(req->url, &u) != 0) {
        set_err(err, err_len, "Bad URL");
        return -1;
    }
    size_t max = req->max_response ? req->max_response : HTTP_DEFAULT_MAX;
    int timeout = req->timeout_s > 0 ? req->timeout_s : 30;

    NetConn *c = net_connect(u.host, u.port, u.tls, u.tls /* verify https */,
                             timeout, err, err_len);
    if (!c) return -1;

    Buf head = {0};
    int ok = 0;
    char hostline[300];
    int default_port = u.tls ? 443 : 80;
    if (u.port == default_port) snprintf(hostline, sizeof(hostline), "%s", u.host);
    else snprintf(hostline, sizeof(hostline), "%s:%d", u.host, u.port);
    char tmp[128];
    buf_add(&head, req->method, strlen(req->method));
    buf_add(&head, " ", 1);
    buf_add(&head, u.path, strlen(u.path));
    buf_add(&head, " HTTP/1.1\r\nHost: ", 17);
    buf_add(&head, hostline, strlen(hostline));
    static const char fixed[] = "\r\nUser-Agent: libemail-client\r\nAccept: */*\r\n"
                                "Accept-Encoding: identity\r\nConnection: close\r\n";
    buf_add(&head, fixed, sizeof(fixed) - 1);
    int has_len = 0;
    for (const char *const *h = req->headers; h && *h; h++) {
        buf_add(&head, *h, strlen(*h));
        buf_add(&head, "\r\n", 2);
        if (strncasecmp(*h, "Content-Length:", 15) == 0) has_len = 1;
    }
    if (!has_len && (req->body_len || strcmp(req->method, "POST") == 0 ||
                     strcmp(req->method, "PUT") == 0)) {
        snprintf(tmp, sizeof(tmp), "Content-Length: %zu\r\n", req->body_len);
        buf_add(&head, tmp, strlen(tmp));
    }
    buf_add(&head, "\r\n", 2);

    Buf body = {0};
    Reader *r = calloc(1, sizeof(*r));
    if (!r) { net_close(c); free(head.p); return -1; }
    r->c = c;
    /* head and body go out in one write: a server that answers from the
     * headers alone then closes on no unread bytes (which would reset the
     * connection under the response). */
    if (req->body_len && buf_add(&head, req->body, req->body_len) != 0) {
        set_err(err, err_len, "Out of memory");
        goto done;
    }
    if (net_conn_write_all(c, head.p, head.len) != 0) {
        set_err(err, err_len, "Send failed");
        goto done;
    }

    char line[8192];
    if (rd_line(r, line, sizeof(line)) <= 0) { set_err(err, err_len, "No response from server"); goto done; }
    long code = 0;
    if (strncmp(line, "HTTP/1.", 7) != 0 || sscanf(line + 8, "%ld", &code) != 1) {
        set_err(err, err_len, "Bad HTTP response");
        goto done;
    }
    long clen = -1;
    int chunked = 0;
    size_t hbytes = 0;
    for (;;) {
        int rl = rd_line(r, line, sizeof(line));
        if (rl <= 0) { set_err(err, err_len, "Truncated HTTP response"); goto done; }
        if (line[0] == '\0') break;
        hbytes += strlen(line);
        if (hbytes > HTTP_MAX_HEADERS) { set_err(err, err_len, "HTTP headers too large"); goto done; }
        const char *v;
        if (header_is(line, "Content-Length", &v)) clen = strtol(v, NULL, 10);
        else if (header_is(line, "Transfer-Encoding", &v) && strcasestr(v, "chunked")) chunked = 1;
    }
    int no_body = code == 204 || code == 304 || (code >= 100 && code < 200) ||
                  strcmp(req->method, "HEAD") == 0;
    int rc = 0;
    if (no_body) rc = 0;
    else if (chunked) rc = rd_chunked(r, &body, max);
    else if (clen >= 0) rc = (size_t)clen > max ? -1 : rd_body_n(r, &body, (size_t)clen, max);
    else rc = rd_until_close(r, &body, max);
    if (rc != 0) { set_err(err, err_len, "Truncated or oversized HTTP body"); goto done; }
    if (!body.p) buf_add(&body, "", 0);
    if (status) *status = code;
    ok = 1;
done:
    free(r);
    net_close(c);
    free(head.p);
    if (!ok) { free(body.p); return -1; }
    if (len_out) *len_out = body.len;
    if (body_out) *body_out = body.p; else free(body.p);
    return 0;
}
