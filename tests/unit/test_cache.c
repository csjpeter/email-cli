#include "test_helpers.h"
#include "cache_store.h"
#include "fs_util.h"
#include "raii.h"
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

    {
        RAII_STRING char *loaded = cache_load(folder, uid);
        ASSERT(loaded == NULL, "cache_load: should return NULL before save");
    }

    /* 2. Save and verify existence */
    const char *content = "Subject: Cache Test\r\n\r\nHello cache!";
    int rc = cache_save(folder, uid, content, strlen(content));
    ASSERT(rc == 0, "cache_save: should return 0 on success");
    ASSERT(cache_exists(folder, uid) == 1, "cache_exists: should be 1 after save");

    /* 3. Load and verify content */
    {
        RAII_STRING char *loaded = cache_load(folder, uid);
        ASSERT(loaded != NULL, "cache_load: should not be NULL after save");
        ASSERT(strcmp(loaded, content) == 0, "cache_load: content mismatch");
    }

    /* 4. Overwrite with new content */
    const char *content2 = "Subject: Updated\r\n\r\nNew body.";
    rc = cache_save(folder, uid, content2, strlen(content2));
    ASSERT(rc == 0, "cache_save: overwrite should return 0");
    {
        RAII_STRING char *loaded = cache_load(folder, uid);
        ASSERT(loaded != NULL, "cache_load: should not be NULL after overwrite");
        ASSERT(strcmp(loaded, content2) == 0, "cache_load: overwritten content mismatch");
    }

    /* 5. Different UIDs are independent */
    ASSERT(cache_exists(folder, 99) == 0, "cache_exists: UID 99 should not exist");

    /* Cleanup */
    unlink("/tmp/email-cli-cache-test-home/.cache/email-cli/messages/INBOX/42.eml");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

void test_hcache_evict(void) {
    char *old_home = getenv("HOME");
    setenv("HOME", "/tmp/email-cli-hcache-evict-test", 1);

    const char *folder = "INBOX";

    /* Save two header cache entries: UIDs 10 and 20 */
    hcache_save(folder, 10, "header-10", 9);
    hcache_save(folder, 20, "header-20", 9);

    ASSERT(hcache_exists(folder, 10) == 1, "hcache_evict: UID 10 exists before evict");
    ASSERT(hcache_exists(folder, 20) == 1, "hcache_evict: UID 20 exists before evict");

    /* Keep only UID 20 → UID 10 should be evicted (lines 214-217) */
    int keep[] = {20};
    hcache_evict_stale(folder, keep, 1);

    ASSERT(hcache_exists(folder, 10) == 0, "hcache_evict: UID 10 evicted");
    ASSERT(hcache_exists(folder, 20) == 1, "hcache_evict: UID 20 kept");

    /* Cleanup */
    hcache_evict_stale(folder, NULL, 0);  /* evict everything */
    unlink("/tmp/email-cli-hcache-evict-test/.cache/email-cli/headers/INBOX/20.hdr");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

void test_ui_prefs(void) {
    char *old_home = getenv("HOME");
    setenv("HOME", "/tmp/email-cli-ui-pref-test-home", 1);
    unlink("/tmp/email-cli-ui-pref-test-home/.cache/email-cli/ui.ini");

    /* 1. Missing key returns default */
    ASSERT(ui_pref_get_int("folder_view_mode", 1) == 1,
           "ui_pref_get_int: missing key should return default 1");
    ASSERT(ui_pref_get_int("folder_view_mode", 0) == 0,
           "ui_pref_get_int: missing key should return default 0");

    /* 2. Set and get back */
    ASSERT(ui_pref_set_int("folder_view_mode", 0) == 0,
           "ui_pref_set_int: should return 0 on success");
    ASSERT(ui_pref_get_int("folder_view_mode", 1) == 0,
           "ui_pref_get_int: should return stored value 0");

    /* 3. Overwrite existing key */
    ASSERT(ui_pref_set_int("folder_view_mode", 1) == 0,
           "ui_pref_set_int: overwrite should return 0");
    ASSERT(ui_pref_get_int("folder_view_mode", 0) == 1,
           "ui_pref_get_int: should return updated value 1");

    /* 4. Multiple keys coexist */
    ASSERT(ui_pref_set_int("other_pref", 42) == 0,
           "ui_pref_set_int: second key should return 0");
    ASSERT(ui_pref_get_int("folder_view_mode", 0) == 1,
           "ui_pref_get_int: first key intact after adding second");
    ASSERT(ui_pref_get_int("other_pref", 0) == 42,
           "ui_pref_get_int: second key should return 42");

    /* 5. Unknown key still returns default */
    ASSERT(ui_pref_get_int("no_such_key", 7) == 7,
           "ui_pref_get_int: unknown key should return default");

    /* Cleanup */
    unlink("/tmp/email-cli-ui-pref-test-home/.cache/email-cli/ui.ini");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}
