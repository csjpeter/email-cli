#include "test_helpers.h"
#include "curl_adapter.h"
#include "raii.h"
#include <curl/curl.h>
#include <stdlib.h>

void test_curl_adapter(void) {
    // libcurl must be initialized for these tests
    curl_global_init(CURL_GLOBAL_ALL);

    // 1. Test Init with SSL verification enabled
    {
        RAII_CURL CURL *curl = curl_adapter_init("user", "pass", 1);
        ASSERT(curl != NULL, "curl_adapter_init should return a handle");
    }

    // 2. Test Init with SSL verification disabled (self-signed cert path)
    {
        RAII_CURL CURL *curl = curl_adapter_init("user", "pass", 0);
        ASSERT(curl != NULL, "curl_adapter_init with verify_peer=0 should return a handle");
    }

    curl_global_cleanup();
}
