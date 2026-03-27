#ifndef CURL_ADAPTER_H
#define CURL_ADAPTER_H

#include <curl/curl.h>

/**
 * @file curl_adapter.h
 * @brief RAII-aware wrapper for libcurl IMAP operations with TLS 1.2+ support.
 */

/** RAII cleanup functions for libcurl handles. */
static inline void curl_cleanup_ptr(CURL **p) {
    if (p && *p) {
        curl_easy_cleanup(*p);
        *p = NULL;
    }
}

static inline void curl_slist_free_all_ptr(struct curl_slist **p) {
    if (p && *p) {
        curl_slist_free_all(*p);
        *p = NULL;
    }
}

/** Automatically cleans up a CURL handle when it goes out of scope. */
#define RAII_CURL  __attribute__((cleanup(curl_cleanup_ptr)))
/** Automatically cleans up a curl_slist when it goes out of scope. */
#define RAII_SLIST __attribute__((cleanup(curl_slist_free_all_ptr)))

/**
 * @brief Initializes a CURL handle with IMAP settings and TLS enforcement.
 *
 * When the URL scheme is imaps://, TLS is required by libcurl automatically.
 * Minimum TLS version is always set to TLS 1.2.
 *
 * @param user        IMAP username.
 * @param pass        IMAP password.
 * @param verify_peer 1 = verify SSL certificate and hostname (production default).
 *                    0 = skip verification (for self-signed certs in test environments).
 * @return Pointer to a newly initialized CURL handle or NULL on failure.
 */
CURL* curl_adapter_init(const char *user, const char *pass, int verify_peer);

/**
 * @brief Executes a CURL request and returns the result code.
 * @param curl       The CURL handle to use.
 * @param url        The IMAP URL to fetch.
 * @param userdata   Data passed to the write callback.
 * @param write_func Callback invoked for each chunk of received data.
 * @return CURLcode status (CURLE_OK on success).
 */
CURLcode curl_adapter_fetch(CURL *curl, const char *url, void *userdata,
                            size_t (*write_func)(char*, size_t, size_t, void*));

#endif // CURL_ADAPTER_H
