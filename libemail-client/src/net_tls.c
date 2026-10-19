/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* TCP and TLS connections of the library, on OpenSSL alone. */
#include "net_tls.h"
#include "logger.h"
#include "tls_revocation.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/**
 * @file net_tls.c
 * @brief See net_tls.h.
 */

struct NetConn {
    int      fd;
    SSL_CTX *ctx;
    SSL     *ssl;
    int      timeout_s;
};

/* ── Trust store ────────────────────────────────────────────────────── */

/** CA bundles of the common Linux distributions, in the order tried. */
static const char *const k_bundles[] = {
    "/etc/ssl/certs/ca-certificates.crt",               /* Debian, Ubuntu, Alpine, Arch */
    "/etc/pki/tls/certs/ca-bundle.crt",                 /* RHEL, Rocky, Fedora, CentOS */
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",/* RHEL family, extracted */
    "/etc/ssl/ca-bundle.pem",                           /* openSUSE */
    "/etc/ssl/cert.pem",                                /* Alpine, others */
    "/var/lib/ca-certificates/ca-bundle.pem",           /* openSUSE, older */
    NULL
};

const char *net_tls_ca_bundle(void)
{
    const char *env = getenv("SSL_CERT_FILE");
    if (env && *env) return env;
    for (int i = 0; k_bundles[i]; i++)
        if (access(k_bundles[i], R_OK) == 0) return k_bundles[i];
    return NULL;
}

SSL_CTX *net_tls_ctx_new(int verify)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    if (!verify) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        return ctx;
    }
    const char *env = getenv("SSL_CERT_FILE");
    if (env && *env) {
        /* the caller names the trusted certificates: nothing else is trusted */
        if (SSL_CTX_load_verify_locations(ctx, env, NULL) != 1)
            logger_log(LOG_ERROR, "TLS: cannot load SSL_CERT_FILE %s", env);
    } else {
        SSL_CTX_set_default_verify_paths(ctx);
        const char *bundle = net_tls_ca_bundle();
        if (bundle && SSL_CTX_load_verify_locations(ctx, bundle, NULL) != 1)
            logger_log(LOG_WARN, "TLS: cannot load CA bundle %s", bundle);
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    tls_revocation_attach(ctx);
    return ctx;
}

/* ── Socket helpers ─────────────────────────────────────────────────── */

static void fcntl_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

static void set_err(char *err, size_t n, const char *msg)
{
    if (err && n) snprintf(err, n, "%s", msg);
}

static int wait_fd(int fd, short events, int timeout_s)
{
    struct pollfd p = { .fd = fd, .events = events };
    for (;;) {
        int r = poll(&p, 1, timeout_s * 1000);
        if (r < 0 && errno == EINTR) continue;      /* a signal is not a failure */
        return r;
    }
}

/** Connect with a time limit; -1 on failure (errno-free reason in @p why). */
static int tcp_connect(const char *host, int port, int timeout_s,
                       char *err, size_t err_len)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *ai = NULL;
    int rc = getaddrinfo(host, portstr, &hints, &ai);
    if (rc != 0) {
        set_err(err, err_len, "Could not resolve hostname");
        logger_log(LOG_ERROR, "getaddrinfo(%s): %s", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *a = ai; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        fcntl_nonblock(fd, 1);
        int cr = connect(fd, a->ai_addr, a->ai_addrlen);
        if (cr < 0 && (errno == EINPROGRESS || errno == EINTR)) {
            if (wait_fd(fd, POLLOUT, timeout_s) > 0) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                cr = soerr == 0 ? 0 : -1;
            }
        }
        if (cr == 0) { fcntl_nonblock(fd, 0); break; }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) set_err(err, err_len, "Could not connect to server");
    return fd;
}

/* ── TLS ────────────────────────────────────────────────────────────── */

static int host_is_ip(const char *host)
{
    unsigned char b[sizeof(struct in6_addr)];
    return inet_pton(AF_INET, host, b) == 1 || inet_pton(AF_INET6, host, b) == 1;
}

/** Block SIGPIPE in this thread; @p old receives the previous mask. */
static void sigpipe_block(sigset_t *old)
{
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &block, old);
}

/** Drop a SIGPIPE raised while blocked, then restore the mask @p old. */
static void sigpipe_unblock(const sigset_t *old)
{
    sigset_t pend, block;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    sigpending(&pend);
    if (sigismember(&pend, SIGPIPE)) {
        struct timespec ts = {0, 0};
        sigtimedwait(&block, NULL, &ts);
    }
    pthread_sigmask(SIG_SETMASK, old, NULL);
}

