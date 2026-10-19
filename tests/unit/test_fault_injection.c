/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-19 */
/* Fault injection tests of the network and message code (EMAIL-139). */
#include "test_helpers.h"
#include "fault_alloc.h"
#include "net_tls.h"
#include "http_client.h"
#include "json_util.h"
#include "compose_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * @file test_fault_injection.c
 * @brief The failure branches that a healthy peer never reaches, driven on
 *        purpose: a peer that resets, stalls, speaks garbage or says nothing
 *        (net_tls.c), a signal that interrupts a blocking call (EINTR), an
 *        attachment that cannot be read (compose_service.c), and a failing
 *        allocator (fault_alloc.h) swept over every allocation point of the
 *        HTTP client, the connection set-up and the message builder.
 */

/* ── Plumbing ───────────────────────────────────────────────────────── */

static int fi_listen(int *port)
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

static void fi_stop(pid_t pid)
{
    if (pid <= 0) return;
    kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);
}

static void fi_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── The misbehaving peer ───────────────────────────────────────────── */

typedef enum {
    PEER_RESET,     /* completes the handshake, then resets the connection */
    PEER_SLOW,      /* completes the handshake, answers "late" after 400 ms */
    PEER_SILENT,    /* completes the handshake, then says nothing */
    PEER_GARBAGE,   /* is not a TLS server: answers with text and closes */
    PEER_CLOSE      /* accepts and closes at once */
} PeerMode;

static void peer_reset_close(int fd)
{
    struct linger lg = { 1, 0 };            /* close() sends RST, not FIN */
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(fd);
}

/** Fork one peer on @p lfd; it serves one connection and exits. */
static pid_t peer_start(int lfd, int tls, PeerMode mode)
{
    pid_t pid = fork();
    if (pid != 0) return pid;
    alarm(30);
    signal(SIGPIPE, SIG_IGN);
    int c = accept(lfd, NULL, NULL);
    if (c < 0) _exit(1);
    if (mode == PEER_CLOSE) { close(c); _exit(0); }
    if (mode == PEER_GARBAGE) {
        const char *g = "this is not TLS\r\n";
        ssize_t w = write(c, g, strlen(g)); (void)w;
        fi_sleep_ms(200);
        close(c);
        _exit(0);
    }
    SSL *ssl = NULL;
    if (tls) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
        SSL_CTX_use_certificate_file(ctx, TEST_CERT_PATH, SSL_FILETYPE_PEM);
        SSL_CTX_use_PrivateKey_file(ctx, TEST_KEY_PATH, SSL_FILETYPE_PEM);
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, c);
        if (SSL_accept(ssl) != 1) _exit(2);
    }
    if (mode == PEER_RESET) {
        fi_sleep_ms(100);                   /* the client is past its handshake */
        peer_reset_close(c);
        _exit(0);
    }
    if (mode == PEER_SLOW) {
        fi_sleep_ms(400);
        if (ssl) SSL_write(ssl, "late", 4);
        else { ssize_t w = write(c, "late", 4); (void)w; }
        fi_sleep_ms(300);
        close(c);
        _exit(0);
    }
    fi_sleep_ms(5000);                      /* PEER_SILENT */
    close(c);
    _exit(0);
}

static NetConn *peer_connect(int port, int tls, int timeout_s, char *err, size_t n)
{
    return net_connect("127.0.0.1", port, tls, 0, timeout_s, err, n);
}

/* ── net_tls: reads and writes that fail ────────────────────────────── */

static void test_net_bad_host_and_port(void)
{
    char err[128] = "";
    ASSERT(net_connect(NULL, 993, 0, 0, 5, err, sizeof(err)) == NULL, "null host refused");
    ASSERT(strstr(err, "Bad host or port") != NULL, "null host: the reason is given");
    ASSERT(net_connect("127.0.0.1", 0, 0, 0, 5, err, sizeof(err)) == NULL, "port 0 refused");
    ASSERT(net_connect("127.0.0.1", 70000, 0, 0, 5, err, sizeof(err)) == NULL,
           "port above 65535 refused");
    ASSERT(net_connect("", 993, 0, 0, 5, NULL, 0) == NULL, "empty host refused without a buffer");
}

