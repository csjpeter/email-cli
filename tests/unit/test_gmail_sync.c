#include "test_helpers.h"
#include "gmail_sync.h"
#include "local_store.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── is_filtered_label ───────────────────────────────────────────────── */

static void test_filtered_null(void) {
    ASSERT(gmail_sync_is_filtered_label(NULL) == 1, "filtered: NULL → filtered");
}

static void test_filtered_category(void) {
    /* CATEGORY_* labels are now indexed (not filtered) — they appear in a
     * dedicated section in the TUI label list. */
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_PERSONAL") == 0,
           "not filtered: CATEGORY_PERSONAL (indexed as category)");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_SOCIAL") == 0,
           "not filtered: CATEGORY_SOCIAL (indexed as category)");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_PROMOTIONS") == 0,
           "not filtered: CATEGORY_PROMOTIONS (indexed as category)");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_UPDATES") == 0,
           "not filtered: CATEGORY_UPDATES (indexed as category)");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_FORUMS") == 0,
           "not filtered: CATEGORY_FORUMS (indexed as category)");
}

static void test_filtered_important(void) {
    ASSERT(gmail_sync_is_filtered_label("IMPORTANT") == 1,
           "filtered: IMPORTANT");
}

static void test_not_filtered_system(void) {
    ASSERT(gmail_sync_is_filtered_label("INBOX") == 0, "not filtered: INBOX");
    ASSERT(gmail_sync_is_filtered_label("SENT") == 0, "not filtered: SENT");
    ASSERT(gmail_sync_is_filtered_label("TRASH") == 0, "not filtered: TRASH");
    ASSERT(gmail_sync_is_filtered_label("SPAM") == 0, "not filtered: SPAM");
    ASSERT(gmail_sync_is_filtered_label("STARRED") == 0, "not filtered: STARRED");
    ASSERT(gmail_sync_is_filtered_label("UNREAD") == 0, "not filtered: UNREAD");
    ASSERT(gmail_sync_is_filtered_label("DRAFT") == 0, "not filtered: DRAFT");
}

static void test_not_filtered_user(void) {
    ASSERT(gmail_sync_is_filtered_label("Work") == 0, "not filtered: Work");
    ASSERT(gmail_sync_is_filtered_label("Personal") == 0, "not filtered: Personal");
    ASSERT(gmail_sync_is_filtered_label("Projects/Alpha") == 0,
           "not filtered: nested label");
}

static void test_filtered_edge_cases(void) {
    ASSERT(gmail_sync_is_filtered_label("") == 0, "not filtered: empty string");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_") == 0,
           "not filtered: bare CATEGORY_ prefix (indexed as category)");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_X") == 0,
           "not filtered: unknown CATEGORY_ suffix (indexed as category)");
}

/* ── build_hdr ───────────────────────────────────────────────────────── */

static void test_build_hdr_basic(void) {
    const char *raw = "From: Alice <alice@example.com>\r\n"
                      "Subject: Hello\r\n"
                      "Date: Wed, 16 Apr 2026 09:30:00 +0000\r\n"
                      "\r\n"
                      "Body text\r\n";
    char *labels[] = {"INBOX", "UNREAD"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 2);
    ASSERT(hdr != NULL, "build_hdr basic: not NULL");

    /* Verify tab-separated format: from\tsubject\tdate\tlabels\tflags */
    int tabs = 0;
    for (const char *p = hdr; *p; p++)
        if (*p == '\t') tabs++;
    ASSERT(tabs == 4, "build_hdr basic: 4 tab separators");

    /* Verify label string contains both labels */
    ASSERT(strstr(hdr, "INBOX") != NULL, "build_hdr basic: has INBOX");
    ASSERT(strstr(hdr, "UNREAD") != NULL, "build_hdr basic: has UNREAD");

    /* Verify UNREAD sets MSG_FLAG_UNSEEN bit (value 1) */
    const char *last_tab = strrchr(hdr, '\t');
    ASSERT(last_tab != NULL, "build_hdr basic: has last tab");
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 1) != 0, "build_hdr basic: UNSEEN flag set");

    free(hdr);
}

