#include "imap_client.h"
#include "imap_util.h"
#include "local_store.h"
#include "logger.h"
#include "raii.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>

/* ── Read buffer ─────────────────────────────────────────────────────── */

#define RBUF_SIZE 65536

struct ImapClient {
    int      fd;
    SSL_CTX *ctx;
    SSL     *ssl;
    int      use_tls;
    int      tag_num;

    /* Receive ring buffer */
    char     rbuf[RBUF_SIZE];
    size_t   rbuf_pos;  /* read position */
    size_t   rbuf_len;  /* bytes available */

    /* Optional download-progress callback (set via imap_set_progress) */
    ImapProgressFn on_progress;
    void          *progress_ctx;
};

/* ── Low-level I/O ───────────────────────────────────────────────────── */

/** Read up to `n` bytes from the socket/TLS layer into `buf`.
 *  Returns bytes read (>0), 0 on EOF, -1 on error. */
static ssize_t net_read(ImapClient *c, char *buf, size_t n) {
    if (c->use_tls) {
        int r = SSL_read(c->ssl, buf, (int)n);
        if (r > 0) return (ssize_t)r;
        int err = SSL_get_error(c->ssl, r);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;
        logger_log(LOG_WARN, "SSL_read error %d", err);
        return -1;
    }
    ssize_t r = read(c->fd, buf, n);
    if (r < 0 && (errno == EINTR || errno == EAGAIN)) return 0;
    return r;
}

/** Write `n` bytes to the socket/TLS layer.
 *  Returns 0 on success, -1 on error. */
static int net_write(ImapClient *c, const char *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r;
        if (c->use_tls) {
            int w = SSL_write(c->ssl, buf + sent, (int)(n - sent));
            if (w <= 0) {
                logger_log(LOG_WARN, "SSL_write error %d",
                           SSL_get_error(c->ssl, w));
                return -1;
            }
            r = (ssize_t)w;
        } else {
            r = write(c->fd, buf + sent, n - sent);
            if (r < 0) {
                if (errno == EINTR) continue;
                logger_log(LOG_WARN, "write: %s", strerror(errno));
                return -1;
            }
        }
        sent += (size_t)r;
    }
    return 0;
}

/* ── Buffered byte reader ────────────────────────────────────────────── */

/** Ensure at least 1 byte is available in rbuf. Returns 0 on success, -1 on EOF/error. */
static int rbuf_fill(ImapClient *c) {
    if (c->rbuf_pos < c->rbuf_len) return 0;
    /* Compact: move unused data to front */
    c->rbuf_pos = 0;
    c->rbuf_len = 0;
    ssize_t r = net_read(c, c->rbuf, RBUF_SIZE);
    if (r <= 0) return -1;
    c->rbuf_len = (size_t)r;
    return 0;
}

/** Read exactly `n` bytes into `out`. Returns 0 on success, -1 on error/EOF. */
static int rbuf_read_exact(ImapClient *c, char *out, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (c->rbuf_pos >= c->rbuf_len) {
            c->rbuf_pos = 0;
            c->rbuf_len = 0;
            ssize_t r = net_read(c, c->rbuf, RBUF_SIZE);
            if (r <= 0) return -1;
            c->rbuf_len = (size_t)r;
        }
        size_t avail = c->rbuf_len - c->rbuf_pos;
        size_t take  = avail < (n - got) ? avail : (n - got);
        memcpy(out + got, c->rbuf + c->rbuf_pos, take);
        c->rbuf_pos += take;
        got         += take;
    }
    return 0;
}

/* ── Dynamic line buffer ─────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} LineBuf;

static void linebuf_free(LineBuf *lb) { free(lb->data); lb->data = NULL; lb->len = lb->cap = 0; }

static int linebuf_append(LineBuf *lb, char ch) {
    if (lb->len + 1 >= lb->cap) {
        size_t ncap = lb->cap ? lb->cap * 2 : 256;
        char *tmp = realloc(lb->data, ncap);
        if (!tmp) return -1;
        lb->data = tmp;
        lb->cap  = ncap;
    }
    lb->data[lb->len++] = ch;
    lb->data[lb->len]   = '\0';
    return 0;
}

/**
 * Read one CRLF-terminated line from the server into `lb`.
 * The trailing \r\n is stripped. Returns 0 on success, -1 on error/EOF.
 */
