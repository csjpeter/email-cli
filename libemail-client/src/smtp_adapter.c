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
    NetConn *c;
    char     buf[4096];
    size_t   pos, len;
} SmtpLink;

/** One reply line (without CRLF): 1 ok, 0 end of stream, -1 error. */
static int link_line(SmtpLink *l, char *line, size_t cap)
{
    size_t n = 0;
    for (;;) {
        if (l->pos >= l->len) {
            ssize_t r = net_conn_read(l->c, l->buf, sizeof(l->buf));
            if (r < 0) return -1;
            if (r == 0) { if (n == 0) return 0; break; }
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

/** Send a command and expect reply @p want (the first digit when @p want < 10). */
static int link_cmd(SmtpLink *l, int want, char *why, size_t why_len,
                    const char *fmt, const char *arg)
{
    char text[512];
    if (link_send(l, fmt, arg) != 0) {
        snprintf(why, why_len, "Failed sending data to the server");
        return -1;
    }
    int code = link_reply(l, text, sizeof(text));
    int ok = want < 10 ? code / 100 == want : code == want;
    if (!ok) {
        char *nl = strchr(text, '\n');
        if (nl) *nl = '\0';
        if (code == 0) snprintf(why, why_len, "Server closed the connection or timed out");
        else snprintf(why, why_len, "Server replied %d %.200s", code, text);
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
    if (hl == 0 || hl >= sizeof(host)) { snprintf(why, why_len, "Bad SMTP URL"); return -1; }
    memcpy(host, hp, hl);
    host[hl] = '\0';
    if (hp[hl] == ':') port = atoi(hp + hl + 1);

    char err[160] = "";
    SmtpLink l = {0};
    l.c = net_connect(host, port, implicit, !no_verify, 30, err, sizeof(err));
    if (!l.c) { snprintf(why, why_len, "%s", err[0] ? err : "Could not connect to server"); return -1; }

    int rc = -1;
    char text[1024];
    char me[128];
    local_name(me, sizeof(me));
    int code = link_reply(&l, text, sizeof(text));
    if (code != 220) { snprintf(why, why_len, "Bad greeting from server"); goto out; }
    if (link_send(&l, "EHLO %s", me) != 0) goto out;
    code = link_reply(&l, text, sizeof(text));
    if (code / 100 != 2) { snprintf(why, why_len, "Server rejected EHLO"); goto out; }
    if (!implicit) {
        /* STARTTLS is mandatory: a plain session would carry the password */
        if (!strcasestr(text, "STARTTLS")) {
            snprintf(why, why_len, "Server does not offer STARTTLS (TLS required)");
            goto out;
        }
        if (link_cmd(&l, 220, why, why_len, "%s", "STARTTLS") != 0) goto out;
        l.pos = l.len = 0;
        if (net_start_tls(l.c, host, !no_verify, err, sizeof(err)) != 0) {
            snprintf(why, why_len, "%s", err[0] ? err : "TLS handshake failed");
            goto out;
        }
        if (link_send(&l, "EHLO %s", me) != 0) goto out;
        code = link_reply(&l, text, sizeof(text));
        if (code / 100 != 2) { snprintf(why, why_len, "Server rejected EHLO"); goto out; }
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
                int ar = link_cmd(&l, 235, why, why_len, "AUTH PLAIN %s", enc);
                free(enc);
                if (ar != 0) {
                    char said[160];
                    snprintf(said, sizeof(said), "%s", why);
                    snprintf(why, why_len, "Authentication failed: %.100s", said);
                    goto out;
                }
            } else {
                char *eu = b64((const unsigned char *)user, ul);
                char *ep = b64((const unsigned char *)(pass ? pass : ""), pl);
                int ar = -1;
                if (eu && ep && link_cmd(&l, 334, why, why_len, "%s", "AUTH LOGIN") == 0 &&
                    link_cmd(&l, 334, why, why_len, "%s", eu) == 0 &&
                    link_cmd(&l, 235, why, why_len, "%s", ep) == 0) ar = 0;
                free(eu);
                free(ep);
                if (ar != 0) { snprintf(why, why_len, "Authentication failed"); goto out; }
            }
        }
    }
    if (link_cmd(&l, 2, why, why_len, "MAIL FROM:%s", from_env) != 0) goto out;
    if (link_cmd(&l, 2, why, why_len, "RCPT TO:%s", to_env) != 0) goto out;
    if (link_cmd(&l, 354, why, why_len, "%s", "DATA") != 0) goto out;

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
        if (wr != 0) { snprintf(why, why_len, "Failed sending the message"); goto out; }
    }
    code = link_reply(&l, text, sizeof(text));
    if (code / 100 != 2) {
        char *nl = strchr(text, '\n');
        if (nl) *nl = '\0';
        snprintf(why, why_len, "Server did not accept the message: %d %.150s", code, text);
        goto out;
    }
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
              size_t message_len) {
    if (!cfg || !from || !to || !message) return -1;

    const char *smtp_user = cfg->smtp_user ? cfg->smtp_user : cfg->user;
    const char *smtp_pass = cfg->smtp_pass ? cfg->smtp_pass : cfg->pass;

    char *url = build_smtp_url(cfg);
    if (!url) {
        fprintf(stderr, "smtp_send: failed to build SMTP URL.\n");
        return -1;
    }

    /* Hard enforcement: only smtps:// (implicit TLS) is allowed.
     * Exception: cfg->ssl_no_verify=1 permits smtp:// for test environments. */
    if (strncmp(url, "smtps://", 8) != 0 && !cfg->ssl_no_verify) {
        fprintf(stderr,
                "smtp_send: refused to send via %s — "
                "only smtps:// is allowed (TLS required).\n"
                "Set SMTP_HOST=smtps://smtp.example.com in your config.\n"
                "For test environments only: add SSL_NO_VERIFY=1 to config.\n", url);
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

    char to_env[512];
    const char *lt2 = strchr(to, '<');
    const char *gt2 = lt2 ? strchr(lt2, '>') : NULL;
    if (lt2 && gt2 && gt2 > lt2) {
        size_t alen = (size_t)(gt2 - lt2 - 1);
        if (alen >= sizeof(to_env)) alen = sizeof(to_env) - 1;
        snprintf(to_env, sizeof(to_env), "<%.*s>", (int)alen, lt2 + 1);
    } else {
        snprintf(to_env, sizeof(to_env), "<%s>", to);
    }

    char why[256] = "";
    int rc = smtp_deliver(url, smtp_user, smtp_pass, cfg->ssl_no_verify,
                          from_env, to_env, message, message_len, why, sizeof(why));
    if (rc != 0) {
        fprintf(stderr, "smtp_send: %s\n", why);
        logger_log(LOG_ERROR, "smtp_send failed: %s", why);
    } else {
        logger_log(LOG_INFO, "smtp_send: message sent from %s to %s", from, to);
    }
    free(url);
    return rc;
}
