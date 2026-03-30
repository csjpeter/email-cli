#include "test_helpers.h"
#include "local_store.h"
#include "fs_util.h"
#include "raii.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ── Helper to set up a clean test environment ────────────────────────── */

static void setup_test_env(const char *home) {
    setenv("HOME", home, 1);
    unsetenv("XDG_DATA_HOME");
    local_store_init("imaps://test.example.com");
}

/* ── Message store tests ─────────────────────────────────────────────── */

void test_local_msg_store(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-store-test");

    const char *folder = "INBOX";
    const int   uid    = 137;

    /* Pre-clean */
    RAII_STRING char *cleanup_path = NULL;
    asprintf(&cleanup_path,
             "/tmp/email-cli-store-test/.local/share/email-cli/accounts/"
             "imap.test.example.com/store/INBOX/7/3/137.eml");
    unlink(cleanup_path);

    /* 1. Not stored initially */
    ASSERT(local_msg_exists(folder, uid) == 0,
           "local_msg_exists: should be 0 before save");

    /* 2. Save and verify */
    const char *content = "From: test@example.com\r\nDate: Mon, 30 Mar 2026 12:00:00 +0000\r\n"
                          "Subject: Test\r\n\r\nHello!";
    int rc = local_msg_save(folder, uid, content, strlen(content));
    ASSERT(rc == 0, "local_msg_save: should return 0");
    ASSERT(local_msg_exists(folder, uid) == 1,
           "local_msg_exists: should be 1 after save");

    /* 3. Load and verify content */
    {
        RAII_STRING char *loaded = local_msg_load(folder, uid);
        ASSERT(loaded != NULL, "local_msg_load: should not be NULL");
        ASSERT(strcmp(loaded, content) == 0, "local_msg_load: content mismatch");
    }

    /* 4. Different UIDs are independent */
    ASSERT(local_msg_exists(folder, 99) == 0,
           "local_msg_exists: UID 99 should not exist");

    /* 5. Reverse digit bucketing: UID 42 → 2/4/ */
    const char *c2 = "Subject: UID 42\r\n\r\nBody";
    local_msg_save(folder, 42, c2, strlen(c2));
    ASSERT(local_msg_exists(folder, 42) == 1,
           "local_msg_exists: UID 42 after save (bucket 2/4)");

    /* 6. UID 5 → 5/0/ (single digit pads to 0) */
    const char *c3 = "Subject: UID 5\r\n\r\nBody";
    local_msg_save(folder, 5, c3, strlen(c3));
    ASSERT(local_msg_exists(folder, 5) == 1,
           "local_msg_exists: UID 5 after save (bucket 5/0)");

    /* Cleanup */
    unlink(cleanup_path);
    /* UID 42 */
    RAII_STRING char *p42 = NULL;
    asprintf(&p42,
             "/tmp/email-cli-store-test/.local/share/email-cli/accounts/"
             "imap.test.example.com/store/INBOX/2/4/42.eml");
    unlink(p42);
    /* UID 5 */
    RAII_STRING char *p5 = NULL;
    asprintf(&p5,
             "/tmp/email-cli-store-test/.local/share/email-cli/accounts/"
             "imap.test.example.com/store/INBOX/5/0/5.eml");
    unlink(p5);

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

/* ── Header eviction tests ───────────────────────────────────────────── */

void test_local_hdr_evict(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-hdr-evict-test");

    const char *folder = "INBOX";

    local_hdr_save(folder, 10, "header-10", 9);
    local_hdr_save(folder, 20, "header-20", 9);

    ASSERT(local_hdr_exists(folder, 10) == 1, "hdr_evict: UID 10 before");
    ASSERT(local_hdr_exists(folder, 20) == 1, "hdr_evict: UID 20 before");

    int keep[] = {20};
    local_hdr_evict_stale(folder, keep, 1);

    ASSERT(local_hdr_exists(folder, 10) == 0, "hdr_evict: UID 10 evicted");
    ASSERT(local_hdr_exists(folder, 20) == 1, "hdr_evict: UID 20 kept");

    /* Cleanup */
    local_hdr_evict_stale(folder, NULL, 0);

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

/* ── Index tests ─────────────────────────────────────────────────────── */

void test_local_index(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-index-test");

    const char *msg =
        "From: noreply@github.com\r\n"
        "Date: Tue, 15 Mar 2026 10:30:00 +0100\r\n"
        "Subject: Test\r\n\r\nBody";

    int rc = local_index_update("INBOX", 42, msg);
    ASSERT(rc == 0, "local_index_update: should return 0");

    /* Verify from index exists */
    RAII_STRING char *from_path = NULL;
    asprintf(&from_path,
             "/tmp/email-cli-index-test/.local/share/email-cli/accounts/"
             "imap.test.example.com/index/from/github.com/noreply");
    {
        RAII_STRING char *content = NULL;
        RAII_FILE FILE *fp = fopen(from_path, "r");
        ASSERT(fp != NULL, "from index file should exist");
        if (fp) {
            char line[256];
            ASSERT(fgets(line, sizeof(line), fp) != NULL,
                   "from index should have a line");
            ASSERT(strstr(line, "INBOX/42") != NULL,
                   "from index should contain INBOX/42");
        }
    }

    /* Verify date index exists */
    RAII_STRING char *date_path = NULL;
    asprintf(&date_path,
             "/tmp/email-cli-index-test/.local/share/email-cli/accounts/"
             "imap.test.example.com/index/date/2026/03/15");
    {
        RAII_FILE FILE *fp = fopen(date_path, "r");
        ASSERT(fp != NULL, "date index file should exist");
        if (fp) {
            char line[256];
            ASSERT(fgets(line, sizeof(line), fp) != NULL,
                   "date index should have a line");
            ASSERT(strstr(line, "INBOX/42") != NULL,
                   "date index should contain INBOX/42");
        }
    }

    /* Duplicate should not be added */
    local_index_update("INBOX", 42, msg);
    {
        RAII_FILE FILE *fp = fopen(from_path, "r");
        int count = 0;
        char line[256];
        while (fp && fgets(line, sizeof(line), fp)) count++;
        ASSERT(count == 1, "from index should have exactly 1 entry (no dupes)");
    }

    /* Cleanup */
    if (from_path) unlink(from_path);
    if (date_path) unlink(date_path);

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

/* ── UI preferences tests ────────────────────────────────────────────── */

void test_ui_prefs(void) {
    char *old_home = getenv("HOME");
    setenv("HOME", "/tmp/email-cli-ui-pref-test-home", 1);
    unsetenv("XDG_DATA_HOME");
    unlink("/tmp/email-cli-ui-pref-test-home/.local/share/email-cli/ui.ini");

    ASSERT(ui_pref_get_int("folder_view_mode", 1) == 1,
           "ui_pref_get_int: missing key should return default 1");
    ASSERT(ui_pref_get_int("folder_view_mode", 0) == 0,
           "ui_pref_get_int: missing key should return default 0");

    ASSERT(ui_pref_set_int("folder_view_mode", 0) == 0,
           "ui_pref_set_int: should return 0");
    ASSERT(ui_pref_get_int("folder_view_mode", 1) == 0,
           "ui_pref_get_int: should return stored value 0");

    ASSERT(ui_pref_set_int("folder_view_mode", 1) == 0,
           "ui_pref_set_int: overwrite should return 0");
    ASSERT(ui_pref_get_int("folder_view_mode", 0) == 1,
           "ui_pref_get_int: should return updated value 1");

    ASSERT(ui_pref_set_int("other_pref", 42) == 0,
           "ui_pref_set_int: second key should return 0");
    ASSERT(ui_pref_get_int("folder_view_mode", 0) == 1,
           "ui_pref_get_int: first key intact");
    ASSERT(ui_pref_get_int("other_pref", 0) == 42,
           "ui_pref_get_int: second key should return 42");

    ASSERT(ui_pref_get_int("no_such_key", 7) == 7,
           "ui_pref_get_int: unknown key should return default");

    unlink("/tmp/email-cli-ui-pref-test-home/.local/share/email-cli/ui.ini");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}