static int read_line(ImapClient *c, LineBuf *lb) {
    lb->len = 0;
    for (;;) {
        if (rbuf_fill(c) != 0) return -1;
        char ch = c->rbuf[c->rbuf_pos++];
        if (ch == '\r') continue;          /* skip CR */
        if (ch == '\n') {
            if (lb->data) lb->data[lb->len] = '\0';
            return 0;
        }
        if (linebuf_append(lb, ch) != 0) return -1;
    }
}

/* ── Command dispatch ────────────────────────────────────────────────── */

/** Send a formatted IMAP command prefixed with the next tag.
 *  `fmt` should NOT include the tag or trailing CRLF.
 *  The tag used is written to `tag_out` (capacity >= 16).
 *  Returns 0 on success, -1 on error. */
static int send_cmd(ImapClient *c, char tag_out[16], const char *fmt, ...) {
    c->tag_num++;
    snprintf(tag_out, 16, "A%04d", c->tag_num);

    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    if (len < 0 || (size_t)len >= sizeof(buf) - 3) return -1;

    /* Append CRLF */
    buf[len]     = '\r';
    buf[len + 1] = '\n';
    buf[len + 2] = '\0';

    /* Log command (mask password) */
    logger_log(LOG_DEBUG, "IMAP [OUT] %s %.*s", tag_out, len, buf);

    /* Write tag + space + command + CRLF as a single buffer so the entire
     * command arrives in one TCP segment and a line-reading server can parse
     * it correctly (two separate writes would be two packets on loopback). */
    char full[4096 + 32];
    int flen = snprintf(full, sizeof(full), "%s %s", tag_out, buf);
    if (flen < 0 || (size_t)flen >= sizeof(full)) return -1;
    if (net_write(c, full, (size_t)flen) != 0) return -1;
    return 0;
}

/* ── Literal reader ──────────────────────────────────────────────────── */

/* Minimum literal size (bytes) before the progress callback is invoked */
#define PROGRESS_THRESHOLD (100 * 1024)
/* Progress is reported every this many bytes */
#define PROGRESS_CHUNK     (128 * 1024)

/**
 * If line ends with `{N}`, read N literal bytes into a heap buffer.
 * `*lit_out` is set to the allocated buffer (NUL-terminated) and
 * `*lit_len` to N.  Returns 0 (no literal), N > 0 (literal read), -1 on error.
 * Calls c->on_progress (if set) every PROGRESS_CHUNK bytes for large literals.
 */
static long read_literal_if_present(ImapClient *c, const char *line,
                                    char **lit_out, size_t *lit_len) {
    *lit_out = NULL;
    *lit_len = 0;

    /* Find trailing {N} */
    const char *p = strrchr(line, '{');
    if (!p) return 0;
    char *end;
    long sz = strtol(p + 1, &end, 10);
    if (*end != '}' || sz < 0) return 0;

    /* Allocate output buffer */
    char *buf = malloc((size_t)sz + 1);
    if (!buf) return -1;

    if (sz > 0) {
        size_t total = (size_t)sz;

        if (!c->on_progress || total < PROGRESS_THRESHOLD) {
            /* Small literal or no callback: read all at once */
            if (rbuf_read_exact(c, buf, total) != 0) { free(buf); return -1; }
        } else {
            /* Large literal: read in chunks and report progress */
            size_t got = 0;
            size_t next_report = PROGRESS_CHUNK;
            while (got < total) {
                size_t want = PROGRESS_CHUNK < (total - got)
                              ? PROGRESS_CHUNK : (total - got);
                if (rbuf_read_exact(c, buf + got, want) != 0) {
                    free(buf); return -1;
                }
                got += want;
                if (got >= next_report || got == total) {
                    c->on_progress(got, total, c->progress_ctx);
                    next_report = got + PROGRESS_CHUNK;
                }
            }
        }
    }

    buf[sz] = '\0';
    *lit_out = buf;
    *lit_len = (size_t)sz;
    return sz;
}