static void test_net_unreadable_ca_file_is_logged_not_fatal(void)
{
    char *saved = getenv("SSL_CERT_FILE") ? strdup(getenv("SSL_CERT_FILE")) : NULL;
    setenv("SSL_CERT_FILE", "/nonexistent/ca-bundle.crt", 1);
    SSL_CTX *ctx = net_tls_ctx_new(1);
    ASSERT(ctx != NULL, "an unreadable SSL_CERT_FILE still gives a context");
    ASSERT(SSL_CTX_get_verify_mode(ctx) == SSL_VERIFY_PEER,
           "and the context still verifies (it trusts nothing)");
    SSL_CTX_free(ctx);
    if (saved) setenv("SSL_CERT_FILE", saved, 1);
    else unsetenv("SSL_CERT_FILE");
    free(saved);
}

static void test_net_tls_handshake_with_a_non_tls_peer(void)
{
    int port; int l = fi_listen(&port);
    pid_t p = peer_start(l, 0, PEER_GARBAGE);
    close(l);
    char err[160] = "";
    NetConn *c = peer_connect(port, 1, 5, err, sizeof(err));
    ASSERT(c == NULL, "a peer that does not speak TLS: no connection");
    ASSERT(strstr(err, "TLS handshake failed") != NULL, "the handshake failure is named");
    fi_stop(p);
}

static void test_net_tls_peer_closing_during_handshake(void)
{
    int port; int l = fi_listen(&port);
    pid_t p = peer_start(l, 0, PEER_CLOSE);
    close(l);
    char err[160] = "";
    ASSERT(peer_connect(port, 1, 5, err, sizeof(err)) == NULL, "closed during the handshake");
    ASSERT(err[0] != '\0', "a reason is given");
    fi_stop(p);
}

static void test_net_tls_read_after_reset(void)
{
    int port; int l = fi_listen(&port);
    pid_t p = peer_start(l, 1, PEER_RESET);
    close(l);
    char err[160] = "";
    NetConn *c = peer_connect(port, 1, 5, err, sizeof(err));
    ASSERT(c != NULL, "connected to the resetting peer");
    char buf[64];
    ssize_t n = net_conn_read(c, buf, sizeof(buf));
    ASSERT(n < 0, "a reset connection is a read error, not end of file");
    net_close(c);
    fi_stop(p);
}

static void test_net_tls_write_after_reset(void)
{
    int port; int l = fi_listen(&port);
    pid_t p = peer_start(l, 1, PEER_RESET);
    close(l);
    NetConn *c = peer_connect(port, 1, 5, NULL, 0);
    ASSERT(c != NULL, "connected to the resetting peer");
    fi_sleep_ms(400);                       /* the RST has arrived */
    static unsigned char big[4 << 20];
    memset(big, 'x', sizeof(big));
    ASSERT(net_conn_write_all(c, big, sizeof(big)) != 0,
           "writing to a reset connection fails and does not kill the process");
    /* SIGPIPE was blocked and swallowed: it is not left pending */
    sigset_t pend;
    sigpending(&pend);
    ASSERT(!sigismember(&pend, SIGPIPE), "no SIGPIPE is left pending");
    net_close(c);
    fi_stop(p);
}

static void test_net_plain_write_after_reset(void)
{
    int port; int l = fi_listen(&port);
    pid_t p = peer_start(l, 0, PEER_RESET);
    close(l);
    NetConn *c = peer_connect(port, 0, 5, NULL, 0);
    ASSERT(c != NULL, "plain connection made");
    fi_sleep_ms(400);
    static unsigned char big[4 << 20];
    memset(big, 'y', sizeof(big));
    ASSERT(net_conn_write_all(c, big, sizeof(big)) != 0, "plain write to a reset peer fails");
    char b[8];
    ASSERT(net_conn_read(c, b, sizeof(b)) <= 0, "plain read after a reset yields no data");
    net_close(c);
    fi_stop(p);
}

