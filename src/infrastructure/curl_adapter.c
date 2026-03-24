#include "curl_adapter.h"
#include <curl/curl.h>

CURL* curl_adapter_init(const char *user, const char *pass) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_USERNAME, user);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, pass);
    
    // Default to verbose for debugging; can be toggled later.
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    
    // Require SSL
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);

    return curl;
}

CURLcode curl_adapter_fetch(CURL *curl, const char *url, void *userdata, size_t (*write_func)(char*, size_t, size_t, void*)) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (write_func) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_func);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, userdata);
    }
    
    return curl_easy_perform(curl);
}