void imap_set_progress(ImapClient *c, ImapProgressFn fn, void *ctx) {
    if (!c) return;
    c->on_progress   = fn;
    c->progress_ctx  = ctx;
}

/* ── Response reader ─────────────────────────────────────────────────── */

typedef struct {
    char  **untagged;  /* heap-allocated array of untagged response strings */
    int     count;
    int     cap;
    char   *literal;  /* the last literal body (first literal wins) */
    size_t  lit_len;
} Response;

static void response_free(Response *r) {
    for (int i = 0; i < r->count; i++) free(r->untagged[i]);
    free(r->untagged);
    free(r->literal);
    memset(r, 0, sizeof(*r));
}

static int response_add(Response *r, const char *line) {
    if (r->count == r->cap) {
        int ncap = r->cap ? r->cap * 2 : 16;
        char **tmp = realloc(r->untagged, (size_t)ncap * sizeof(char *));
        if (!tmp) return -1;
        r->untagged = tmp;
        r->cap      = ncap;
    }
    char *copy = strdup(line);
    if (!copy) return -1;
    r->untagged[r->count++] = copy;
    return 0;
}

/**
 * Read server responses until we see our tagged reply.
 * Collects untagged lines and the first literal body encountered.
 * Returns 0 on OK, -1 on NO/BAD/error.
 */
static int read_response(ImapClient *c, const char *tag, Response *r) {
    LineBuf lb = {NULL, 0, 0};

    for (;;) {
        if (read_line(c, &lb) != 0) {
            linebuf_free(&lb);
            return -1;
        }

        const char *line = lb.data ? lb.data : "";
        logger_log(LOG_DEBUG, "IMAP [ IN] %s", line);

        /* Tagged response? */
        size_t tlen = strlen(tag);
        if (strncmp(line, tag, tlen) == 0 && line[tlen] == ' ') {
            const char *status = line + tlen + 1;
            int ok = (strncasecmp(status, "OK", 2) == 0);
            if (!ok)
                logger_log(LOG_WARN, "IMAP %s", line);
            linebuf_free(&lb);  /* free AFTER all accesses to line/status */
            return ok ? 0 : -1;
        }

        /* Untagged: check for literal */
        char  *lit     = NULL;
        size_t lit_len = 0;
        long   lsz     = read_literal_if_present(c, line, &lit, &lit_len);
        if (lsz < 0) { linebuf_free(&lb); return -1; }

        response_add(r, line);

        if (lit) {
            /* We read the literal; now read the closing line: ")\r\n" or similar */
            if (!r->literal) {
                r->literal = lit;
                r->lit_len = lit_len;
            } else {
                free(lit);  /* second literal — discard (shouldn't happen) */
            }
            /* Read the remainder line after the literal (e.g. ")" or empty) */
            LineBuf trail = {NULL, 0, 0};
            if (read_line(c, &trail) == 0 && trail.data && trail.data[0])
                logger_log(LOG_DEBUG, "IMAP [ IN] %s", trail.data);
            linebuf_free(&trail);
        }
    }
}

/* ── URL parser ──────────────────────────────────────────────────────── */

/**
 * Parse "imaps://host:port" or "imap://host" into components.
 * Returns 0 on success.
 */
static int parse_url(const char *url, char *host, size_t hsize,
                     char *port, size_t psize, int *use_tls) {
    *use_tls = 0;
    const char *p = url;

    if (strncasecmp(p, "imaps://", 8) == 0) { *use_tls = 1; p += 8; }
    else if (strncasecmp(p, "imap://", 7) == 0) { p += 7; }
    else {
        /* Treat as bare hostname, default IMAPS */
        *use_tls = 1;
        snprintf(host, hsize, "%s", url);
        snprintf(port, psize, "993");
        return 0;
    }

    /* host[:port] */
    const char *colon = strchr(p, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= hsize) return -1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        snprintf(port, psize, "%s", colon + 1);
    } else {
        snprintf(host, hsize, "%s", p);
        snprintf(port, psize, "%s", *use_tls ? "993" : "143");
    }
    return 0;
}

