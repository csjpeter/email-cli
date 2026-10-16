/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* A small HTTP/1.1 client on OpenSSL: the REST calls of the library. */
#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>

/**
 * @file http_client.h
 * @brief One request, one response, one connection (EMAIL-103: no libcurl).
 *
 * Supports http:// and https://, a request body, response bodies framed by
 * Content-Length, chunked transfer coding or the end of the connection.  No
 * redirects, no cookies, no proxies, no compression: the Gmail API, the OAuth
 * token endpoint and the OCSP / CRL downloads need none of these.  An https
 * connection verifies the certificate and the host name and checks
 * revocation (net_tls.h); an http one is for local test servers and the
 * revocation sources.
 */

/** One request. */
typedef struct {
    const char        *method;      /**< "GET", "POST", "DELETE", ... */
    const char        *url;         /**< http://host[:port]/path or https://... */
    const char *const *headers;     /**< "Name: value" lines, NULL terminated; may be NULL */
    const void        *body;        /**< request body; may be NULL */
    size_t             body_len;
    int                timeout_s;   /**< per read/write/connect; 0 = 30 */
    size_t             max_response;/**< 0 = 16 MiB */
} HttpRequest;

/**
 * @brief Perform @p req.
 * @param status    Receives the HTTP status code.
 * @param body_out  Receives the response body, NUL terminated; free() it.
 * @param len_out   Receives its length (without the NUL); may be NULL.
 * @param err       Receives a short reason when the call fails.
 * @return 0 when a response was read (whatever its status), -1 otherwise.
 */
int http_request(const HttpRequest *req, long *status, char **body_out,
                 size_t *len_out, char *err, size_t err_len);

/**
 * @brief Percent-encode @p s for a URL query or a form body (RFC 3986
 *        unreserved characters stay).
 * @return heap string; free() it.
 */
char *http_url_escape(const char *s);

#endif /* HTTP_CLIENT_H */