static void test_net_tls_read_times_out_on_a_silent_peer(void)
{
    int port; int l = fi_listen(&port);
    pid_t p = peer_start(l, 1, PEER_SILENT);
    close(l);
    NetConn *c = peer_connect(port, 1, 1, NULL, 0);
    ASSERT(c != NULL, "connected to the silent peer");
    char buf[16];
    time_t t0 = time(NULL);
    ASSERT(net_conn_read(c, buf, sizeof(buf)) < 0, "a silent peer times out the read");
    ASSERT(time(NULL) - t0 < 4, "and does so after the timeout, not never");
    net_close(c);
    fi_stop(p);
}

static volatile sig_atomic_t g_ticks;
static void on_tick(int sig) { (void)sig; g_ticks++; }

/** Signals every 40 ms, without SA_RESTART, so blocking calls return EINTR. */
static void ticks_on(struct sigaction *old)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_tick;
    sigemptyset(&sa.sa_mask);
    g_ticks = 0;
    sigaction(SIGALRM, &sa, old);
    struct itimerval it = { {0, 40000}, {0, 40000} };
    setitimer(ITIMER_REAL, &it, NULL);
}

static void ticks_off(const struct sigaction *old)
{
    struct itimerval off = { {0, 0}, {0, 0} };
    setitimer(ITIMER_REAL, &off, NULL);
    sigaction(SIGALRM, old, NULL);
}

static void interrupted_read_case(int tls)
{
    int port; int l = fi_listen(&port);
    pid_t p = peer_start(l, tls, PEER_SLOW);
    close(l);
    NetConn *c = peer_connect(port, tls, 5, NULL, 0);
    ASSERT(c != NULL, "connected to the slow peer");
    struct sigaction old;
    ticks_on(&old);
    char buf[16] = "";
    ssize_t n = net_conn_read(c, buf, sizeof(buf));
    ticks_off(&old);
    ASSERT(g_ticks > 0, "signals did arrive while the read was blocked");
    ASSERT(n == 4 && memcmp(buf, "late", 4) == 0,
           "an interrupted read is continued, not failed");
    net_close(c);
    fi_stop(p);
}

static void test_net_tls_read_survives_signals(void) { interrupted_read_case(1); }
static void test_net_plain_read_survives_signals(void) { interrupted_read_case(0); }

static void test_net_tls_handshake_survives_signals(void)
{
    int port; int l = fi_listen(&port);
    pid_t p = peer_start(l, 1, PEER_SILENT);
    close(l);
    struct sigaction old;
    ticks_on(&old);
    NetConn *c = peer_connect(port, 1, 5, NULL, 0);
    ticks_off(&old);
    ASSERT(c != NULL, "the handshake completes although signals interrupt it");
    net_close(c);
    fi_stop(p);
}

/* ── Fault injection: an accepting peer that answers HTTP ───────────── */

/** Serves connections forever: reads the head, answers "ok", closes. */
static pid_t http_loop_start(int lfd)
{
    pid_t pid = fork();
    if (pid != 0) return pid;
    alarm(120);
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) _exit(1);
        char buf[4096];
        size_t have = 0;
        struct timeval tv = {1, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (have < sizeof(buf) - 1) {
            ssize_t n = read(c, buf + have, sizeof(buf) - 1 - have);
            if (n <= 0) break;
            have += (size_t)n;
            buf[have] = '\0';
            if (strstr(buf, "\r\n\r\n")) break;
        }
        struct pollfd pf = { .fd = c, .events = POLLIN };
        while (poll(&pf, 1, 150) > 0) {         /* take the request body too */
            char junk[4096];
            if (read(c, junk, sizeof(junk)) <= 0) break;
        }
        const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                        "Connection: close\r\n\r\nok";
        ssize_t w = write(c, r, strlen(r)); (void)w;
        close(c);
    }
}

