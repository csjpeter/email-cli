#include "test_helpers.h"
#include "curl_adapter.h"
#include <curl/curl.h>
#include <stdlib.h>

void test_curl_adapter(void) {
    // libcurl must be initialized for these tests
    curl_global_init(CURL_GLOBAL_ALL);

    // 1. Test Init
    CURL *curl = curl_adapter_init("user", "pass");
    ASSERT(curl != NULL, "curl_adapter_init should return a handle");
    
    // We don't perform a fetch here because it needs a network/mock server,
    // which is covered in functional tests.
    
    curl_easy_cleanup(curl);
    curl_global_cleanup();
}
