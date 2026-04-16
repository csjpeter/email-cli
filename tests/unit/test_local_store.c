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
    local_store_init("imaps://test.example.com", "testuser");
}

/* ── Message store tests ─────────────────────────────────────────────── */

void test_local_msg_store(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-store-test");

    const char *folder = "INBOX";
    const char *uid    = "0000000000000137";

    /* Pre-clean */
    unlink("/tmp/email-cli-store-test/.local/share/email-cli/accounts/"
           "testuser/store/INBOX/7/3/0000000000000137.eml");

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
    ASSERT(local_msg_exists(folder, "0000000000000099") == 0,
           "local_msg_exists: UID 99 should not exist");

    /* 5. Reverse digit bucketing: UID 42 → 2/4/ */
    const char *c2 = "Subject: UID 42\r\n\r\nBody";
    local_msg_save(folder, "0000000000000042", c2, strlen(c2));
    ASSERT(local_msg_exists(folder, "0000000000000042") == 1,
           "local_msg_exists: UID 42 after save (bucket 2/4)");

    /* 6. UID 5 → 5/0/ (single digit pads to 0) */
    const char *c3 = "Subject: UID 5\r\n\r\nBody";
    local_msg_save(folder, "0000000000000005", c3, strlen(c3));
    ASSERT(local_msg_exists(folder, "0000000000000005") == 1,
           "local_msg_exists: UID 5 after save (bucket 5/0)");

    /* Cleanup */
    unlink("/tmp/email-cli-store-test/.local/share/email-cli/accounts/"
           "testuser/store/INBOX/7/3/0000000000000137.eml");
    unlink("/tmp/email-cli-store-test/.local/share/email-cli/accounts/"
           "testuser/store/INBOX/2/4/0000000000000042.eml");
    unlink("/tmp/email-cli-store-test/.local/share/email-cli/accounts/"
           "testuser/store/INBOX/5/0/0000000000000005.eml");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

/* ── Header eviction tests ───────────────────────────────────────────── */

void test_local_hdr_evict(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-hdr-evict-test");

    const char *folder = "INBOX";

    local_hdr_save(folder, "0000000000000010", "header-10", 9);
    local_hdr_save(folder, "0000000000000020", "header-20", 9);

    ASSERT(local_hdr_exists(folder, "0000000000000010") == 1, "hdr_evict: UID 10 before");
    ASSERT(local_hdr_exists(folder, "0000000000000020") == 1, "hdr_evict: UID 20 before");

    char keep[][17] = {"0000000000000020"};
    local_hdr_evict_stale(folder, (const char (*)[17])keep, 1);

    ASSERT(local_hdr_exists(folder, "0000000000000010") == 0, "hdr_evict: UID 10 evicted");
    ASSERT(local_hdr_exists(folder, "0000000000000020") == 1, "hdr_evict: UID 20 kept");

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

    int rc = local_index_update("INBOX", "0000000000000042", msg);
    ASSERT(rc == 0, "local_index_update: should return 0");

    /* Verify from index exists */
    const char *from_path =
        "/tmp/email-cli-index-test/.local/share/email-cli/accounts/"
        "testuser/index/from/github.com/noreply";
    {
        RAII_FILE FILE *fp = fopen(from_path, "r");
        ASSERT(fp != NULL, "from index file should exist");
        if (fp) {
            char line[256];
            ASSERT(fgets(line, sizeof(line), fp) != NULL,
                   "from index should have a line");
            ASSERT(strstr(line, "INBOX/0000000000000042") != NULL,
                   "from index should contain INBOX/0000000000000042");
        }
    }

    /* Verify date index exists */
    const char *date_path =
        "/tmp/email-cli-index-test/.local/share/email-cli/accounts/"
        "testuser/index/date/2026/03/15";
    {
        RAII_FILE FILE *fp = fopen(date_path, "r");
        ASSERT(fp != NULL, "date index file should exist");
        if (fp) {
            char line[256];
            ASSERT(fgets(line, sizeof(line), fp) != NULL,
                   "date index should have a line");
            ASSERT(strstr(line, "INBOX/0000000000000042") != NULL,
                   "date index should contain INBOX/0000000000000042");
        }
    }

    /* Duplicate should not be added */
    local_index_update("INBOX", "0000000000000042", msg);
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

/* ── Manifest tests ──────────────────────────────────────────────────── */

void test_manifest(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-manifest-test");

    /* Pre-clean */
    unlink("/tmp/email-cli-manifest-test/.local/share/email-cli/"
           "accounts/testuser/manifests/INBOX.tsv");

    /* 1. Load non-existent manifest returns NULL */
    Manifest *m = manifest_load("INBOX");
    ASSERT(m == NULL, "manifest_load: NULL for missing file");

    /* 2. Create manifest, add entries, save */
    m = calloc(1, sizeof(Manifest));
    ASSERT(m != NULL, "manifest: calloc");
    manifest_upsert(m, "0000000000000042", strdup("Alice <alice@example.com>"),
                              strdup("Hello World"),
                              strdup("2024-03-15 10:00"), MSG_FLAG_UNSEEN);
    manifest_upsert(m, "0000000000000137", strdup("Bob <bob@test.org>"),
                              strdup("Re: Meeting"),
                              strdup("2024-03-16 14:30"), 0);
    ASSERT(m->count == 2, "manifest: 2 entries after upsert");

    int rc = manifest_save("INBOX", m);
    ASSERT(rc == 0, "manifest_save: returns 0");

    /* 3. Load back and verify */
    manifest_free(m);
    m = manifest_load("INBOX");
    ASSERT(m != NULL, "manifest_load: not NULL after save");
    ASSERT(m->count == 2, "manifest_load: 2 entries");

    ManifestEntry *e42 = manifest_find(m, "0000000000000042");
    ASSERT(e42 != NULL, "manifest_find: UID 42 found");
    ASSERT(strcmp(e42->from, "Alice <alice@example.com>") == 0,
           "manifest: UID 42 from correct");
    ASSERT(strcmp(e42->subject, "Hello World") == 0,
           "manifest: UID 42 subject correct");
    ASSERT(strcmp(e42->date, "2024-03-15 10:00") == 0,
           "manifest: UID 42 date correct");

    ManifestEntry *e137 = manifest_find(m, "0000000000000137");
    ASSERT(e137 != NULL, "manifest_find: UID 137 found");
    ASSERT(strcmp(e137->subject, "Re: Meeting") == 0,
           "manifest: UID 137 subject correct");

    /* 4. Upsert updates existing entry */
    manifest_upsert(m, "0000000000000042", strdup("Alice Updated"),
                           strdup("Updated Subject"),
                           strdup("2024-03-15 11:00"), 0 /* no flags */);
    ASSERT(m->count == 2, "manifest: still 2 after upsert-update");
    e42 = manifest_find(m, "0000000000000042");
    ASSERT(strcmp(e42->subject, "Updated Subject") == 0,
           "manifest: upsert updated subject");

    /* 5. manifest_find returns NULL for missing UID */
    ASSERT(manifest_find(m, "0000000000000999") == NULL,
           "manifest_find: NULL for missing UID");

    /* 6. manifest_retain keeps only specified UIDs */
    char keep[][17] = {"0000000000000137"};
    manifest_retain(m, (const char (*)[17])keep, 1);
    ASSERT(m->count == 1, "manifest_retain: 1 entry after retain");
    ASSERT(manifest_find(m, "0000000000000137") != NULL, "manifest_retain: UID 137 kept");
    ASSERT(manifest_find(m, "0000000000000042") == NULL, "manifest_retain: UID 42 removed");

    /* 7. Nested folder manifest */
    Manifest *m2 = calloc(1, sizeof(Manifest));
    manifest_upsert(m2, "0000000000000001", strdup("Test"), strdup("Nested"), strdup("2024-01-01 00:00"), 0 /* no flags */);
    ASSERT(manifest_save("munka/ai", m2) == 0, "manifest_save: nested folder");
    manifest_free(m2);
    m2 = manifest_load("munka/ai");
    ASSERT(m2 != NULL, "manifest_load: nested folder");
    ASSERT(m2->count == 1, "manifest: nested has 1 entry");
    manifest_free(m2);

    /* Cleanup */
    manifest_free(m);
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
