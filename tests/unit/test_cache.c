#include "test_helpers.h"
#include "cache_store.h"
#include "fs_util.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

void test_cache_store(void) {
    char *old_home = getenv("HOME");
    setenv("HOME", "/tmp/email-cli-cache-test-home", 1);

    const char *folder = "INBOX";
    const int   uid    = 42;

    /* 1. Not cached initially */
    ASSERT(cache_exists(folder, uid) == 0, "cache_exists: should be 0 before save");

    char *loaded = cache_load(folder, uid);
    ASSERT(loaded == NULL, "cache_load: should return NULL before save");

    /* 2. Save and verify existence */
    const char *content = "Subject: Cache Test\r\n\r\nHello cache!";
    int rc = cache_save(folder, uid, content, strlen(content));
    ASSERT(rc == 0, "cache_save: should return 0 on success");
    ASSERT(cache_exists(folder, uid) == 1, "cache_exists: should be 1 after save");

    /* 3. Load and verify content */
    loaded = cache_load(folder, uid);
    ASSERT(loaded != NULL, "cache_load: should not be NULL after save");
    ASSERT(strcmp(loaded, content) == 0, "cache_load: content mismatch");
    free(loaded);

    /* 4. Overwrite with new content */
    const char *content2 = "Subject: Updated\r\n\r\nNew body.";
    rc = cache_save(folder, uid, content2, strlen(content2));
    ASSERT(rc == 0, "cache_save: overwrite should return 0");
    loaded = cache_load(folder, uid);
    ASSERT(loaded != NULL, "cache_load: should not be NULL after overwrite");
    ASSERT(strcmp(loaded, content2) == 0, "cache_load: overwritten content mismatch");
    free(loaded);

    /* 5. Different UIDs are independent */
    ASSERT(cache_exists(folder, 99) == 0, "cache_exists: UID 99 should not exist");

    /* Cleanup */
    unlink("/tmp/email-cli-cache-test-home/.cache/email-cli/messages/INBOX/42.eml");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}
