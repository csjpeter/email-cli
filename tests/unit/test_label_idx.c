#include "test_helpers.h"
#include "local_store.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

/*
 * These tests use a temporary directory as the account base.
 * local_store_init() with a dummy URL sets g_account_base.
 */

/* ── Tests ────────────────────────────────────────────────────────── */

static void test_label_idx_empty(void) {
    /* Re-init local store with a test directory */
    char url[256];
    snprintf(url, sizeof(url), "imaps://labelidx-test-%d.example.com", getpid());
    local_store_init(url, NULL);

    /* No .idx file exists → contains returns 0, count returns 0 */
    ASSERT(label_idx_contains("INBOX", "18c9b46d67a6123f") == 0,
           "empty: contains returns 0");
    ASSERT(label_idx_count("INBOX") == 0, "empty: count returns 0");

    /* Load empty → returns 0 count, no error */
    char (*uids)[17] = NULL;
    int count = 0;
    ASSERT(label_idx_load("INBOX", &uids, &count) == 0, "empty: load ok");
    ASSERT(count == 0, "empty: load count=0");
    free(uids);
}

static void test_label_idx_add_and_contains(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://labelidx-add-%d.example.com", getpid());
    local_store_init(url, NULL);

    ASSERT(label_idx_add("INBOX", "0000000000000003") == 0, "add uid3 ok");
    ASSERT(label_idx_add("INBOX", "0000000000000001") == 0, "add uid1 ok");
    ASSERT(label_idx_add("INBOX", "0000000000000005") == 0, "add uid5 ok");
    ASSERT(label_idx_add("INBOX", "0000000000000002") == 0, "add uid2 ok");
    ASSERT(label_idx_add("INBOX", "0000000000000004") == 0, "add uid4 ok");

    /* Duplicate add should be a no-op */
    ASSERT(label_idx_add("INBOX", "0000000000000003") == 0, "add dup ok");

    ASSERT(label_idx_count("INBOX") == 5, "count=5");
    ASSERT(label_idx_contains("INBOX", "0000000000000001") == 1, "contains 1");
    ASSERT(label_idx_contains("INBOX", "0000000000000003") == 1, "contains 3");
    ASSERT(label_idx_contains("INBOX", "0000000000000005") == 1, "contains 5");
    ASSERT(label_idx_contains("INBOX", "0000000000000006") == 0, "not contains 6");
}

static void test_label_idx_remove(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://labelidx-rm-%d.example.com", getpid());
    local_store_init(url, NULL);

    label_idx_add("TEST", "aaaa000000000001");
    label_idx_add("TEST", "aaaa000000000002");
    label_idx_add("TEST", "aaaa000000000003");

    ASSERT(label_idx_count("TEST") == 3, "before remove: 3");

    ASSERT(label_idx_remove("TEST", "aaaa000000000002") == 0, "remove mid ok");
    ASSERT(label_idx_count("TEST") == 2, "after remove: 2");
    ASSERT(label_idx_contains("TEST", "aaaa000000000002") == 0, "removed uid gone");
    ASSERT(label_idx_contains("TEST", "aaaa000000000001") == 1, "uid1 still there");
    ASSERT(label_idx_contains("TEST", "aaaa000000000003") == 1, "uid3 still there");

    /* Remove non-existent → no-op */
    ASSERT(label_idx_remove("TEST", "aaaa000000000009") == 0, "remove nonexist ok");
    ASSERT(label_idx_count("TEST") == 2, "still 2 after noop remove");
}

static void test_label_idx_load(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://labelidx-load-%d.example.com", getpid());
    local_store_init(url, NULL);

    label_idx_add("STARRED", "bbbb000000000010");
    label_idx_add("STARRED", "bbbb000000000005");
    label_idx_add("STARRED", "bbbb000000000020");

    char (*uids)[17] = NULL;
    int count = 0;
    ASSERT(label_idx_load("STARRED", &uids, &count) == 0, "load ok");
    ASSERT(count == 3, "load count=3");

    /* Verify sorted order */
    ASSERT(strcmp(uids[0], "bbbb000000000005") == 0, "sorted[0]=05");
    ASSERT(strcmp(uids[1], "bbbb000000000010") == 0, "sorted[1]=10");
    ASSERT(strcmp(uids[2], "bbbb000000000020") == 0, "sorted[2]=20");
    free(uids);
}

