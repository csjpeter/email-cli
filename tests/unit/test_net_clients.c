/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* Unit tests of the library's own HTTP and SMTP clients (EMAIL-103). */
#include "test_helpers.h"
#include "http_client.h"
#include "net_tls.h"
#include "smtp_adapter.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * @file test_net_clients.c
 * @brief The HTTP client (Content-Length, chunked and close-delimited bodies,
 *        errors, limits), the URL escape, the CA bundle choice and the SMTP
 *        client (implicit TLS, STARTTLS, AUTH PLAIN and LOGIN, rejections,
 *        dot stuffing), all against forked mock servers on loopback.
 */

/* ── Plumbing ───────────────────────────────────────────────────────── */

static int nc_listen(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    socklen_t l = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &l);
    *port = ntohs(a.sin_port);
    return fd;
}

static int nc_dead_port(void)
{
    int port = 0;
    int fd = nc_listen(&port);
    close(fd);
    return port;
}

/* ── HTTP mock ──────────────────────────────────────────────────────── */

/** Read a whole request (headers and Content-Length body) into @p req. */
static int http_read_request(int c, char *req, size_t cap)
{
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t n = read(c, req + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        req[total] = '\0';
        char *he = strstr(req, "\r\n\r\n");
        if (he) {
            char *cl = strcasestr(req, "Content-Length:");
            long want = cl ? strtol(cl + 15, NULL, 10) : 0;
            if ((long)total >= (he - req) + 4 + want) break;
        }
    }
    return (int)total;
}

static void sendall(int c, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(c, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static pid_t http_mock(int lfd, int rounds)
{
    pid_t pid = fork();
    if (pid != 0) { close(lfd); return pid; }
    alarm(60);
    for (int i = 0; i < rounds; i++) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) break;
        char req[16384];
        http_read_request(c, req, sizeof(req));
        char path[256] = "";
        sscanf(req, "%*s %255s", path);
        if (strcmp(path, "/cl") == 0) {
            const char *r = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nhello world";
            sendall(c, r, strlen(r));
        } else if (strcmp(path, "/chunked") == 0) {
            const char *r = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                            "5\r\nhello\r\n6;ext=1\r\n world\r\n1\r\n!\r\n0\r\nX-Trailer: t\r\n\r\n";
            sendall(c, r, strlen(r));
        } else if (strcmp(path, "/close") == 0) {
            const char *r = "HTTP/1.0 200 OK\r\n\r\nuntil the end";
            sendall(c, r, strlen(r));
        } else if (strcmp(path, "/204") == 0) {
            const char *r = "HTTP/1.1 204 No Content\r\n\r\n";
            sendall(c, r, strlen(r));
        } else if (strcmp(path, "/404") == 0) {
            const char *r = "HTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\n{}";
            sendall(c, r, strlen(r));
        } else if (strcmp(path, "/echo") == 0 || strncmp(path, "/echo?", 6) == 0) {
            char r[20000];
            int n = snprintf(r, sizeof(r),
                             "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s", strlen(req), req);
            sendall(c, r, (size_t)n);
        } else if (strcmp(path, "/trunc") == 0) {
            const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort";
            sendall(c, r, strlen(r));
        } else if (strcmp(path, "/big") == 0) {
            char *body = malloc(65536);
            memset(body, 'x', 65536);
            char head[128];
            int n = snprintf(head, sizeof(head),
                             "HTTP/1.1 200 OK\r\nContent-Length: 65536\r\n\r\n");
            sendall(c, head, (size_t)n);
            sendall(c, body, 65536);
            free(body);
        } else if (strcmp(path, "/slow") == 0) {
            sleep(5);
        } else if (strcmp(path, "/garbage") == 0) {
            const char *r = "this is not http\r\n\r\n";
            sendall(c, r, strlen(r));
        }
        close(c);
    }
    close(lfd);
    _exit(0);
}

static void http_url(char *out, size_t n, int port, const char *path)
{
    snprintf(out, n, "http://127.0.0.1:%d%s", port, path);
}

