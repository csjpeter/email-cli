/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-04-13 */
/* SMTP sender of RFC 2822 messages, on the library's own TLS connection. */
#include "smtp_adapter.h"
#include "logger.h"
#include "net_tls.h"
#include <openssl/evp.h>
#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file smtp_adapter.c
 * @brief SMTP adapter for sending RFC 2822 messages (no libcurl, EMAIL-103).
 */

/* ── URL construction ─────────────────────────────────────────────── */

/**
 * Build the SMTP URL from config.
 * Returns heap-allocated string; caller must free().
 */
static char *build_smtp_url(const Config *cfg) {
    char *url = NULL;

    if (cfg->smtp_host && cfg->smtp_host[0]) {
        /* Use configured SMTP host; append port if specified */
        if (cfg->smtp_port) {
            /* Check if port is already embedded in the URL */
            const char *after_scheme = strstr(cfg->smtp_host, "://");
            int has_port = after_scheme && strchr(after_scheme + 3, ':') != NULL;
            if (has_port) {
                url = strdup(cfg->smtp_host);
            } else {
                if (asprintf(&url, "%s:%d", cfg->smtp_host, cfg->smtp_port) < 0)
                    url = NULL;
            }
        } else {
            url = strdup(cfg->smtp_host);
        }
    } else if (cfg->host) {
        /* Derive SMTP URL from IMAP URL */
        if (strncmp(cfg->host, "imaps://", 8) == 0) {
            if (asprintf(&url, "smtps://%s", cfg->host + 8) < 0) url = NULL;
        } else if (strncmp(cfg->host, "imap://", 7) == 0) {
            if (asprintf(&url, "smtp://%s", cfg->host + 7) < 0) url = NULL;
        }
        else
            url = strdup(cfg->host);
    } else {
        url = strdup("smtp://localhost");
    }
    return url;
}

/* ── SMTP session ─────────────────────────────────────────────────── */

typedef struct {
    NetConn    *c;
    char        buf[4096];
    size_t      pos, len;
    /* Where the failure message says it happened (EMAIL-237). */
    const char *host;
    int         port;
    const char *user;
    int         io_state;   /* 0 none, 1 closed by the server, 2 error or timeout */
    int         io_errno;
} SmtpLink;

/** Read and write limit of an SMTP session, seconds (smtp_set_io_timeout). */
static int g_smtp_io_timeout = 30;

void smtp_set_io_timeout(int seconds)
{
    g_smtp_io_timeout = seconds > 0 ? seconds : 30;
}

/** Make @p s one printable line: cut at the first line end, controls to blanks. */
static void clean_line(char *s)
{
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n') { *s = '\0'; return; }
        if ((unsigned char)*s < 0x20 || *s == 0x7f) *s = ' ';
    }
}

/**
 * Write the failure message of one protocol step: the step, the server and
 * port, the account, and the server's status line (bounded), or what happened
 * to the connection when there was no reply.  No credential and no message
 * body is ever put into it.  @p code 0 means no reply.
 */
static void smtp_why(const SmtpLink *l, const char *step, int code, const char *text,
                     char *why, size_t why_len)
{
    char acct[160] = "";
    if (l->user && l->user[0]) snprintf(acct, sizeof(acct), " as %.100s", l->user);
    if (code > 0) {
        char line[220];
        snprintf(line, sizeof(line), "%.200s", text ? text : "");
        clean_line(line);
        snprintf(why, why_len, "SMTP %s at %s:%d%s failed: server replied %d %s",
                 step, l->host, l->port, acct, code, line);
    } else if (l->io_state == 1) {
        snprintf(why, why_len, "SMTP %s at %s:%d%s failed: no reply, the server closed the connection",
                 step, l->host, l->port, acct);
    } else {
        const char *ne = net_conn_error(l->c);
        snprintf(why, why_len, "SMTP %s at %s:%d%s failed: no reply, %s",
                 step, l->host, l->port, acct, ne[0] ? ne : "connection error");
    }
}

/** One reply line (without CRLF): 1 ok, 0 end of stream, -1 error. */
static int link_line(SmtpLink *l, char *line, size_t cap)
{
    size_t n = 0;
    for (;;) {
        if (l->pos >= l->len) {
            errno = 0;
            ssize_t r = net_conn_read(l->c, l->buf, sizeof(l->buf));
            if (r < 0) { l->io_state = 2; l->io_errno = errno; return -1; }
            if (r == 0) { l->io_state = 1; if (n == 0) return 0; break; }
            l->pos = 0;
            l->len = (size_t)r;
        }
        char ch = l->buf[l->pos++];
        if (ch == '\n') break;
        if (n + 1 < cap) line[n++] = ch;
    }
    if (n && line[n - 1] == '\r') n--;
    line[n] = '\0';
    return 1;
}

