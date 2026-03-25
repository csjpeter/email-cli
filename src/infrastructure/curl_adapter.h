#ifndef CURL_ADAPTER_H
#define CURL_ADAPTER_H

#include <curl/curl.h>

/**
 * @file curl_adapter.h
 * @brief RAII-aware wrapper for libcurl IMAP operations.
 */

/**
 * @brief Initializes a CURL handle with common IMAP settings.
 * @param user IMAP username.
 * @param pass IMAP password.
 * @return Pointer to a newly initialized CURL handle or NULL.
 */
CURL* curl_adapter_init(const char *user, const char *pass);

/**
 * @brief Executes a CURL request and returns the response code.
 * @param curl The CURL handle to use.
 * @param url The IMAP URL to fetch.
 * @param userdata Data to pass to the write function.
 * @param write_func Callback function for processing received data.
 * @return CURLcode status (CURLE_OK on success).
 */
CURLcode curl_adapter_fetch(CURL *curl, const char *url, void *userdata, size_t (*write_func)(char*, size_t, size_t, void*));

#endif // CURL_ADAPTER_H
