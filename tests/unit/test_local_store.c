#include "test_helpers.h"
#include "local_store.h"
#include "fs_util.h"
#include "raii.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

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

    /* ui_pref_get_str / ui_pref_set_str */
    ASSERT(ui_pref_get_str("str_key") == NULL,
           "ui_pref_get_str: missing file returns NULL");

    ASSERT(ui_pref_set_str("str_key", "hello") == 0,
           "ui_pref_set_str: returns 0 on first write");
    {
        char *v = ui_pref_get_str("str_key");
        ASSERT(v != NULL, "ui_pref_get_str: returns non-NULL after set");
        ASSERT(strcmp(v, "hello") == 0, "ui_pref_get_str: value matches");
        free(v);
    }

    ASSERT(ui_pref_set_str("str_key", "world") == 0,
           "ui_pref_set_str: overwrite returns 0");
    {
        char *v = ui_pref_get_str("str_key");
        ASSERT(v != NULL, "ui_pref_get_str: overwrite non-NULL");
        ASSERT(strcmp(v, "world") == 0, "ui_pref_get_str: overwrite value matches");
        free(v);
    }

    ASSERT(ui_pref_set_str("another_key", "value2") == 0,
           "ui_pref_set_str: second key returns 0");
    {
        char *v1 = ui_pref_get_str("str_key");
        char *v2 = ui_pref_get_str("another_key");
        ASSERT(v1 && strcmp(v1, "world") == 0,  "ui_pref_get_str: first key intact");
        ASSERT(v2 && strcmp(v2, "value2") == 0, "ui_pref_get_str: second key correct");
        free(v1); free(v2);
    }

    ASSERT(ui_pref_get_str("no_such_str_key") == NULL,
           "ui_pref_get_str: unknown key returns NULL");

    unlink("/tmp/email-cli-ui-pref-test-home/.local/share/email-cli/ui.ini");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

/* ── msg delete tests ────────────────────────────────────────────────── */