/**
 * Read a (possibly multi-line) reply.  Returns the reply code, 0 on a
 * transport error.  The text of all lines is appended to @p text.
 */
static int link_reply(SmtpLink *l, char *text, size_t text_cap)
{
    char line[1024];
    int code = 0;
    if (text && text_cap) text[0] = '\0';
    for (;;) {
        if (link_line(l, line, sizeof(line)) <= 0) return 0;
        if (strlen(line) < 3 || !isdigit((unsigned char)line[0]) ||
            !isdigit((unsigned char)line[1]) || !isdigit((unsigned char)line[2]))
            return 0;
        code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
        if (text && text_cap) {
            size_t have = strlen(text);
            snprintf(text + have, text_cap - have, "%s\n", line + (line[3] ? 4 : 3));
        }
        logger_log(LOG_DEBUG, "SMTP [ IN] %s", line);
        if (line[3] != '-') return code;
    }
}

static int link_send(SmtpLink *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int link_send(SmtpLink *l, const char *fmt, ...)
{
    char out[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(out) - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(out) - 2) return -1;
    logger_log(LOG_DEBUG, "SMTP [OUT] %.*s", n > 5 && strncasecmp(out, "AUTH", 4) == 0 ? 4 : n, out);
    out[n++] = '\r';
    out[n++] = '\n';
    return net_conn_write_all(l->c, out, (size_t)n);
}

/** Send a command and expect reply @p want (the first digit when @p want < 10);
 *  @p step names the step in the failure message. */
static int link_cmd(SmtpLink *l, const char *step, int want, char *why, size_t why_len,
                    const char *fmt, const char *arg)
{
    char text[512];
    l->io_state = 0;
    if (link_send(l, fmt, arg) != 0) {
        l->io_state = 2;
        l->io_errno = errno;
        smtp_why(l, step, 0, NULL, why, why_len);
        return -1;
    }
    int code = link_reply(l, text, sizeof(text));
    int ok = want < 10 ? code / 100 == want : code == want;
    if (!ok) {
        smtp_why(l, step, code, text, why, why_len);
        return -1;
    }
    return 0;
}

static char *b64(const unsigned char *d, size_t n)
{
    char *o = malloc(4 * ((n + 2) / 3) + 1);
    if (!o) return NULL;
    int len = EVP_EncodeBlock((unsigned char *)o, d, (int)n);
    o[len] = '\0';
    return o;
}

static int local_name(char *out, size_t cap)
{
    if (gethostname(out, cap) != 0 || !out[0]) snprintf(out, cap, "localhost");
    out[cap - 1] = '\0';
    return 0;
}

/**
 * Turn a recipient list ("A <a@x>, b@y; c@z") into the envelope form: one
 * "<addr>" per line.  Commas and semicolons separate the entries.
 */
static void envelope_list(const char *list, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    const char *p = list;
    while (*p) {
        size_t el = strcspn(p, ",;");
        const char *lt = memchr(p, '<', el);
        const char *gt = lt ? memchr(lt, '>', (size_t)(p + el - lt)) : NULL;
        const char *a = p, *e = p + el;
        if (lt && gt) { a = lt + 1; e = gt; }
        while (a < e && isspace((unsigned char)*a)) a++;
        while (e > a && isspace((unsigned char)e[-1])) e--;
        if (e > a && n + (size_t)(e - a) + 4 < cap)
            n += (size_t)snprintf(out + n, cap - n, "<%.*s>\n", (int)(e - a), a);
        p += el;
        if (*p) p++;
    }
}

/**
 * Deliver one message.  @p url is smtps://host[:port] (implicit TLS) or
 * smtp://host[:port] (STARTTLS is required, a plain session is never used).
 * @return 0 on success, -1 on error with the reason in @p why.
 */
static int smtp_deliver(const char *url, const char *user, const char *pass,
                        int no_verify, const char *from_env, const char *to_env,
                        const char *msg, size_t msg_len, char *why, size_t why_len)
{
    int implicit = strncmp(url, "smtps://", 8) == 0;
    const char *hp = url + (implicit ? 8 : 7);
    char host[256];
    int port = implicit ? 465 : 25;
    size_t hl = strcspn(hp, ":/");
    if (hl == 0 || hl >= sizeof(host)) { snprintf(why, why_len, "SMTP connect failed: the SMTP URL has no usable host name"); return -1; }
    memcpy(host, hp, hl);
    host[hl] = '\0';
    if (hp[hl] == ':') port = atoi(hp + hl + 1);

    char err[500] = "";
    SmtpLink l = {0};
    l.host = host;
    l.port = port;
    l.user = user;
    l.c = net_connect(host, port, implicit, !no_verify, g_smtp_io_timeout, err, sizeof(err));
    if (!l.c) {
        snprintf(why, why_len, "SMTP %s at %.80s:%d%s%.40s failed: %.220s",
                 implicit ? "connect/TLS" : "connect", host, port,
                 user && user[0] ? " as " : "", user && user[0] ? user : "",
                 err[0] ? err : "could not connect");
        return -1;
    }

    int rc = -1;
    char text[1024];
    char me[128];
    local_name(me, sizeof(me));
    int code = link_reply(&l, text, sizeof(text));
    if (code != 220) { smtp_why(&l, "greeting", code, text, why, why_len); goto out; }
    if (link_send(&l, "EHLO %s", me) != 0) { smtp_why(&l, "EHLO", 0, NULL, why, why_len); goto out; }
    code = link_reply(&l, text, sizeof(text));
    if (code / 100 != 2) { smtp_why(&l, "EHLO", code, text, why, why_len); goto out; }
    if (!implicit) {
        /* STARTTLS is mandatory: a plain session would carry the password */
        if (!strcasestr(text, "STARTTLS")) {
            snprintf(why, why_len, "SMTP STARTTLS at %s:%d%s%.60s failed: the server does not offer "
                     "STARTTLS (TLS is required, the password is not sent in the clear)", host, port,
                     user && user[0] ? " as " : "", user && user[0] ? user : "");
            goto out;
        }
        if (link_cmd(&l, "STARTTLS", 220, why, why_len, "%s", "STARTTLS") != 0) goto out;
        l.pos = l.len = 0;
        if (net_start_tls(l.c, host, !no_verify, err, sizeof(err)) != 0) {
            snprintf(why, why_len, "SMTP TLS at %.100s:%d%s%.60s failed: %.150s", host, port,
                     user && user[0] ? " as " : "", user && user[0] ? user : "",
                     err[0] ? err : "TLS handshake failed");
            goto out;
        }
        if (link_send(&l, "EHLO %s", me) != 0) { smtp_why(&l, "EHLO", 0, NULL, why, why_len); goto out; }
        code = link_reply(&l, text, sizeof(text));
        if (code / 100 != 2) { smtp_why(&l, "EHLO", code, text, why, why_len); goto out; }
    }

    if (user && user[0]) {
        const char *auth = strcasestr(text, "AUTH");
        if (auth) {
            size_t ul = strlen(user), pl = pass ? strlen(pass) : 0;
            if (strcasestr(auth, "PLAIN")) {
                unsigned char *raw = malloc(ul + pl + 3);
                if (!raw) goto out;
                raw[0] = 0;
                memcpy(raw + 1, user, ul);
                raw[ul + 1] = 0;
                if (pl) memcpy(raw + ul + 2, pass, pl);
                char *enc = b64(raw, ul + pl + 2);
                free(raw);
                if (!enc) goto out;
                int ar = link_cmd(&l, "AUTH", 235, why, why_len, "AUTH PLAIN %s", enc);
                free(enc);
                if (ar != 0) goto out;
            } else {
                char *eu = b64((const unsigned char *)user, ul);
                char *ep = b64((const unsigned char *)(pass ? pass : ""), pl);
                int ar = -1;
                if (eu && ep && link_cmd(&l, "AUTH", 334, why, why_len, "%s", "AUTH LOGIN") == 0 &&
                    link_cmd(&l, "AUTH", 334, why, why_len, "%s", eu) == 0 &&
                    link_cmd(&l, "AUTH", 235, why, why_len, "%s", ep) == 0) ar = 0;
                free(eu);
                free(ep);
                if (ar != 0) goto out;
            }
        }
    }
    if (link_cmd(&l, "MAIL FROM", 2, why, why_len, "MAIL FROM:%s", from_env) != 0) goto out;
    /* to_env is a newline-separated list of "<addr>" (envelope_list) */
    for (const char *r = to_env; *r; ) {
        size_t rl = strcspn(r, "\n");
        if (rl == 0) { r++; continue; }
        char one[600];
        snprintf(one, sizeof(one), "%.*s", (int)rl, r);
        if (link_cmd(&l, "RCPT TO", 2, why, why_len, "RCPT TO:%s", one) != 0) goto out;
        r += rl;
    }
    if (link_cmd(&l, "DATA", 354, why, why_len, "%s", "DATA") != 0) goto out;

    /* the message: CRLF line ends, leading dots doubled, closed by "." */
    {
        size_t cap = msg_len * 2 + 16;
        char *o = malloc(cap);
        if (!o) goto out;
        size_t n = 0;
        int bol = 1;
        for (size_t i = 0; i < msg_len; i++) {
            char ch = msg[i];
            if (bol && ch == '.') o[n++] = '.';
            if (ch == '\n' && (i == 0 || msg[i - 1] != '\r')) o[n++] = '\r';
            o[n++] = ch;
            bol = ch == '\n';
        }
        if (n < 2 || o[n - 2] != '\r' || o[n - 1] != '\n') { o[n++] = '\r'; o[n++] = '\n'; }
        o[n++] = '.'; o[n++] = '\r'; o[n++] = '\n';
        int wr = net_conn_write_all(l.c, o, n);
        free(o);
        if (wr != 0) {
            l.io_state = 2;
            l.io_errno = errno;
            smtp_why(&l, "message", 0, NULL, why, why_len);
            goto out;
        }
    }
    l.io_state = 0;
    code = link_reply(&l, text, sizeof(text));
    if (code / 100 != 2) { smtp_why(&l, "message", code, text, why, why_len); goto out; }
    link_send(&l, "QUIT");
    rc = 0;
out:
    net_close(l.c);
    return rc;
}

/* ── Public API ──────────────────────────────────────────────────── */

int smtp_send(const Config *cfg,
              const char *from,
              const char *to,
              const char *message,
              size_t message_len,
              char *err,
              size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!cfg || !from || !to || !message) {
        if (err && err_len) snprintf(err, err_len, "missing argument");
        return -1;
    }

    const char *smtp_user = cfg->smtp_user ? cfg->smtp_user : cfg->user;
    const char *smtp_pass = cfg->smtp_pass ? cfg->smtp_pass : cfg->pass;

    char *url = build_smtp_url(cfg);
    if (!url) {
        if (err && err_len) snprintf(err, err_len, "failed to build the SMTP URL");
        logger_log(LOG_ERROR, "smtp_send: failed to build the SMTP URL");
        return -1;
    }

    /* Hard enforcement: only smtps:// (implicit TLS) is allowed.
     * Exception: cfg->ssl_no_verify=1 permits smtp:// for test environments. */
    if (strncmp(url, "smtps://", 8) != 0 && !cfg->ssl_no_verify) {
        if (err && err_len)
            snprintf(err, err_len,
                     "refused to send via %s - only smtps:// is allowed (TLS required). "
                     "Set SMTP_HOST=smtps://smtp.example.com in your config "
                     "(for test environments only: SSL_NO_VERIFY=1)", url);
        logger_log(LOG_ERROR,
                   "smtp_send: rejected insecure SMTP URL: %s", url);
        free(url);
        return -1;
    }

    /* Envelope From: strip display name, extract bare address */
    char from_env[512];
    const char *lt = strchr(from, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt && gt > lt) {
        size_t alen = (size_t)(gt - lt - 1);
        if (alen >= sizeof(from_env)) alen = sizeof(from_env) - 1;
        snprintf(from_env, sizeof(from_env), "<%.*s>", (int)alen, lt + 1);
    } else {
        snprintf(from_env, sizeof(from_env), "<%s>", from);
    }

    char to_env[2048];
    envelope_list(to, to_env, sizeof(to_env));

    char why[400] = "";
    int rc = smtp_deliver(url, smtp_user, smtp_pass, cfg->ssl_no_verify,
                          from_env, to_env, message, message_len, why, sizeof(why));
    if (rc != 0) {
        if (err && err_len) snprintf(err, err_len, "%s", why);
        logger_log(LOG_ERROR, "smtp_send failed: %s", why);
    } else {
        logger_log(LOG_INFO, "smtp_send: message sent from %s to %s", from, to);
    }
    free(url);
    return rc;
}