static void test_http_every_allocation_failing_in_turn(void)
{
    int port; int l = fi_listen(&port);
    pid_t srv = http_loop_start(l);
    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/path?q=1", port);
    const char *hdrs[] = { "X-Test: 1", NULL };
    int swept = 0;
    for (long k = 1; k < 400; k++) {
        HttpRequest rq = { .method = "POST", .url = url, .headers = hdrs,
                           .body = "a=b", .body_len = 3, .timeout_s = 5 };
        long st = 0; char *body = NULL; size_t len = 0; char err[128] = "";
        fault_alloc_arm(k);
        int rc = http_request(&rq, &st, &body, &len, err, sizeof(err));
        int fired = fault_alloc_disarm();
        if (rc == 0) {
            ASSERT(body != NULL && st == 200, "a response that was read is whole");
            if (!fired) ASSERT(strcmp(body, "ok") == 0 && len == 2, "the body is intact");
        } else {
            ASSERT(body == NULL, "a failed request returns no body");
            ASSERT(fired, "only an injected failure makes this request fail");
            ASSERT(err[0] != '\0', "and says why");
        }
        free(body);
        if (!fired) { swept = 1; break; }
    }
    ASSERT(swept, "the sweep reached the end of the allocations");

    char *esc = NULL;
    for (long k = 1; k < 20; k++) {
        fault_alloc_arm(k);
        esc = http_url_escape("a b&c/d");
        int fired = fault_alloc_disarm();
        if (!fired) { ASSERT(esc && strcmp(esc, "a%20b%26c%2Fd") == 0, "escape intact"); free(esc); break; }
        ASSERT(esc == NULL, "a failed escape returns NULL");
    }
    close(l);
    fi_stop(srv);
}

static void test_net_connect_every_allocation_failing_in_turn(void)
{
    int port; int l = fi_listen(&port);
    pid_t srv = http_loop_start(l);
    int swept = 0;
    for (long k = 1; k < 100; k++) {
        char err[64] = "";
        fault_alloc_arm(k);
        NetConn *c = net_connect("127.0.0.1", port, 0, 0, 5, err, sizeof(err));
        int fired = fault_alloc_disarm();
        if (c) net_close(c);
        if (!fired) { ASSERT(c != NULL, "an unharmed connect succeeds"); swept = 1; break; }
    }
    ASSERT(swept, "the connect sweep reached the end of the allocations");
    close(l);
    fi_stop(srv);
}

static void test_http_ipv6_url_forms(void)
{
    /* nothing listens: the point is that every form is parsed, and refused or
     * connected-to without a crash */
    const char *bad[] = {
        "http://[::1/x",            /* no closing bracket */
        "http://[]:80/x",           /* empty host */
        "http://[::1]junk/x",       /* text after the bracket */
        "ftp://example.org/x",      /* not http */
        "http://:80/x",             /* no host */
        "http://host:0/x",          /* port zero */
        "http://host:99999/x",      /* port too large */
        "http://host:80x/x",        /* port with text */
        NULL
    };
    for (int i = 0; bad[i]; i++) {
        HttpRequest rq = { .method = "GET", .url = bad[i], .timeout_s = 2 };
        long st; char *body = NULL; char err[96] = "";
        ASSERT(http_request(&rq, &st, &body, NULL, err, sizeof(err)) != 0, "a bad URL is refused");
        ASSERT(body == NULL, "no body for a bad URL");
    }
    HttpRequest ok = { .method = "GET", .url = "http://[::1]:1/", .timeout_s = 2 };
    long st; char *body = NULL; char err[96] = "";
    ASSERT(http_request(&ok, &st, &body, NULL, err, sizeof(err)) != 0,
           "a well-formed IPv6 URL with nobody listening fails to connect");
    ASSERT(err[0] != '\0', "with a reason");
}

static void test_http_url_without_a_path(void)
{
    int port; int l = fi_listen(&port);
    pid_t srv = http_loop_start(l);
    const char *tail[] = { "", "?x=1", "#frag", "/p#frag", NULL };
    for (int i = 0; tail[i]; i++) {
        char url[96];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", port, tail[i]);
        HttpRequest rq = { .method = "GET", .url = url, .timeout_s = 5 };
        long st = 0; char *body = NULL; char err[96] = "";
        ASSERT(http_request(&rq, &st, &body, NULL, err, sizeof(err)) == 0, "the request goes out");
        ASSERT(st == 200 && body && strcmp(body, "ok") == 0, "and is answered");
        free(body);
    }
    close(l);
    fi_stop(srv);
}