static void test_label_idx_write_bulk(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://labelidx-bulk-%d.example.com", getpid());
    local_store_init(url, NULL);

    char uids[4][17] = {
        "cccc000000000001",
        "cccc000000000002",
        "cccc000000000003",
        "cccc000000000004"
    };
    ASSERT(label_idx_write("BULK", (const char (*)[17])uids, 4) == 0, "write bulk ok");
    ASSERT(label_idx_count("BULK") == 4, "bulk count=4");
    ASSERT(label_idx_contains("BULK", "cccc000000000003") == 1, "bulk contains 3");
}

static void test_label_idx_hex_uids(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://labelidx-hex-%d.example.com", getpid());
    local_store_init(url, NULL);

    /* Gmail-style hex UIDs */
    label_idx_add("Work", "18c9b46d67a6123f");
    label_idx_add("Work", "18c9b46d67a60001");
    label_idx_add("Work", "18c9b46d67a6ffff");

    ASSERT(label_idx_count("Work") == 3, "hex count=3");
    ASSERT(label_idx_contains("Work", "18c9b46d67a6123f") == 1, "hex contains");
    ASSERT(label_idx_contains("Work", "18c9b46d67a60001") == 1, "hex contains min");
    ASSERT(label_idx_contains("Work", "18c9b46d67a6ffff") == 1, "hex contains max");
    ASSERT(label_idx_contains("Work", "18c9b46d67a60000") == 0, "hex not contains");

    /* Verify sorted order */
    char (*uids)[17] = NULL;
    int count = 0;
    label_idx_load("Work", &uids, &count);
    ASSERT(count == 3, "hex load 3");
    ASSERT(strcmp(uids[0], "18c9b46d67a60001") == 0, "hex sorted[0]");
    ASSERT(strcmp(uids[1], "18c9b46d67a6123f") == 0, "hex sorted[1]");
    ASSERT(strcmp(uids[2], "18c9b46d67a6ffff") == 0, "hex sorted[2]");
    free(uids);
}

static void test_gmail_history_id(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://labelidx-hist-%d.example.com", getpid());
    local_store_init(url, NULL);

    ASSERT(local_gmail_history_load() == NULL, "history: initially NULL");

    ASSERT(local_gmail_history_save("12345678") == 0, "history: save ok");
    char *hid = local_gmail_history_load();
    ASSERT(hid != NULL, "history: load not NULL");
    ASSERT(strcmp(hid, "12345678") == 0, "history: value matches");
    free(hid);

    /* Overwrite */
    ASSERT(local_gmail_history_save("99999999") == 0, "history: overwrite ok");
    hid = local_gmail_history_load();
    ASSERT(hid != NULL && strcmp(hid, "99999999") == 0, "history: overwritten value");
    free(hid);
}

static void test_label_idx_list(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://labelidx-list-%d.example.com", getpid());
    local_store_init(url, NULL);

    /* Create a few label indexes */
    label_idx_add("INBOX",   "0000000000000001");
    label_idx_add("SENT",    "0000000000000002");
    label_idx_add("Work",    "0000000000000003");
    label_idx_add("_nolabel","0000000000000004");

    char **labels = NULL;
    int count = 0;
    int rc = label_idx_list(&labels, &count);
    ASSERT(rc == 0, "label_idx_list: rc=0");
    ASSERT(count == 4, "label_idx_list: 4 labels");

    /* Check that all expected labels are present (order not guaranteed by readdir) */
    int found_inbox = 0, found_sent = 0, found_work = 0, found_nolabel = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(labels[i], "INBOX") == 0) found_inbox = 1;
        if (strcmp(labels[i], "SENT") == 0) found_sent = 1;
        if (strcmp(labels[i], "Work") == 0) found_work = 1;
        if (strcmp(labels[i], "_nolabel") == 0) found_nolabel = 1;
        free(labels[i]);
    }
    free(labels);
    ASSERT(found_inbox && found_sent && found_work && found_nolabel,
           "label_idx_list: all labels found");
}

/* ── Registration ─────────────────────────────────────────────────── */

void test_label_idx(void) {
    RUN_TEST(test_label_idx_empty);
    RUN_TEST(test_label_idx_add_and_contains);
    RUN_TEST(test_label_idx_remove);
    RUN_TEST(test_label_idx_load);
    RUN_TEST(test_label_idx_write_bulk);
    RUN_TEST(test_label_idx_hex_uids);
    RUN_TEST(test_gmail_history_id);
    RUN_TEST(test_label_idx_list);
}