static int http_get(int port, const char *path, long *st, char **body, size_t *len,
                    char *err, size_t el)
{
    char url[128];
    http_url(url, sizeof(url), port, path);
    HttpRequest r = { .method = "GET", .url = url, .timeout_s = 5 };
    return http_request(&r, st, body, len, err, el);
}

static void test_http_content_length(void)
{
    int port; int l = nc_listen(&port);
    pid_t p = http_mock(l, 1);
    long st; char *b; size_t n; char err[128];
    ASSERT(http_get(port, "/cl", &st, &b, &n, err, sizeof(err)) == 0, "GET with Content-Length succeeds");
    ASSERT(st == 200, "status 200");
    ASSERT(n == 11 && strcmp(b, "hello world") == 0, "body read by length");
    free(b);
    waitpid(p, NULL, 0);
}

static void test_http_chunked(void)
{
    int port; int l = nc_listen(&port);
    pid_t p = http_mock(l, 1);
    long st; char *b; size_t n; char err[128];
    ASSERT(http_get(port, "/chunked", &st, &b, &n, err, sizeof(err)) == 0, "chunked response succeeds");
    ASSERT(n == 12 && strcmp(b, "hello world!") == 0, "chunks joined, extension and trailer ignored");
    free(b);
    waitpid(p, NULL, 0);
}

static void test_http_close_delimited(void)
{
    int port; int l = nc_listen(&port);
    pid_t p = http_mock(l, 1);
    long st; char *b; size_t n; char err[128];
    ASSERT(http_get(port, "/close", &st, &b, &n, err, sizeof(err)) == 0, "close-delimited response succeeds");
    ASSERT(strcmp(b, "until the end") == 0, "body runs to the end of the connection");
    free(b);
    waitpid(p, NULL, 0);
}

static void test_http_no_content_and_error_status(void)
{
    int port; int l = nc_listen(&port);
    pid_t p = http_mock(l, 2);
    long st; char *b; size_t n; char err[128];
    ASSERT(http_get(port, "/204", &st, &b, &n, err, sizeof(err)) == 0 && st == 204, "204 is a response");
    ASSERT(b != NULL && n == 0, "with an empty body, not NULL");
    free(b);
    ASSERT(http_get(port, "/404", &st, &b, &n, err, sizeof(err)) == 0, "404 is a response too");
    ASSERT(st == 404 && strcmp(b, "{}") == 0, "its status and body are returned");
    free(b);
    waitpid(p, NULL, 0);
}

static void test_http_request_is_well_formed(void)
{
    int port; int l = nc_listen(&port);
    pid_t p = http_mock(l, 2);
    char url[128];
    http_url(url, sizeof(url), port, "/echo?a=1");
    const char *hdr[] = { "Authorization: Bearer tok", "Content-Type: application/json", NULL };
    HttpRequest r = { .method = "POST", .url = url, .headers = hdr,
                      .body = "{\"x\":1}", .body_len = 7, .timeout_s = 5 };
    long st; char *b; size_t n; char err[128];
    ASSERT(http_request(&r, &st, &b, &n, err, sizeof(err)) == 0, "POST succeeds");
    ASSERT(strncmp(b, "POST /echo?a=1 HTTP/1.1\r\n", 25) == 0, "request line has method, path and query");
    char host[64];
    snprintf(host, sizeof(host), "Host: 127.0.0.1:%d\r\n", port);
    ASSERT(strstr(b, host) != NULL, "Host header carries the port");
    ASSERT(strstr(b, "Authorization: Bearer tok\r\n") != NULL, "caller headers are sent");
    ASSERT(strstr(b, "Content-Length: 7\r\n") != NULL, "Content-Length is the body length");
    ASSERT(strstr(b, "Connection: close\r\n") != NULL, "one request per connection");
    ASSERT(strstr(b, "\r\n\r\n{\"x\":1}") != NULL, "the body follows the headers");
    free(b);

    HttpRequest d = { .method = "DELETE", .url = url, .timeout_s = 5 };
    ASSERT(http_request(&d, &st, &b, &n, err, sizeof(err)) == 0, "DELETE succeeds");
    ASSERT(strncmp(b, "DELETE /echo?a=1 HTTP/1.1\r\n", 27) == 0, "DELETE request line");
    ASSERT(strstr(b, "Content-Length") == NULL, "a bodiless DELETE sends no Content-Length");
    free(b);
    waitpid(p, NULL, 0);
}

