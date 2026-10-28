/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* TCP and TLS connections of the library, on OpenSSL alone. */
#ifndef NET_TLS_H
#define NET_TLS_H

#include <stddef.h>
#include <sys/types.h>
#include <openssl/ssl.h>

/**
 * @file net_tls.h
 * @brief A TCP connection with optional TLS, used by the HTTP and SMTP clients
 *        (EMAIL-103: the library needs no libcurl, so the programs can be
 *        linked fully static).
 *
 * Name resolution is the C library's getaddrinfo().  The trusted CA
 * certificates are the file SSL_CERT_FILE names when that is set, otherwise
 * OpenSSL's default locations plus the well-known bundle paths of the common
 * Linux distributions (net_tls_ca_bundle()), so a binary copied to another
 * distribution finds the CA store of that system.  A verified connection
 * checks the host name and, for the leaf certificate, revocation
 * (tls_revocation.h).
 */

typedef struct NetConn NetConn;

/**
 * @brief The CA bundle file the library trusts: SSL_CERT_FILE, else the first
 *        existing well-known bundle; NULL when there is none.
 */
const char *net_tls_ca_bundle(void);

/**
 * @brief New client context: TLS 1.2 or newer, the trust store loaded and,
 *        when @p verify, peer verification with the revocation check.
 */
SSL_CTX *net_tls_ctx_new(int verify);

/**
 * @brief Connect to @p host : @p port, with TLS when @p use_tls.
 * @param verify      1 = verify the certificate and the host name.
 * @param timeout_s   Limit of the connect and of every later read or write.
 * @param err         Receives the reason on failure: the stage (resolve, connect,
 *                    handshake), HOST:PORT, the errno or OpenSSL text and the timeout.
 * @return the connection, or NULL.
 */
NetConn *net_connect(const char *host, int port, int use_tls, int verify,
                     int timeout_s, char *err, size_t err_len);

/** @brief Start TLS on a plain connection (SMTP STARTTLS). 0 / -1. */
int net_start_tls(NetConn *c, const char *host, int verify, char *err, size_t err_len);

/** @brief Read up to @p len bytes: count, 0 at end of stream, -1 on error or timeout. */
ssize_t net_conn_read(NetConn *c, void *buf, size_t len);

/** @brief Write all of @p buf: 0 / -1. */
int net_conn_write_all(NetConn *c, const void *buf, size_t len);

/**
 * @brief Why the last net_conn_read() or net_conn_write_all() failed, as
 *        "read from HOST:PORT failed: REASON" (OpenSSL error string or errno
 *        text, "timed out after N s" for a timeout); "" when none failed.
 */
const char *net_conn_error(const NetConn *c);

/** @brief Close and free; safe with NULL. */
void net_close(NetConn *c);

#endif /* NET_TLS_H */
