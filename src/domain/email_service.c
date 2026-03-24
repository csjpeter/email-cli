#include "email_service.h"
#include "curl_adapter.h"
#include "raii.h"
#include <stdio.h>
#include <string.h>

/**
 * Standard callback for printing to stdout.
 */
static size_t write_to_stdout(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)userdata;
    size_t total = size * nmemb;
    fwrite(ptr, size, nmemb, stdout);
    return total;
}

int email_service_fetch_recent(const Config *cfg) {
    RAII_CURL CURL *curl = curl_adapter_init(cfg->user, cfg->pass);
    if (!curl) return -1;

    printf("--- Fetching recent emails from %s/%s ---\n", cfg->host, cfg->folder);

    // In a full implementation, we would:
    // 1. Fetch UIDs from the folder.
    // 2. Select the N most recent.
    // 3. Fetch each one.
    
    // For this prototype, we'll try to fetch the most recent message (UID *) or similar.
    // Note: libcurl IMAP URL format for latest is complex.
    // Simplest test: fetch message with UID 1 (if it exists).
    
    RAII_STRING char *url = NULL;
    asprintf(&url, "%s/%s/;UID=1", cfg->host, cfg->folder);
    
    CURLcode res = curl_adapter_fetch(curl, url, NULL, write_to_stdout);
    if (res != CURLE_OK) {
        fprintf(stderr, "CURL error: %s\n", curl_easy_strerror(res));
        return -1;
    }

    return 0;
}