static void test_http_failures(void)
{
    int port; int l = nc_listen(&port);
    pid_t p = http_mock(l, 4);
    long st; char *b; size_t n; char err[128];
    ASSERT(http_get(port, "/trunc", &st, &b, &n, err, sizeof(err)) == -1, "a truncated body is an error");
    ASSERT(strstr(err, "Truncated") != NULL, "named as such");
    ASSERT(b == NULL, "no body is returned on failure");
    ASSERT(http_get(port, "/garbage", &st, &b, &n, err, sizeof(err)) == -1, "a non-HTTP answer is an error");
    ASSERT(strstr(err, "Bad HTTP response") != NULL, "named as such");

    char url[128];
    http_url(url, sizeof(url), port, "/big");
    HttpRequest r = { .method = "GET", .url = url, .timeout_s = 5, .max_response = 1000 };
    ASSERT(http_request(&r, &st, &b, &n, err, sizeof(err)) == -1, "a body over the limit is an error");

    http_url(url, sizeof(url), port, "/slow");
    HttpRequest s = { .method = "GET", .url = url, .timeout_s = 1 };
    time_t t0 = time(NULL);
    ASSERT(http_request(&s, &st, &b, &n, err, sizeof(err)) == -1, "no answer within the timeout is an error");
    ASSERT(time(NULL) - t0 < 4, "and it does not wait much longer than asked");
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
}

static void test_http_bad_urls_and_unreachable(void)
{
    long st; char *b; size_t n; char err[128];
    HttpRequest r = { .method = "GET", .url = "ftp://example.org/", .timeout_s = 2 };
    ASSERT(http_request(&r, &st, &b, &n, err, sizeof(err)) == -1, "only http and https are accepted");
    ASSERT(strcmp(err, "Bad URL") == 0, "named as a bad URL");
    r.url = "http://:80/";
    ASSERT(http_request(&r, &st, &b, &n, err, sizeof(err)) == -1, "an empty host is refused");
    r.url = "http://127.0.0.1:99999/";
    ASSERT(http_request(&r, &st, &b, &n, err, sizeof(err)) == -1, "a port out of range is refused");
    ASSERT(http_request(NULL, &st, &b, &n, err, sizeof(err)) == -1, "a NULL request is refused");

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", nc_dead_port());
    r.url = url;
    ASSERT(http_request(&r, &st, &b, &n, err, sizeof(err)) == -1, "a refused connection is an error");
    ASSERT(strstr(err, "Could not connect") != NULL, "named as such");

    r.url = "http://no-such-host.invalid/";
    ASSERT(http_request(&r, &st, &b, &n, err, sizeof(err)) == -1, "an unresolvable host is an error");
    ASSERT(strstr(err, "resolve") != NULL, "named as such");
}

static void test_http_url_escape(void)
{
    char *e = http_url_escape("a b&c=d/é~-._");
    ASSERT(e && strcmp(e, "a%20b%26c%3Dd%2F%C3%A9~-._") == 0, "reserved characters are percent-encoded, unreserved kept");
    free(e);
    ASSERT(http_url_escape(NULL) == NULL, "NULL gives NULL");
    e = http_url_escape("");
    ASSERT(e && e[0] == '\0', "empty stays empty");
    free(e);
}

static void test_net_ca_bundle_choice(void)
{
    setenv("SSL_CERT_FILE", "/nonexistent/ca.pem", 1);
    ASSERT(strcmp(net_tls_ca_bundle(), "/nonexistent/ca.pem") == 0, "SSL_CERT_FILE names the trusted certificates");
    unsetenv("SSL_CERT_FILE");
    const char *b = net_tls_ca_bundle();
    ASSERT(b == NULL || access(b, R_OK) == 0, "otherwise a readable well-known bundle or none");
    SSL_CTX *c = net_tls_ctx_new(1);
    ASSERT(c != NULL, "a verifying context can be made");
    SSL_CTX_free(c);
    c = net_tls_ctx_new(0);
    ASSERT(c != NULL && SSL_CTX_get_verify_mode(c) == SSL_VERIFY_NONE, "a non-verifying one verifies nothing");
    SSL_CTX_free(c);
}