static void test_http_large_post_every_allocation_failing_in_turn(void)
{
    int port; int l = fi_listen(&port);
    pid_t srv = http_loop_start(l);
    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/up", port);
    static char big[6000];
    memset(big, 'z', sizeof(big));
    int swept = 0;
    for (long k = 1; k < 100; k++) {
        HttpRequest rq = { .method = "POST", .url = url, .body = big,
                           .body_len = sizeof(big), .timeout_s = 5 };
        long st = 0; char *body = NULL; char err[96] = "";
        fault_alloc_arm(k);
        int rc = http_request(&rq, &st, &body, NULL, err, sizeof(err));
        int fired = fault_alloc_disarm();
        if (rc != 0) {
            ASSERT(fired && body == NULL && err[0] != '\0', "a failure is injected, reported and bodiless");
        } else {
            ASSERT(st == 200 && body, "an answered request is whole");
        }
        free(body);
        if (!fired) { swept = 1; break; }
    }
    ASSERT(swept, "the sweep reached the end of the allocations");
    close(l);
    fi_stop(srv);
}

static void test_http_peers_that_hang_up(void)
{
    PeerMode modes[] = { PEER_CLOSE, PEER_RESET };
    for (int i = 0; i < 2; i++) {
        int port; int l = fi_listen(&port);
        pid_t p = peer_start(l, 0, modes[i]);
        close(l);
        char url[96];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", port);
        static char big[1 << 20];
        HttpRequest rq = { .method = "POST", .url = url, .body = big,
                           .body_len = sizeof(big), .timeout_s = 5 };
        long st = 0; char *body = NULL; char err[96] = "";
        ASSERT(http_request(&rq, &st, &body, NULL, err, sizeof(err)) != 0,
               "a peer that hangs up fails the request");
        ASSERT(body == NULL && err[0] != '\0', "without a body, with a reason");
        fi_stop(p);
    }
}

static void test_json_array_failures_and_allocation_failures(void)
{
    const char *bad[] = {
        "{\"k\":[1,2]}",            /* not strings */
        "{\"k\":[\"abc",            /* unterminated string */
        "{\"k\":[\"a\\",            /* ends inside an escape */
        "{\"k\":[\"a\" \"b\"]}",    /* no separator is tolerated or refused, never a crash */
        NULL
    };
    for (int i = 0; bad[i]; i++) {
        char **arr = NULL; int n = -1;
        int rc = json_get_string_array(bad[i], "k", &arr, &n);
        if (rc != 0) ASSERT(arr == NULL && n == 0, "a refused array leaves nothing behind");
        for (int j = 0; j < n && rc == 0; j++) free(arr[j]);
        free(arr);
    }
    const char *twelve = "{\"k\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\"12\"]}";
    int swept = 0;
    for (long k = 1; k < 60; k++) {
        char **arr = NULL; int n = 0;
        fault_alloc_arm(k);
        int rc = json_get_string_array(twelve, "k", &arr, &n);
        int fired = fault_alloc_disarm();
        if (rc == 0) {
            ASSERT(n == 12 && strcmp(arr[11], "12") == 0, "a parsed array is whole");
            for (int j = 0; j < n; j++) free(arr[j]);
            free(arr);
        } else {
            ASSERT(fired && arr == NULL && n == 0, "an allocation failure fails cleanly");
        }
        if (!fired) { swept = 1; break; }
    }
    ASSERT(swept, "the sweep reached the end of the allocations");
}

/* ── compose_service ────────────────────────────────────────────────── */

static char *fi_attachment(const char *content, size_t reps)
{
    char *path = NULL;
    if (asprintf(&path, "%s/fi-attach-XXXXXX", test_scratch_dir()) == -1) return NULL;
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    for (size_t i = 0; i < reps; i++) {
        ssize_t w = write(fd, content, strlen(content)); (void)w;
    }
    close(fd);
    return path;
}

static void test_compose_unreadable_attachment(void)
{
    char *path = fi_attachment("secret", 1);
    ASSERT(path != NULL, "attachment made");
    ASSERT(chmod(path, 0) == 0, "attachment made unreadable");
    const char *atts[] = { path, NULL };
    ComposeParams p = { .from = "a@example.org", .to = "b@example.org", .subject = "s",
                        .body = "b", .attachments = atts, .attach_count = 1 };
    char *out = NULL; size_t len = 0;
    int rc = compose_build_message(&p, &out, &len);
    chmod(path, 0600);
    unlink(path);
    free(path);
    ASSERT(rc == -1, "an attachment that cannot be opened fails the message");
    ASSERT(out == NULL, "and leaves no half-built message");
}