void test_local_msg_delete(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-delete-test");

    const char *folder = "INBOX";
    const char *uid    = "0000000000001000";

    /* Save then delete */
    int rc = local_msg_save(folder, uid, "test body", 9);
    ASSERT(rc == 0, "delete: save succeeded");
    ASSERT(local_msg_exists(folder, uid) == 1, "delete: exists before delete");

    int del = local_msg_delete(folder, uid);
    ASSERT(del == 0, "delete: returns 0");
    ASSERT(local_msg_exists(folder, uid) == 0, "delete: does not exist after delete");

    /* Deleting a non-existent message should not crash */
    int del2 = local_msg_delete(folder, "0000000000001001");
    ASSERT(del2 == 0, "delete: non-existent msg returns 0");

    /* Delete also removes .hdr if it exists */
    const char *uid2 = "0000000000001002";
    local_hdr_save(folder, uid2, "from\tsubject\tdate\t\t0", 20);
    ASSERT(local_hdr_exists(folder, uid2) == 1, "delete: hdr exists before delete");
    local_msg_delete(folder, uid2);
    ASSERT(local_hdr_exists(folder, uid2) == 0, "delete: hdr removed by delete");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

/* ── index email extraction tests ────────────────────────────────────── */

void test_local_index_email_extraction(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-email-extract-test");

    /* "Name <user@domain>" format */
    {
        const char *msg =
            "From: Alice <alice@example.com>\r\n"
            "Date: Mon, 01 Jan 2024 10:00:00 +0000\r\n"
            "Subject: Test\r\n\r\nBody";
        int rc = local_index_update("INBOX", "0000000000000201", msg);
        ASSERT(rc == 0, "email_extraction: Name<email> index_update returns 0");

        const char *from_path =
            "/tmp/email-cli-email-extract-test/.local/share/email-cli/accounts/"
            "testuser/index/from/example.com/alice";
        RAII_FILE FILE *fp = fopen(from_path, "r");
        ASSERT(fp != NULL, "email_extraction: Name<email> from index created");
        if (fp) {
            char line[256];
            ASSERT(fgets(line, sizeof(line), fp) != NULL,
                   "email_extraction: from index has a line");
            ASSERT(strstr(line, "INBOX/0000000000000201") != NULL,
                   "email_extraction: from index contains correct ref");
        }
    }

    /* "<user@domain>" format without display name */
    {
        const char *msg2 =
            "From: <bob@test.org>\r\n"
            "Date: Tue, 02 Jan 2024 11:00:00 +0000\r\n"
            "Subject: Test2\r\n\r\nBody";
        int rc = local_index_update("INBOX", "0000000000000202", msg2);
        ASSERT(rc == 0, "email_extraction: <email> index_update returns 0");

        const char *from_path2 =
            "/tmp/email-cli-email-extract-test/.local/share/email-cli/accounts/"
            "testuser/index/from/test.org/bob";
        RAII_FILE FILE *fp2 = fopen(from_path2, "r");
        ASSERT(fp2 != NULL, "email_extraction: <email> from index created");
    }

    /* Plain "user@domain" format */
    {
        const char *msg3 =
            "From: carol@sample.net\r\n"
            "Date: Wed, 03 Jan 2024 12:00:00 +0000\r\n"
            "Subject: Test3\r\n\r\nBody";
        int rc = local_index_update("INBOX", "0000000000000203", msg3);
        ASSERT(rc == 0, "email_extraction: plain email index_update returns 0");

        const char *from_path3 =
            "/tmp/email-cli-email-extract-test/.local/share/email-cli/accounts/"
            "testuser/index/from/sample.net/carol";
        RAII_FILE FILE *fp3 = fopen(from_path3, "r");
        ASSERT(fp3 != NULL, "email_extraction: plain email from index created");
    }

    /* From header with no @ sign: no from index entry created (graceful skip) */
    {
        const char *msg4 =
            "From: noemail\r\n"
            "Date: Thu, 04 Jan 2024 13:00:00 +0000\r\n"
            "Subject: Test4\r\n\r\nBody";
        int rc = local_index_update("INBOX", "0000000000000204", msg4);
        ASSERT(rc == 0, "email_extraction: no-@ returns 0 (graceful)");
    }

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

/* ── trash labels tests ──────────────────────────────────────────────── */

void test_local_trash_labels(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-trash-labels-test");

    const char *uid = "0000000000002000";
    const char *labels = "INBOX,Work,Important";

    /* Save and load */
    int rc = local_trash_labels_save(uid, labels);
    ASSERT(rc == 0, "trash_labels: save returns 0");

    RAII_STRING char *loaded = local_trash_labels_load(uid);
    ASSERT(loaded != NULL, "trash_labels: load returns non-NULL");
    ASSERT(strcmp(loaded, labels) == 0, "trash_labels: loaded value matches saved");

    /* Remove */
    local_trash_labels_remove(uid);
    RAII_STRING char *after_remove = local_trash_labels_load(uid);
    ASSERT(after_remove == NULL, "trash_labels: NULL after remove");

    /* Load non-existent returns NULL */
    RAII_STRING char *missing = local_trash_labels_load("0000000000009999");
    ASSERT(missing == NULL, "trash_labels: missing uid returns NULL");

    /* Remove non-existent should not crash */
    local_trash_labels_remove("0000000000009998");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

/* ── gmail history id tests ──────────────────────────────────────────── */

void test_local_gmail_history(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-gmail-history-test");

    /* Pre-clean any leftover history file from previous runs */
    unlink("/tmp/email-cli-gmail-history-test/.local/share/email-cli/accounts/"
           "testuser/gmail_history_id");

    /* Load when missing returns NULL */
    RAII_STRING char *none = local_gmail_history_load();
    ASSERT(none == NULL, "gmail_history: missing returns NULL");

    /* Save and load */
    int rc = local_gmail_history_save("12345678");
    ASSERT(rc == 0, "gmail_history: save returns 0");

    RAII_STRING char *loaded = local_gmail_history_load();
    ASSERT(loaded != NULL, "gmail_history: load returns non-NULL");
    ASSERT(strcmp(loaded, "12345678") == 0, "gmail_history: loaded value matches");

    /* Overwrite with new value */
    int rc2 = local_gmail_history_save("99999999");
    ASSERT(rc2 == 0, "gmail_history: overwrite returns 0");

    RAII_STRING char *loaded2 = local_gmail_history_load();
    ASSERT(loaded2 != NULL, "gmail_history: overwrite load returns non-NULL");
    ASSERT(strcmp(loaded2, "99999999") == 0, "gmail_history: overwrite value correct");

    /* Null history id returns -1 */
    int rc3 = local_gmail_history_save(NULL);
    ASSERT(rc3 == -1, "gmail_history: NULL id returns -1");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

/* ── local_hdr_get_labels tests ──────────────────────────────────────── */

void test_local_hdr_get_labels(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-hdr-labels-test");

    const char *folder = "";  /* Gmail flat store uses empty folder */
    const char *uid    = "0000000000003000";

    /* Gmail .hdr format: from\tsubject\tdate\tlabels\tflags */
    const char *hdr_content = "Alice <alice@example.com>\tHello\t2024-01-01 10:00\tINBOX,Work\t1";
    int rc = local_hdr_save(folder, uid, hdr_content, strlen(hdr_content));
    ASSERT(rc == 0, "hdr_labels: save returns 0");

    RAII_STRING char *labels = local_hdr_get_labels(folder, uid);
    ASSERT(labels != NULL, "hdr_labels: get_labels returns non-NULL");
    ASSERT(strcmp(labels, "INBOX,Work") == 0, "hdr_labels: labels value correct");

    /* Non-Gmail .hdr (no labels field) → NULL */
    const char *uid2 = "0000000000003001";
    const char *hdr2 = "Bob\tSubject\tDate";  /* only 3 fields, no 4th tab */
    local_hdr_save(folder, uid2, hdr2, strlen(hdr2));
    RAII_STRING char *labels2 = local_hdr_get_labels(folder, uid2);
    ASSERT(labels2 == NULL, "hdr_labels: non-Gmail hdr returns NULL");

    /* Non-existent uid → NULL */
    RAII_STRING char *labels3 = local_hdr_get_labels(folder, "0000000000009997");
    ASSERT(labels3 == NULL, "hdr_labels: missing uid returns NULL");

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

/* ── local_flag_search tests ────────────────────────────────────────────── */

void test_local_flag_search(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-flag-search-test");

    /* Pre-clean */
    unlink("/tmp/email-cli-flag-search-test/.local/share/email-cli/"
           "accounts/testuser/manifests/INBOX.tsv");
    unlink("/tmp/email-cli-flag-search-test/.local/share/email-cli/"
           "accounts/testuser/manifests/Sent.tsv");

    /* Build two manifests across two folders */
    Manifest *inbox = calloc(1, sizeof(Manifest));
    manifest_upsert(inbox, "0000000000000001", strdup("Alice"), strdup("Unread in INBOX"),
                    strdup("2024-03-01 10:00"), MSG_FLAG_UNSEEN);
    manifest_upsert(inbox, "0000000000000002", strdup("Bob"),   strdup("Read in INBOX"),
                    strdup("2024-03-02 11:00"), 0);
    manifest_save("INBOX", inbox);
    manifest_free(inbox);

    Manifest *sent = calloc(1, sizeof(Manifest));
    manifest_upsert(sent, "0000000000000003", strdup("Carol"), strdup("Flagged in Sent"),
                    strdup("2024-03-03 12:00"), MSG_FLAG_FLAGGED);
    manifest_upsert(sent, "0000000000000004", strdup("Dave"),  strdup("Unread in Sent"),
                    strdup("2024-03-04 13:00"), MSG_FLAG_UNSEEN);
    manifest_save("Sent", sent);
    manifest_free(sent);

    /* Test 1: flag_search for UNSEEN finds UID 1 (INBOX) and UID 4 (Sent) */
    SearchResult *res = NULL;
    int cnt = 0;
    int rc = local_flag_search(MSG_FLAG_UNSEEN, &res, &cnt);
    ASSERT(rc == 0,   "flag_search: returns 0");
    ASSERT(cnt == 2,  "flag_search UNSEEN: 2 results");
    int found1 = 0, found4 = 0;
    for (int i = 0; i < cnt; i++) {
        if (strcmp(res[i].uid, "0000000000000001") == 0) {
            found1 = 1;
            ASSERT(strcmp(res[i].folder, "INBOX") == 0, "flag_search: UID1 folder is INBOX");
            ASSERT(res[i].flags & MSG_FLAG_UNSEEN,       "flag_search: UID1 has UNSEEN");
        }
        if (strcmp(res[i].uid, "0000000000000004") == 0) {
            found4 = 1;
            ASSERT(strcmp(res[i].folder, "Sent") == 0,   "flag_search: UID4 folder is Sent");
        }
    }
    ASSERT(found1, "flag_search UNSEEN: UID1 found");
    ASSERT(found4, "flag_search UNSEEN: UID4 found");
    local_search_free(res, cnt);

    /* Test 2: flag_search for FLAGGED finds only UID 3 (Sent) */
    res = NULL; cnt = 0;
    local_flag_search(MSG_FLAG_FLAGGED, &res, &cnt);
    ASSERT(cnt == 1,  "flag_search FLAGGED: 1 result");
    ASSERT(strcmp(res[0].uid, "0000000000000003") == 0, "flag_search FLAGGED: UID3");
    ASSERT(strcmp(res[0].folder, "Sent") == 0,           "flag_search FLAGGED: Sent folder");
    local_search_free(res, cnt);

    /* Test 3: flag_search for both bits returns union (UID1, UID3, UID4) */
    res = NULL; cnt = 0;
    local_flag_search(MSG_FLAG_UNSEEN | MSG_FLAG_FLAGGED, &res, &cnt);
    ASSERT(cnt == 3,  "flag_search UNSEEN|FLAGGED: 3 results");
    local_search_free(res, cnt);

    /* Test 4: UID2 (read, no flags) is never returned */
    res = NULL; cnt = 0;
    local_flag_search(MSG_FLAG_UNSEEN, &res, &cnt);
    int found2 = 0;
    for (int i = 0; i < cnt; i++)
        if (strcmp(res[i].uid, "0000000000000002") == 0) found2 = 1;
    ASSERT(!found2, "flag_search: read UID2 not in UNSEEN results");
    local_search_free(res, cnt);

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

void test_manifest_count_after_flag_update(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-count-update-test");

    /* Pre-clean */
    unlink("/tmp/email-cli-count-update-test/.local/share/email-cli/"
           "accounts/testuser/manifests/INBOX.tsv");

    /* Build a manifest with 3 unread messages */
    Manifest *m = calloc(1, sizeof(Manifest));
    manifest_upsert(m, "0000000000000010", strdup("A"), strdup("Msg1"),
                    strdup("2024-01-01 08:00"), MSG_FLAG_UNSEEN);
    manifest_upsert(m, "0000000000000020", strdup("B"), strdup("Msg2"),
                    strdup("2024-01-02 09:00"), MSG_FLAG_UNSEEN);
    manifest_upsert(m, "0000000000000030", strdup("C"), strdup("Msg3"),
                    strdup("2024-01-03 10:00"), 0 /* read */);
    manifest_save("INBOX", m);

    /* Initial count: 2 unread, 0 flagged */
    int unread = -1, flagged = -1;
    manifest_count_all_flags(&unread, &flagged);
    ASSERT(unread  == 2, "count_after_update: initial unread is 2");
    ASSERT(flagged == 0, "count_after_update: initial flagged is 0");

    /* Simulate user pressing 'n' on UID 10 — mark as read */
    ManifestEntry *e = manifest_find(m, "0000000000000010");
    ASSERT(e != NULL, "count_after_update: UID10 found");
    e->flags &= ~MSG_FLAG_UNSEEN;
    manifest_save("INBOX", m);

    /* Count should now be 1 */
    manifest_count_all_flags(&unread, &flagged);
    ASSERT(unread == 1, "count_after_update: unread drops to 1 after save");

    /* Mark UID 20 as read too */
    e = manifest_find(m, "0000000000000020");
    e->flags &= ~MSG_FLAG_UNSEEN;
    manifest_save("INBOX", m);

    manifest_count_all_flags(&unread, &flagged);
    ASSERT(unread == 0, "count_after_update: unread drops to 0");

    /* Flag UID 30 */
    e = manifest_find(m, "0000000000000030");
    e->flags |= MSG_FLAG_FLAGGED;
    manifest_save("INBOX", m);

    manifest_count_all_flags(&unread, &flagged);
    ASSERT(flagged == 1, "count_after_update: flagged becomes 1");

    manifest_free(m);
    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}

void test_flag_search_folder_isolation(void) {
    char *old_home = getenv("HOME");
    setup_test_env("/tmp/email-cli-flag-iso-test");

    /* Pre-clean */
    unlink("/tmp/email-cli-flag-iso-test/.local/share/email-cli/"
           "accounts/testuser/manifests/INBOX.tsv");
    unlink("/tmp/email-cli-flag-iso-test/.local/share/email-cli/"
           "accounts/testuser/manifests/Spam.tsv");

    /* Same UID in two different folders (shouldn't happen in practice but verify
     * that flag_search reports the correct folder for each) */
    Manifest *inbox = calloc(1, sizeof(Manifest));
    manifest_upsert(inbox, "0000000000000099", strdup("X"), strdup("INBOX copy"),
                    strdup("2024-06-01 00:00"), MSG_FLAG_UNSEEN);
    manifest_save("INBOX", inbox);
    manifest_free(inbox);

    Manifest *spam = calloc(1, sizeof(Manifest));
    manifest_upsert(spam, "0000000000000099", strdup("X"), strdup("Spam copy"),
                    strdup("2024-06-01 00:00"), MSG_FLAG_UNSEEN);
    manifest_save("Spam", spam);
    manifest_free(spam);

    SearchResult *res = NULL; int cnt = 0;
    local_flag_search(MSG_FLAG_UNSEEN, &res, &cnt);
    ASSERT(cnt == 2, "flag_search isolation: 2 results (same UID in 2 folders)");
    int found_inbox = 0, found_spam = 0;
    for (int i = 0; i < cnt; i++) {
        if (strcmp(res[i].folder, "INBOX") == 0) found_inbox = 1;
        if (strcmp(res[i].folder, "Spam")  == 0) found_spam  = 1;
    }
    ASSERT(found_inbox, "flag_search isolation: INBOX result present");
    ASSERT(found_spam,  "flag_search isolation: Spam result present");
    local_search_free(res, cnt);

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}