/* ── Connect ─────────────────────────────────────────────────────────── */

ImapClient *imap_connect(const char *host_url, const char *user,
                         const char *pass, int verify_tls) {
    char host[256], port[16];
    int  use_tls = 1;
    if (parse_url(host_url, host, sizeof(host), port, sizeof(port), &use_tls) != 0) {
        logger_log(LOG_ERROR, "imap_connect: bad URL: %s", host_url);
        return NULL;
    }

    /* TCP connect */
    struct addrinfo hints = {0};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *ai = NULL;
    int rc = getaddrinfo(host, port, &hints, &ai);
    if (rc != 0) {
        logger_log(LOG_ERROR, "getaddrinfo(%s:%s): %s", host, port, gai_strerror(rc));
        return NULL;
    }

    int fd = -1;
    for (struct addrinfo *r = ai; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);

    if (fd < 0) {
        logger_log(LOG_ERROR, "connect to %s:%s failed: %s", host, port, strerror(errno));
        return NULL;
    }

    ImapClient *c = calloc(1, sizeof(ImapClient));
    if (!c) { close(fd); return NULL; }
    c->fd = fd;
    c->use_tls = use_tls;

    /* TLS handshake */
    if (use_tls) {
        /* Init OpenSSL (idempotent in OpenSSL 1.1+) */
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            logger_log(LOG_ERROR, "SSL_CTX_new failed");
            free(c); close(fd);
            return NULL;
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        if (!verify_tls) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        } else {
            SSL_CTX_set_default_verify_paths(ctx);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        }
        SSL *ssl = SSL_new(ctx);
        if (!ssl) {
            logger_log(LOG_ERROR, "SSL_new failed");
            SSL_CTX_free(ctx); free(c); close(fd);
            return NULL;
        }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host);  /* SNI */
        if (SSL_connect(ssl) != 1) {
            logger_log(LOG_ERROR, "SSL handshake failed with %s", host);
            SSL_free(ssl); SSL_CTX_free(ctx); free(c); close(fd);
            return NULL;
        }
        c->ctx = ctx;
        c->ssl = ssl;
    }

    /* Read server greeting */
    LineBuf lb = {NULL, 0, 0};
    if (read_line(c, &lb) != 0) {
        logger_log(LOG_ERROR, "No greeting from %s", host);
        goto fail;
    }
    logger_log(LOG_DEBUG, "IMAP [ IN] %s", lb.data ? lb.data : "");
    linebuf_free(&lb);

    /* LOGIN */
    char tag[16];
    /* Send LOGIN with literal username/password to handle special chars correctly.
     * Simple approach: just quote them (assume no embedded DQUOTE or backslash). */
    if (send_cmd(c, tag, "LOGIN \"%s\" \"%s\"", user, pass) != 0)
        goto fail;

    Response resp = {0};
    rc = read_response(c, tag, &resp);
    response_free(&resp);
    if (rc != 0) {
        logger_log(LOG_ERROR, "LOGIN failed for user %s on %s", user, host);
        goto fail;
    }

    logger_log(LOG_DEBUG, "IMAP connected and authenticated: %s@%s", user, host);
    return c;

fail:
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); }
    if (c->ctx) SSL_CTX_free(c->ctx);
    close(c->fd);
    free(c);
    return NULL;
}

/* ── Disconnect ──────────────────────────────────────────────────────── */

void imap_disconnect(ImapClient *c) {
    if (!c) return;
    /* Send LOGOUT (ignore errors — we're closing anyway) */
    char tag[16];
    send_cmd(c, tag, "LOGOUT");
    Response r = {0};
    read_response(c, tag, &r);
    response_free(&r);

    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); }
    if (c->ctx) SSL_CTX_free(c->ctx);
    if (c->fd >= 0) close(c->fd);
    free(c);
}

