#include "test_helpers.h"
#include "local_store.h"
#include "fs_util.h"
#include "raii.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

void test_local_msg_store(void) {
    char *old_home = getenv("HOME");
    setenv("HOME", "/tmp/email-cli-local-store-test-home", 1);
    unsetenv("XDG_DATA_HOME");

    const char *folder = "INBOX";
    const int   uid    = 42;

    /* Pre-clean from any previous run */
    unlink("/tmp/email-cli-local-store-test-home/.local/share/email-cli/messages/INBOX/42.eml");

    /* 1. Not stored initially */
    ASSERT(local_msg_exists(folder, uid) == 0, "local_msg_exists: should be 0 before save");

    {
        RAII_STRING char *loaded = local_msg_load(folder, uid);
        ASSERT(loaded == NULL, "local_msg_load: should return NULL before save");
    }

    /* 2. Save and verify existence */
    const char *content = "Subject: Local Store Test\r\n\r\nHello local store!";
    int rc = local_msg_save(folder, uid, content, strlen(content));
    ASSERT(rc == 0, "local_msg_save: should return 0 on success");
    ASSERT(local_msg_exists(folder, uid) == 1, "local_msg_exists: should be 1 after save");

    /* 3. Load and verify content */
    {
        RAII_STRING char *loaded = local_msg_load(folder, uid);
        ASSERT(loaded != NULL, "local_msg_load: should not be NULL after save");
        ASSERT(strcmp(loaded, content) == 0, "local_msg_load: content mismatch");
    }

    /* 4. Overwrite with new content */
    const char *content2 = "Subject: Updated\r\n\r\nNew body.";
    rc = local_msg_save(folder, uid, content2, strlen(content2));
    ASSERT(rc == 0, "local_msg_save: overwrite should return 0");
    {
        RAII_STRING char *loaded = local_msg_load(folder, uid);
        ASSERT(loaded != NULL, "local_msg_load: should not be NULL after overwrite");
        ASSERT(strcmp(loaded, content2) == 0, "local_msg_load: overwritten content mismatch");
    }

    /* 5. Different UIDs are independent */
    ASSERT(local_msg_exists(folder, 99) == 0, "local_msg_exists: UID 99 should not exist");

    /* Cleanup */
    unlink("/tmp/email-cli-local-store-test-home/.local/share/email-cli/messages/INBOX/42.eml");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

void test_local_hdr_evict(void) {
    char *old_home = getenv("HOME");
    setenv("HOME", "/tmp/email-cli-hdr-evict-test", 1);
    unsetenv("XDG_DATA_HOME");

    const char *folder = "INBOX";

    /* Save two header entries: UIDs 10 and 20 */
    local_hdr_save(folder, 10, "header-10", 9);
    local_hdr_save(folder, 20, "header-20", 9);

    ASSERT(local_hdr_exists(folder, 10) == 1, "local_hdr_evict: UID 10 exists before evict");
    ASSERT(local_hdr_exists(folder, 20) == 1, "local_hdr_evict: UID 20 exists before evict");

    /* Keep only UID 20 → UID 10 should be evicted */
    int keep[] = {20};
    local_hdr_evict_stale(folder, keep, 1);

    ASSERT(local_hdr_exists(folder, 10) == 0, "local_hdr_evict: UID 10 evicted");
    ASSERT(local_hdr_exists(folder, 20) == 1, "local_hdr_evict: UID 20 kept");

    /* Cleanup */
    local_hdr_evict_stale(folder, NULL, 0);  /* evict everything */
    unlink("/tmp/email-cli-hdr-evict-test/.local/share/email-cli/headers/INBOX/20.hdr");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

void test_ui_prefs(void) {
    char *old_home = getenv("HOME");
    setenv("HOME", "/tmp/email-cli-ui-pref-test-home", 1);
    unsetenv("XDG_DATA_HOME");
    unlink("/tmp/email-cli-ui-pref-test-home/.local/share/email-cli/ui.ini");

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
    unlink("/tmp/email-cli-ui-pref-test-home/.local/share/email-cli/ui.ini");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}