/* ── SMTP mock ──────────────────────────────────────────────────────── */

typedef enum {
    SM_OK,            /* AUTH PLAIN LOGIN offered, everything accepted */
    SM_LOGIN_ONLY,    /* only AUTH LOGIN offered */
    SM_AUTH_FAIL,     /* 535 after AUTH */
    SM_RCPT_FAIL,     /* 550 for RCPT */
    SM_NO_STARTTLS,   /* plain server without STARTTLS */
    SM_DATA_FAIL,     /* 554 after the message (EMAIL-139) */
    SM_TLS_DEAD       /* says "go ahead" to STARTTLS and then speaks no TLS (EMAIL-139) */
} SmtpMode;

typedef struct { int fd; SSL *ssl; } Link;

static int ln_read(Link *l, char *b, int n)
{
    return l->ssl ? SSL_read(l->ssl, b, n) : (int)read(l->fd, b, (size_t)n);
}

static void ln_say(Link *l, const char *s)
{
    if (l->ssl) SSL_write(l->ssl, s, (int)strlen(s));
    else sendall(l->fd, s, strlen(s));
}

static int ln_line(Link *l, char *line, int cap)
{
    int n = 0;
    char ch;
    while (n < cap - 1) {
        if (ln_read(l, &ch, 1) != 1) return -1;
        if (ch == '\n') break;
        if (ch != '\r') line[n++] = ch;
    }
    line[n] = '\0';
    return n;
}

/** Serves one session; the transcript (commands and message) goes to @p rep. */
static void smtp_session(Link *l, SmtpMode mode, int starttls, SSL_CTX *tctx, int rep)
{
    char line[2048];
    ln_say(l, "220 mock ESMTP\r\n");
    for (;;) {
        if (ln_line(l, line, sizeof(line)) < 0) return;
        if (strncasecmp(line, "AUTH", 4) == 0) {
            char show[64];
            snprintf(show, sizeof(show), "%.32s\n", line);
            sendall(rep, show, strlen(show));
        } else if (strncasecmp(line, "DATA", 4) != 0) {
            dprintf(rep, "%s\n", line);
        }
        if (strncasecmp(line, "EHLO", 4) == 0) {
            const char *auth = mode == SM_LOGIN_ONLY ? "250-AUTH LOGIN\r\n" : "250-AUTH PLAIN LOGIN\r\n";
            ln_say(l, "250-mock\r\n");
            if (starttls && !l->ssl) ln_say(l, "250-STARTTLS\r\n");
            ln_say(l, auth);
            ln_say(l, "250 OK\r\n");
        } else if (strncasecmp(line, "STARTTLS", 8) == 0) {
            ln_say(l, "220 go ahead\r\n");
            if (mode == SM_TLS_DEAD) return;
            l->ssl = SSL_new(tctx);
            SSL_set_fd(l->ssl, l->fd);
            if (SSL_accept(l->ssl) != 1) return;
        } else if (strncasecmp(line, "AUTH PLAIN", 10) == 0) {
            ln_say(l, mode == SM_AUTH_FAIL ? "535 5.7.8 bad\r\n" : "235 ok\r\n");
        } else if (strncasecmp(line, "AUTH LOGIN", 10) == 0) {
            ln_say(l, "334 VXNlcm5hbWU6\r\n");
            ln_line(l, line, sizeof(line));
            dprintf(rep, "LOGINUSER %s\n", line);
            ln_say(l, "334 UGFzc3dvcmQ6\r\n");
            ln_line(l, line, sizeof(line));
            dprintf(rep, "LOGINPASS %s\n", line);
            ln_say(l, mode == SM_AUTH_FAIL ? "535 5.7.8 bad\r\n" : "235 ok\r\n");
        } else if (strncasecmp(line, "MAIL", 4) == 0) {
            ln_say(l, "250 ok\r\n");
        } else if (strncasecmp(line, "RCPT", 4) == 0) {
            ln_say(l, mode == SM_RCPT_FAIL ? "550 5.1.1 no such user\r\n" : "250 ok\r\n");
        } else if (strncasecmp(line, "DATA", 4) == 0) {
            ln_say(l, "354 go\r\n");
            dprintf(rep, "--DATA--\n");
            for (;;) {
                if (ln_line(l, line, sizeof(line)) < 0) return;
                if (strcmp(line, ".") == 0) break;
                dprintf(rep, "%s\n", line);
            }
            dprintf(rep, "--END--\n");
            ln_say(l, mode == SM_DATA_FAIL ? "554 5.7.1 rejected as spam\r\n" : "250 queued\r\n");
        } else if (strncasecmp(line, "QUIT", 4) == 0) {
            ln_say(l, "221 bye\r\n");
            return;
        }
    }
}