static void test_compose_every_allocation_failing_in_turn(void)
{
    char *path = fi_attachment("0123456789abcdef", 20);     /* 320 bytes: several base64 lines */
    ASSERT(path != NULL, "attachment made");
    const char *atts[] = { path, path, NULL };
    int swept = 0;
    for (long k = 1; k < 100; k++) {
        ComposeParams p = { .from = "a@example.org", .to = "b@example.org",
                            .cc = "c@example.org", .bcc = "d@example.org",
                            .subject = "s", .body = "line1\nline2\n",
                            .reply_to_msg_id = "<id@example.org>",
                            .attachments = atts, .attach_count = 2 };
        char *out = NULL; size_t len = 0;
        fault_alloc_arm(k);
        int rc = compose_build_message(&p, &out, &len);
        int fired = fault_alloc_disarm();
        if (rc == 0) {
            ASSERT(out != NULL && len > 320, "a built message is whole");
            ASSERT(strstr(out, "multipart/mixed") != NULL, "and multipart");
        } else {
            ASSERT(out == NULL, "a failed build leaves no message");
            ASSERT(fired, "only an injected failure fails this build");
        }
        free(out);
        if (!fired) { swept = 1; break; }
    }
    ASSERT(swept, "the build sweep reached the end of the allocations");

    swept = 0;
    for (long k = 1; k < 20; k++) {
        ComposeParams p = { .from = "a@example.org", .to = "b@example.org",
                            .subject = "s", .body = "x\ny\n" };
        char *out = NULL; size_t len = 0;
        fault_alloc_arm(k);
        int rc = compose_build_message(&p, &out, &len);
        int fired = fault_alloc_disarm();
        ASSERT((rc == 0) == (out != NULL), "plain build: result and output agree");
        free(out);
        if (!fired) { swept = 1; break; }
    }
    ASSERT(swept, "the plain build sweep reached the end of the allocations");
    unlink(path);
    free(path);
}

static void test_compose_reply_subject_without_space(void)
{
    char *to = NULL, *subj = NULL, *id = NULL;
    const char *raw = "From: a@example.org\r\nSubject: Re:Re:  hello\r\n"
                      "Message-ID: <1@example.org>\r\n\r\nbody\r\n";
    ASSERT(compose_extract_reply_meta(raw, &to, &subj, &id) == 0, "meta extracted");
    ASSERT(subj && strcmp(subj, "Re: hello") == 0,
           "\"Re:\" prefixes with and without a space are all stripped");
    free(to); free(subj); free(id);
}

void test_fault_injection(void)
{
    RUN_TEST(test_net_bad_host_and_port);
    RUN_TEST(test_net_unreadable_ca_file_is_logged_not_fatal);
    RUN_TEST(test_net_tls_handshake_with_a_non_tls_peer);
    RUN_TEST(test_net_tls_peer_closing_during_handshake);
    RUN_TEST(test_net_tls_read_after_reset);
    RUN_TEST(test_net_tls_write_after_reset);
    RUN_TEST(test_net_plain_write_after_reset);
    RUN_TEST(test_net_tls_read_times_out_on_a_silent_peer);
    RUN_TEST(test_net_tls_read_survives_signals);
    RUN_TEST(test_net_plain_read_survives_signals);
    RUN_TEST(test_net_tls_handshake_survives_signals);
    RUN_TEST(test_http_every_allocation_failing_in_turn);
    RUN_TEST(test_net_connect_every_allocation_failing_in_turn);
    RUN_TEST(test_http_ipv6_url_forms);
    RUN_TEST(test_http_url_without_a_path);
    RUN_TEST(test_http_large_post_every_allocation_failing_in_turn);
    RUN_TEST(test_http_peers_that_hang_up);
    RUN_TEST(test_json_array_failures_and_allocation_failures);
    RUN_TEST(test_compose_unreadable_attachment);
    RUN_TEST(test_compose_every_allocation_failing_in_turn);
    RUN_TEST(test_compose_reply_subject_without_space);
}
