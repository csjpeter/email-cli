#include "curl_adapter.h"
#include "logger.h"
#include <curl/curl.h>
#include <string.h>

/**
 * @brief CURL debug callback — forwards IMAP traffic to the logger at DEBUG level.
 */
static int curl_debug_cb(CURL *handle, curl_infotype type,
                         char *data, size_t size, void *userp) {
    (void)handle; (void)userp;
    if (type != CURLINFO_HEADER_IN && type != CURLINFO_HEADER_OUT &&
        type != CURLINFO_DATA_IN   && type != CURLINFO_DATA_OUT)
        return 0;

    /* Trim trailing whitespace for clean log lines. */
    char buf[4096];
    size_t n = size < sizeof(buf) - 1 ? size : sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    if (n > 0)
        logger_log(LOG_DEBUG, "IMAP [%s] %s",
                   (type == CURLINFO_HEADER_OUT || type == CURLINFO_DATA_OUT) ? "OUT" : " IN",
                   buf);
    return 0;
}

CURL* curl_adapter_init(const char *user, const char *pass, int verify_peer) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_USERNAME, user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, pass);

    /* Upgrade to TLS if the server supports it (imaps:// forces it unconditionally). */
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_TRY);

    /* Require at least TLS 1.2 whenever TLS is negotiated. */
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    if (!verify_peer) {
        /* Allow self-signed certificates – test environments only. */
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    /* Forward IMAP traffic to the logger at DEBUG level. */
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    return curl;
}

CURLcode curl_adapter_fetch(CURL *curl, const char *url, void *userdata,
                            size_t (*write_func)(char*, size_t, size_t, void*)) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (write_func) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_func);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, userdata);
    }

    return curl_easy_perform(curl);
}
