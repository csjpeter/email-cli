#ifndef CURL_ADAPTER_H
#define CURL_ADAPTER_H

#include <curl/curl.h>

/**
 * Initializes a CURL handle with common IMAP settings.
 */
CURL* curl_adapter_init(const char *user, const char *pass);

/**
 * Executes a CURL request and returns the response code.
 */
CURLcode curl_adapter_fetch(CURL *curl, const char *url, void *userdata, size_t (*write_func)(char*, size_t, size_t, void*));

#endif // CURL_ADAPTER_H