/** Fork an SMTP server; *rep_rd is the read end of its transcript pipe. */
static pid_t smtp_mock(int lfd, int implicit, SmtpMode mode, int *rep_rd)
{
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid != 0) { close(lfd); close(pfd[1]); *rep_rd = pfd[0]; return pid; }
    close(pfd[0]);
    alarm(60);
    signal(SIGPIPE, SIG_IGN);
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate_file(ctx, TEST_CERT_PATH, SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ctx, TEST_KEY_PATH, SSL_FILETYPE_PEM);
    int c = accept(lfd, NULL, NULL);
    if (c >= 0) {
        Link l = { c, NULL };
        int ok = 1;
        if (implicit) {
            l.ssl = SSL_new(ctx);
            SSL_set_fd(l.ssl, c);
            ok = SSL_accept(l.ssl) == 1;
        }
        if (ok) smtp_session(&l, mode, !implicit && mode != SM_NO_STARTTLS, ctx, pfd[1]);
        close(c);
    }
    close(pfd[1]);
    _exit(0);
}

static void drain(int fd, char *out, size_t cap)
{
    size_t n = 0;
    ssize_t r;
    while (n < cap - 1 && (r = read(fd, out + n, cap - 1 - n)) > 0) n += (size_t)r;
    out[n] = '\0';
    close(fd);
}

/* What the last smtp_case() send returned as reason, and what it wrote to
 * stderr (EMAIL-112: nothing). */
static char g_smtp_why[256];
static char g_smtp_stderr[512];

/* smtp_send() with stderr captured into @p out. */
static int smtp_send_captured(const Config *cfg, const char *msg, char *why, size_t why_len,
                              char *out, size_t out_len)
{
    int cap[2];
    if (pipe(cap) != 0) return -2;
    fcntl(cap[0], F_SETFL, O_NONBLOCK);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    dup2(cap[1], STDERR_FILENO);
    int rc = smtp_send(cfg, "Alice <a@example.org>", "bob@example.org", msg, strlen(msg),
                       why, why_len);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    close(cap[1]);
    ssize_t got = read(cap[0], out, out_len - 1);
    out[got > 0 ? got : 0] = '\0';
    close(cap[0]);
    return rc;
}

static int smtp_case(SmtpMode mode, int implicit, int no_verify, const char *user,
                     const char *msg, char *transcript, size_t tcap)
{
    int port; int l = nc_listen(&port);
    int rep = -1;
    pid_t p = smtp_mock(l, implicit, mode, &rep);
    char host[96];
    snprintf(host, sizeof(host), "%s://127.0.0.1:%d", implicit ? "smtps" : "smtp", port);
    Config cfg = {0};
    cfg.smtp_host = host;
    cfg.smtp_user = (char *)user;
    cfg.smtp_pass = "p@ss";
    cfg.ssl_no_verify = no_verify;
    int rc = smtp_send_captured(&cfg, msg, g_smtp_why, sizeof(g_smtp_why),
                                g_smtp_stderr, sizeof(g_smtp_stderr));
    drain(rep, transcript, tcap);
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
    return rc;
}