/** TLS handshake on c->fd; fills c->ctx and c->ssl. 0 / -1. */
static int tls_handshake(NetConn *c, const char *host, int verify,
                         char *err, size_t err_len)
{
    c->ctx = net_tls_ctx_new(verify);
    if (!c->ctx) { set_err(err, err_len, "TLS context failed"); return -1; }
    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) { set_err(err, err_len, "TLS session failed"); return -1; }
    SSL_set_fd(c->ssl, c->fd);
    if (!host_is_ip(host)) SSL_set_tlsext_host_name(c->ssl, host);   /* SNI */
    if (verify) {
        if (host_is_ip(host)) {
            if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(c->ssl), host) != 1) {
                set_err(err, err_len, "TLS host check failed");
                return -1;
            }
        } else {
            SSL_set_hostflags(c->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            if (SSL_set1_host(c->ssl, host) != 1) {
                set_err(err, err_len, "TLS host check failed");
                return -1;
            }
        }
    }
    int hs;
    sigset_t old;
    sigpipe_block(&old);        /* a server that hangs up is an error, not a signal */
    do {
        errno = 0;
        hs = SSL_connect(c->ssl);
        /* an interrupted handshake read or write is continued, not failed */
    } while (hs != 1 && errno == EINTR);
    sigpipe_unblock(&old);
    if (hs != 1) {
        long vr = SSL_get_verify_result(c->ssl);
        if (verify && vr != X509_V_OK) {
            char m[160];
            snprintf(m, sizeof(m), "server certificate rejected: %s",
                     X509_verify_cert_error_string(vr));
            set_err(err, err_len, m);
        } else {
            set_err(err, err_len, "TLS handshake failed");
        }
        logger_log(LOG_ERROR, "TLS handshake failed with %s", host);
        return -1;
    }
    logger_log(LOG_DEBUG, "TLS handshake OK with %s (%s)", host, SSL_get_version(c->ssl));
    return 0;
}

static void set_timeouts(int fd, int timeout_s)
{
    struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

NetConn *net_connect(const char *host, int port, int use_tls, int verify,
                     int timeout_s, char *err, size_t err_len)
{
    if (err && err_len) err[0] = '\0';
    if (!host || !*host || port <= 0 || port > 65535) {
        set_err(err, err_len, "Bad host or port");
        return NULL;
    }
    if (timeout_s <= 0) timeout_s = 30;
    int fd = tcp_connect(host, port, timeout_s, err, err_len);
    if (fd < 0) return NULL;
    set_timeouts(fd, timeout_s);
    NetConn *c = calloc(1, sizeof(*c));
    if (!c) { set_err(err, err_len, "Out of memory"); close(fd); return NULL; }
    c->fd = fd;
    c->timeout_s = timeout_s;
    if (use_tls && tls_handshake(c, host, verify, err, err_len) != 0) {
        net_close(c);
        return NULL;
    }
    return c;
}

int net_start_tls(NetConn *c, const char *host, int verify, char *err, size_t err_len)
{
    if (!c || c->ssl) return -1;
    if (tls_handshake(c, host, verify, err, err_len) != 0) return -1;
    return 0;
}

/* ── I/O ────────────────────────────────────────────────────────────── */

ssize_t net_conn_read(NetConn *c, void *buf, size_t len)
{
    if (!c || !buf || len == 0) return -1;
    for (;;) {
        if (c->ssl) {
            errno = 0;
            int n = SSL_read(c->ssl, buf, (int)(len > 1 << 30 ? 1 << 30 : len));
            if (n > 0) return n;
            int e = SSL_get_error(c->ssl, n);
            if (e == SSL_ERROR_ZERO_RETURN) return 0;
            if (e == SSL_ERROR_SYSCALL && n == 0 && errno == 0) return 0;   /* EOF without close_notify */
            if (e == SSL_ERROR_SYSCALL && errno == EINTR) continue;
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                if (wait_fd(c->fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT,
                            c->timeout_s) > 0) continue;
            }
            return -1;
        }
        ssize_t n = read(c->fd, buf, len);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        return -1;
    }
}

/** Write all of @p p with SIGPIPE blocked, so a closed peer is an error return. */
static int write_nosigpipe(NetConn *c, const unsigned char *p, size_t len)
{
    sigset_t old;
    sigpipe_block(&old);
    int rc = 0;
    while (len > 0) {
        if (c->ssl) {
            errno = 0;
            int n = SSL_write(c->ssl, p, (int)(len > 1 << 30 ? 1 << 30 : len));
            if (n > 0) { p += n; len -= (size_t)n; continue; }
            int e = SSL_get_error(c->ssl, n);
            if (e == SSL_ERROR_SYSCALL && errno == EINTR) continue;
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                if (wait_fd(c->fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT,
                            c->timeout_s) > 0) continue;
            }
            rc = -1;
            break;
        } else {
            ssize_t n = send(c->fd, p, len, MSG_NOSIGNAL);
            if (n > 0) { p += n; len -= (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            rc = -1;
            break;
        }
    }
    sigpipe_unblock(&old);
    return rc;
}

int net_conn_write_all(NetConn *c, const void *buf, size_t len)
{
    if (!c || (!buf && len)) return -1;
    return write_nosigpipe(c, buf, len);
}

void net_close(NetConn *c)
{
    if (!c) return;
    if (c->ssl) {
        /* the close_notify goes to a peer that may be gone: that is not a
         * reason to die of SIGPIPE */
        sigset_t old;
        sigpipe_block(&old);
        SSL_shutdown(c->ssl);
        sigpipe_unblock(&old);
        SSL_free(c->ssl);
    }
    if (c->ctx) SSL_CTX_free(c->ctx);
    if (c->fd >= 0) close(c->fd);
    free(c);
}