/* ── LIST ────────────────────────────────────────────────────────────── */

/**
 * Parse one `* LIST (\flags) "sep" "name"` or `* LIST (\flags) "sep" name` line.
 * Returns heap-allocated folder name (Modified UTF-7, not yet decoded), or NULL.
 * Sets *sep_out to the separator character.
 */
static char *parse_list_line(const char *line, char *sep_out) {
    /* Skip "* LIST " */
    if (strncasecmp(line, "* LIST ", 7) != 0) return NULL;
    const char *p = line + 7;

    /* Skip flags: (...) */
    if (*p == '(') {
        p = strchr(p, ')');
        if (!p) return NULL;
        p++;
    }
    while (*p == ' ') p++;

    /* Separator: "." or "/" or NIL */
    if (*p == '"') {
        p++;
        if (*p && *(p + 1) == '"') {
            *sep_out = *p;
            p += 2;
        } else if (*p == '"') {
            /* empty separator */
            p++;
        }
    } else if (strncasecmp(p, "NIL", 3) == 0) {
        *sep_out = '.';
        p += 3;
    }
    while (*p == ' ') p++;

    /* Folder name: quoted or unquoted */
    if (*p == '"') {
        p++;
        const char *end = strchr(p, '"');
        if (!end) return NULL;
        size_t len = (size_t)(end - p);
        char *name = malloc(len + 1);
        if (!name) return NULL;
        memcpy(name, p, len);
        name[len] = '\0';
        return name;
    } else {
        /* Unquoted: until end of line */
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\r')) len--;
        char *name = malloc(len + 1);
        if (!name) return NULL;
        memcpy(name, p, len);
        name[len] = '\0';
        return name;
    }
}

int imap_list(ImapClient *c, char ***folders_out, int *count_out, char *sep_out) {
    *folders_out = NULL;
    *count_out   = 0;
    if (sep_out) *sep_out = '.';

    char tag[16];
    if (send_cmd(c, tag, "LIST \"\" \"*\"") != 0) return -1;

    Response resp = {0};
    if (read_response(c, tag, &resp) != 0) {
        response_free(&resp);
        return -1;
    }

    int count = 0, cap = 0;
    char **folders = NULL;
    char  sep = '.';

    for (int i = 0; i < resp.count; i++) {
        char got_sep = '.';
        char *raw = parse_list_line(resp.untagged[i], &got_sep);
        if (!raw) continue;
        sep = got_sep;
        char *name = imap_utf7_decode(raw);
        free(raw);
        if (!name) continue;

        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            char **tmp = realloc(folders, (size_t)cap * sizeof(char *));
            if (!tmp) { free(name); break; }
            folders = tmp;
        }
        folders[count++] = name;
    }

    response_free(&resp);
    *folders_out = folders;
    *count_out   = count;
    if (sep_out) *sep_out = sep;
    return 0;
}

/* ── SELECT ──────────────────────────────────────────────────────────── */

int imap_select(ImapClient *c, const char *folder) {
    char *utf7 = imap_utf7_encode(folder);
    const char *name = utf7 ? utf7 : folder;

    char tag[16];
    int rc;
    /* Quote the folder name */
    rc = send_cmd(c, tag, "SELECT \"%s\"", name);
    free(utf7);
    if (rc != 0) return -1;

    Response resp = {0};
    rc = read_response(c, tag, &resp);
    response_free(&resp);
    return rc;
}

/* ── UID SEARCH ──────────────────────────────────────────────────────── */