static void test_smtp_implicit_tls_with_auth_plain(void)
{
    char t[4096];
    int rc = smtp_case(SM_OK, 1, 1, "user1", "Subject: hi\r\n\r\nbody\r\n", t, sizeof(t));
    ASSERT(rc == 0, "smtps send succeeds");
    ASSERT(strstr(t, "AUTH PLAIN AHVzZXIxAHBAc3M=") != NULL, "AUTH PLAIN carries \\0user\\0pass in base64");
    ASSERT(strstr(t, "MAIL FROM:<a@example.org>") != NULL, "envelope sender is the bare address");
    ASSERT(strstr(t, "RCPT TO:<bob@example.org>") != NULL, "envelope recipient");
    ASSERT(strstr(t, "--DATA--\nSubject: hi\n\nbody\n--END--") != NULL, "the message arrives intact");
}

static void test_smtp_auth_login_fallback(void)
{
    char t[4096];
    int rc = smtp_case(SM_LOGIN_ONLY, 1, 1, "user1", "Subject: x\r\n\r\nb\r\n", t, sizeof(t));
    ASSERT(rc == 0, "a server with only AUTH LOGIN is served");
    ASSERT(strstr(t, "LOGINUSER dXNlcjE=") != NULL, "user name in base64");
    ASSERT(strstr(t, "LOGINPASS cEBzcw==") != NULL, "password in base64");
}

static void test_smtp_rejections(void)
{
    char t[4096];
    ASSERT(smtp_case(SM_AUTH_FAIL, 1, 1, "user1", "Subject: x\r\n\r\nb\r\n", t, sizeof(t)) == -1,
           "a refused login fails the send");
    ASSERT(g_smtp_why[0] != '\0', "the refused login is described in the returned reason");
    ASSERT(g_smtp_stderr[0] == '\0', "and smtp_send printed nothing (EMAIL-112)");
    ASSERT(strstr(t, "--DATA--") == NULL, "and no message is sent");
    ASSERT(smtp_case(SM_RCPT_FAIL, 1, 1, "user1", "Subject: x\r\n\r\nb\r\n", t, sizeof(t)) == -1,
           "a refused recipient fails the send");
    ASSERT(g_smtp_why[0] != '\0' && g_smtp_stderr[0] == '\0',
           "the refused recipient is in the reason, nothing printed");
    ASSERT(strstr(t, "--DATA--") == NULL, "and no message is sent");
}

static void test_smtp_message_refused_after_data(void)
{
    char t[4096];
    ASSERT(smtp_case(SM_DATA_FAIL, 1, 1, "user1", "Subject: x\r\n\r\nb\r\n", t, sizeof(t)) == -1,
           "a message refused after the final dot fails the send");
    ASSERT(strstr(g_smtp_why, "did not accept the message") != NULL && strstr(g_smtp_why, "554") != NULL,
           "the reason carries the server's code");
    ASSERT(g_smtp_stderr[0] == '\0', "and nothing is printed");
}

static void test_smtp_starttls_that_never_starts(void)
{
    char t[4096];
    ASSERT(smtp_case(SM_TLS_DEAD, 0, 1, "user1", "Subject: x\r\n\r\nb\r\n", t, sizeof(t)) == -1,
           "a server that accepts STARTTLS and then speaks no TLS fails the send");
    ASSERT(g_smtp_why[0] != '\0' && strstr(t, "--DATA--") == NULL,
           "with a reason, and no message was sent in the clear");
}

static void test_smtp_starttls(void)
{
    char t[4096];
    int rc = smtp_case(SM_OK, 0, 1, "user1", "Subject: st\r\n\r\nb\r\n", t, sizeof(t));
    ASSERT(rc == 0, "smtp:// is upgraded with STARTTLS and sends");
    ASSERT(strstr(t, "STARTTLS") != NULL, "STARTTLS was issued");
    ASSERT(strstr(t, "AUTH PLAIN") != NULL, "login happens after the upgrade");
    rc = smtp_case(SM_NO_STARTTLS, 0, 1, "user1", "Subject: st\r\n\r\nb\r\n", t, sizeof(t));
    ASSERT(rc == -1, "a server without STARTTLS is refused");
    ASSERT(strstr(t, "AUTH") == NULL, "the password is never sent in the clear");
}