static void test_build_hdr_starred(void) {
    const char *raw = "From: Bob\r\nSubject: Star me\r\nDate: Thu, 17 Apr 2026 10:00:00 +0000\r\n\r\n";
    char *labels[] = {"STARRED"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 1);
    ASSERT(hdr != NULL, "build_hdr starred: not NULL");

    /* STARRED sets MSG_FLAG_FLAGGED (value 2) */
    const char *last_tab = strrchr(hdr, '\t');
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 2) != 0, "build_hdr starred: FLAGGED flag set");

    free(hdr);
}

static void test_build_hdr_no_labels(void) {
    const char *raw = "From: Nobody\r\nSubject: Archived\r\nDate: Mon, 14 Apr 2026 08:00:00 +0000\r\n\r\n";
    char *hdr = gmail_sync_build_hdr(raw, NULL, 0);
    ASSERT(hdr != NULL, "build_hdr no labels: not NULL");

    /* Flags should be 0 (no UNREAD, no STARRED) */
    const char *last_tab = strrchr(hdr, '\t');
    int flags = atoi(last_tab + 1);
    ASSERT(flags == 0, "build_hdr no labels: flags=0");

    free(hdr);
}

static void test_build_hdr_missing_headers(void) {
    /* Message with no From/Subject/Date headers */
    const char *raw = "\r\nJust a body.\r\n";
    char *labels[] = {"INBOX"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 1);
    ASSERT(hdr != NULL, "build_hdr missing headers: not NULL");

    /* Should have empty fields but not crash */
    ASSERT(hdr[0] == '\t', "build_hdr missing headers: from is empty");

    free(hdr);
}

static void test_build_hdr_combined_flags(void) {
    const char *raw = "From: X\r\nSubject: Y\r\nDate: Mon, 14 Apr 2026 08:00:00 +0000\r\n\r\n";
    char *labels[] = {"UNREAD", "STARRED", "INBOX"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 3);
    ASSERT(hdr != NULL, "build_hdr combined: not NULL");

    const char *last_tab = strrchr(hdr, '\t');
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 1) != 0, "build_hdr combined: UNSEEN set");
    ASSERT((flags & 2) != 0, "build_hdr combined: FLAGGED set");

    free(hdr);
}

/* ── incremental sync — no history ───────────────────────────────────── */

static void test_incremental_no_history(void) {
    /* Without local_store_init, local_gmail_history_load returns NULL → -2 */
    int rc = gmail_sync_incremental(NULL);
    ASSERT(rc == -2, "incremental: no historyId → returns -2");
}

/* ── repair_archive_flags ────────────────────────────────────────────── */

/* Helper: sets HOME to a temp dir and inits local store for Gmail. */
static void setup_gmail_test_env(const char *home) {
    setenv("HOME", home, 1);
    unsetenv("XDG_DATA_HOME");
    local_store_init("gmail://csjpeterjaket@gmail.com", "csjpeterjaket@gmail.com");
}

static void test_repair_archive_flags_clears_unseen(void) {
    /* A message synced to _nolabel while still having UNREAD label → UNSEEN
     * flag should be cleared by repair_archive_flags(). */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    const char *uid = "0000000000aabbcc";

    /* Write a .hdr with UNSEEN bit set (flags = 1) */
    const char *hdr = "Sender\tArchived msg\t2026-04-20\tUNREAD\t1";
    local_hdr_save("", uid, hdr, strlen(hdr));

    /* Add to _nolabel index */
    label_idx_add("_nolabel", uid);

    /* Run repair */
    gmail_sync_repair_archive_flags();

    /* Load and verify UNSEEN was cleared */
    char *loaded = local_hdr_load("", uid);
    ASSERT(loaded != NULL, "repair: .hdr still exists");
    const char *last_tab = strrchr(loaded, '\t');
    ASSERT(last_tab != NULL, "repair: flags tab present");
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 1) == 0, "repair: UNSEEN bit cleared for archived message");
    free(loaded);
}