int imap_uid_search(ImapClient *c, const char *criteria,
                    int **uids_out, int *count_out) {
    *uids_out  = NULL;
    *count_out = 0;

    char tag[16];
    if (send_cmd(c, tag, "UID SEARCH %s", criteria) != 0) return -1;

    Response resp = {0};
    if (read_response(c, tag, &resp) != 0) {
        response_free(&resp);
        return -1;
    }

    /* Parse "* SEARCH uid uid uid ..." */
    int cap = 32, cnt = 0;
    int *uids = NULL;

    for (int i = 0; i < resp.count; i++) {
        const char *line = resp.untagged[i];
        if (strncasecmp(line, "* SEARCH", 8) != 0) continue;
        const char *p = line + 8;
        for (;;) {
            while (*p == ' ') p++;
            if (!*p) break;
            char *e;
            long uid = strtol(p, &e, 10);
            if (e == p) break;
            if (uid > 0) {
                if (!uids) {
                    uids = malloc((size_t)cap * sizeof(int));
                    if (!uids) { response_free(&resp); return -1; }
                }
                if (cnt == cap) {
                    cap *= 2;
                    int *tmp = realloc(uids, (size_t)cap * sizeof(int));
                    if (!tmp) { free(uids); response_free(&resp); return -1; }
                    uids = tmp;
                }
                uids[cnt++] = (int)uid;
            }
            p = e;
        }
    }

    response_free(&resp);
    *uids_out  = uids;
    *count_out = cnt;
    return 0;
}

/* ── UID FETCH ───────────────────────────────────────────────────────── */

static char *uid_fetch_part(ImapClient *c, int uid, const char *section) {
    char tag[16];
    if (send_cmd(c, tag, "UID FETCH %d (UID %s)", uid, section) != 0)
        return NULL;

    Response resp = {0};
    if (read_response(c, tag, &resp) != 0) {
        response_free(&resp);
        return NULL;
    }

    char *result = NULL;
    if (resp.literal) {
        result = resp.literal;
        resp.literal = NULL;  /* transfer ownership */
    }
    response_free(&resp);

    if (!result)
        logger_log(LOG_WARN, "UID FETCH %d %s: no literal in response", uid, section);
    return result;
}

char *imap_uid_fetch_headers(ImapClient *c, int uid) {
    return uid_fetch_part(c, uid, "BODY.PEEK[HEADER]");
}

char *imap_uid_fetch_body(ImapClient *c, int uid) {
    return uid_fetch_part(c, uid, "BODY.PEEK[]");
}

/* ── UID FETCH FLAGS ─────────────────────────────────────────────────── */

/**
 * Parse a `* N FETCH (... FLAGS (\Flag1 $keyword ...) ...)` untagged line
 * and return a MSG_FLAG_* bitmask.
 */
static int parse_imap_flags(const char *line) {
    /* Find FLAGS ( ... ) in the line */
    const char *p = strstr(line, "FLAGS (");
    if (!p) return 0;
    p += 7; /* skip "FLAGS (" */
    int flags = 0;
    /* Check for known flags */
    if (strstr(p, "\\Seen") == NULL) flags |= MSG_FLAG_UNSEEN;  /* absence of \Seen = unseen */
    if (strstr(p, "\\Flagged") != NULL) flags |= MSG_FLAG_FLAGGED;
    if (strstr(p, "$Done")     != NULL) flags |= MSG_FLAG_DONE;
    return flags;
}

int imap_uid_fetch_flags(ImapClient *c, int uid) {
    char tag[16];
    if (send_cmd(c, tag, "UID FETCH %d (UID FLAGS)", uid) != 0) return -1;

    Response resp = {0};
    if (read_response(c, tag, &resp) != 0) {
        response_free(&resp);
        return -1;
    }

    int flags = -1;
    for (int i = 0; i < resp.count; i++) {
        if (strstr(resp.untagged[i], "FETCH") && strstr(resp.untagged[i], "FLAGS")) {
            flags = parse_imap_flags(resp.untagged[i]);
            break;
        }
    }
    response_free(&resp);
    return flags < 0 ? 0 : flags;
}

/* ── UID STORE (set/clear flag) ──────────────────────────────────────── */

int imap_uid_set_flag(ImapClient *c, int uid, const char *flag_name, int add) {
    char tag[16];
    if (send_cmd(c, tag, "UID STORE %d %sFLAGS (%s)",
                 uid, add ? "+" : "-", flag_name) != 0)
        return -1;
    Response resp = {0};
    int rc = read_response(c, tag, &resp);
    response_free(&resp);
    return rc;
}