static void test_smtp_dot_stuffing_and_line_ends(void)
{
    char t[4096];
    int rc = smtp_case(SM_OK, 1, 1, "user1", "Subject: d\n\n.hidden\nlast line", t, sizeof(t));
    ASSERT(rc == 0, "a message with bare LF and no final newline is sent");
    ASSERT(strstr(t, "--DATA--\nSubject: d\n\n..hidden\nlast line\n--END--") != NULL,
           "line ends are CRLF, a leading dot is doubled, the end is closed");
}

static void test_smtp_verifies_the_certificate(void)
{
    char t[4096];
    int rc = smtp_case(SM_OK, 1, 0, "user1", "Subject: v\r\n\r\nb\r\n", t, sizeof(t));
    ASSERT(rc == -1, "a self-signed server certificate is refused when verifying");
    ASSERT(g_smtp_why[0] != '\0', "the TLS failure comes back as a reason");
    ASSERT(g_smtp_stderr[0] == '\0', "and smtp_send printed nothing (EMAIL-112)");
    ASSERT(t[0] == '\0', "and nothing was said to that server");
}

static void test_smtp_connect_failure_is_returned_not_printed(void)
{
    int port; int l = nc_listen(&port);
    close(l);                                   /* a port nobody listens on */
    char host[96];
    snprintf(host, sizeof(host), "smtps://127.0.0.1:%d", port);
    Config cfg = {0};
    cfg.smtp_host = host;
    cfg.smtp_user = "user1";
    cfg.smtp_pass = "p@ss";
    cfg.ssl_no_verify = 1;
    char why[256] = "", out[256];
    ASSERT(smtp_send_captured(&cfg, "Subject: c\r\n\r\nb\r\n", why, sizeof(why),
                              out, sizeof(out)) == -1, "a refused connection fails the send");
    ASSERT(why[0] != '\0', "the reason comes back through the interface");
    ASSERT(out[0] == '\0', "smtp_send printed nothing on stderr");
    ASSERT(smtp_send_captured(&cfg, "x", NULL, 0, out, sizeof(out)) == -1 && out[0] == '\0',
           "without a buffer it is still silent");

    Config bad = {0};
    bad.smtp_host = "smtp://mail.example.org";
    char why2[400] = "";
    ASSERT(smtp_send_captured(&bad, "x", why2, sizeof(why2), out, sizeof(out)) == -1,
           "smtp:// without the test flag is refused");
    ASSERT(strstr(why2, "smtps://") != NULL && out[0] == '\0',
           "the refusal names the fix and prints nothing");
}

static void test_smtp_no_credentials_skips_auth(void)
{
    char t[4096];
    int rc = smtp_case(SM_OK, 1, 1, "", "Subject: n\r\n\r\nb\r\n", t, sizeof(t));
    ASSERT(rc == 0, "an empty user name sends without login");
    ASSERT(strstr(t, "AUTH") == NULL, "no AUTH command");
}

void test_net_clients(void)
{
    signal(SIGPIPE, SIG_IGN);
    RUN_TEST(test_http_content_length);
    RUN_TEST(test_http_chunked);
    RUN_TEST(test_http_close_delimited);
    RUN_TEST(test_http_no_content_and_error_status);
    RUN_TEST(test_http_request_is_well_formed);
    RUN_TEST(test_http_failures);
    RUN_TEST(test_http_bad_urls_and_unreachable);
    RUN_TEST(test_http_url_escape);
    RUN_TEST(test_net_ca_bundle_choice);
    RUN_TEST(test_smtp_implicit_tls_with_auth_plain);
    RUN_TEST(test_smtp_auth_login_fallback);
    RUN_TEST(test_smtp_rejections);
    RUN_TEST(test_smtp_message_refused_after_data);
    RUN_TEST(test_smtp_starttls_that_never_starts);
    RUN_TEST(test_smtp_starttls);
    RUN_TEST(test_smtp_dot_stuffing_and_line_ends);
    RUN_TEST(test_smtp_verifies_the_certificate);
    RUN_TEST(test_smtp_connect_failure_is_returned_not_printed);
    RUN_TEST(test_smtp_no_credentials_skips_auth);
}