static void test_repair_archive_flags_preserves_flagged(void) {
    /* STARRED (FLAGGED bit) must survive the repair. */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    const char *uid = "0000000000aabbdd";

    /* flags = 3 = UNSEEN | FLAGGED */
    const char *hdr = "Sender\tStarred archived\t2026-04-20\tUNREAD,STARRED\t3";
    local_hdr_save("", uid, hdr, strlen(hdr));
    label_idx_add("_nolabel", uid);

    gmail_sync_repair_archive_flags();

    char *loaded = local_hdr_load("", uid);
    ASSERT(loaded != NULL, "repair flagged: .hdr exists");
    const char *last_tab = strrchr(loaded, '\t');
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 1) == 0, "repair flagged: UNSEEN cleared");
    ASSERT((flags & 2) != 0, "repair flagged: FLAGGED preserved");
    free(loaded);
}

static void test_repair_archive_flags_noop_when_already_read(void) {
    /* If UNSEEN is already 0, the .hdr should not be rewritten (flags stay). */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    const char *uid = "0000000000aabbee";

    /* flags = 0, no UNREAD */
    const char *hdr = "Sender\tAlready read\t2026-04-20\t\t0";
    local_hdr_save("", uid, hdr, strlen(hdr));
    label_idx_add("_nolabel", uid);

    gmail_sync_repair_archive_flags();

    char *loaded = local_hdr_load("", uid);
    ASSERT(loaded != NULL, "repair noop: .hdr exists");
    const char *last_tab = strrchr(loaded, '\t');
    int flags = atoi(last_tab + 1);
    ASSERT(flags == 0, "repair noop: flags remain 0");
    free(loaded);
}

static void test_build_hdr_archive_unread_flags(void) {
    /* build_hdr with UNREAD but no real label → flags still has UNSEEN.
     * The caller (sync loop) is responsible for clearing it when assigning
     * to _nolabel.  Verify build_hdr itself does not silently drop the flag. */
    const char *raw = "From: X\r\nSubject: Archived\r\nDate: Mon, 14 Apr 2026 08:00:00 +0000\r\n\r\n";
    char *labels[] = {"UNREAD", "CATEGORY_PROMOTIONS"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 2);
    ASSERT(hdr != NULL, "build_hdr archive+unread: not NULL");

    const char *last_tab = strrchr(hdr, '\t');
    int flags = atoi(last_tab + 1);
    /* build_hdr sets UNSEEN; the caller must clear it for _nolabel messages */
    ASSERT((flags & 1) != 0, "build_hdr archive+unread: UNSEEN set by build_hdr (caller clears it)");

    free(hdr);
}

/* ── Registration ────────────────────────────────────────────────────── */

void test_gmail_sync(void) {
    RUN_TEST(test_filtered_null);
    RUN_TEST(test_filtered_category);
    RUN_TEST(test_filtered_important);
    RUN_TEST(test_not_filtered_system);
    RUN_TEST(test_not_filtered_user);
    RUN_TEST(test_filtered_edge_cases);
    RUN_TEST(test_build_hdr_basic);
    RUN_TEST(test_build_hdr_starred);
    RUN_TEST(test_build_hdr_no_labels);
    RUN_TEST(test_build_hdr_missing_headers);
    RUN_TEST(test_build_hdr_combined_flags);
    RUN_TEST(test_build_hdr_archive_unread_flags);
    RUN_TEST(test_incremental_no_history);
    RUN_TEST(test_repair_archive_flags_clears_unseen);
    RUN_TEST(test_repair_archive_flags_preserves_flagged);
    RUN_TEST(test_repair_archive_flags_noop_when_already_read);
}
