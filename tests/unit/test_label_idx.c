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

/* ── local_hdr_get_labels tests (#26) ─────────────────────────────── */

static void test_hdr_get_labels_normal(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://hdr-labels-%d.example.com", getpid());
    local_store_init(url, NULL);

    /* Save a Gmail-style .hdr: from\tsubject\tdate\tlabels\tflags */
    const char *hdr = "Alice\tHello\t2026-04-17 10:00\tINBOX,STARRED,Work\t3";
    local_hdr_save("", "18c9b46d67a60001", hdr, strlen(hdr));

    char *labels = local_hdr_get_labels("", "18c9b46d67a60001");
    ASSERT(labels != NULL, "hdr_get_labels: not NULL");
    ASSERT(strcmp(labels, "INBOX,STARRED,Work") == 0, "hdr_get_labels: correct value");
    free(labels);
}

static void test_hdr_get_labels_missing(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://hdr-labels-miss-%d.example.com", getpid());
    local_store_init(url, NULL);

    char *labels = local_hdr_get_labels("", "0000000000000099");
    ASSERT(labels == NULL, "hdr_get_labels missing: NULL");
}

static void test_hdr_get_labels_empty(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://hdr-labels-empty-%d.example.com", getpid());
    local_store_init(url, NULL);

    /* Empty labels field */
    const char *hdr = "Bob\tSubj\t2026-04-17\t\t0";
    local_hdr_save("", "18c9b46d67a60002", hdr, strlen(hdr));

    char *labels = local_hdr_get_labels("", "18c9b46d67a60002");
    ASSERT(labels != NULL, "hdr_get_labels empty: not NULL");
    ASSERT(labels[0] == '\0', "hdr_get_labels empty: empty string");
    free(labels);
}

static void test_hdr_get_labels_single(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://hdr-labels-single-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *hdr = "Carol\tTest\t2026-04-17\tINBOX\t1";
    local_hdr_save("", "18c9b46d67a60003", hdr, strlen(hdr));

    char *labels = local_hdr_get_labels("", "18c9b46d67a60003");
    ASSERT(labels != NULL && strcmp(labels, "INBOX") == 0,
           "hdr_get_labels single: INBOX");
    free(labels);
}

static void test_hdr_get_labels_many(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://hdr-labels-many-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *hdr = "Dave\tMulti\t2026-04-17\tINBOX,UNREAD,STARRED,Work,Personal\t3";
    local_hdr_save("", "18c9b46d67a60004", hdr, strlen(hdr));

    char *labels = local_hdr_get_labels("", "18c9b46d67a60004");
    ASSERT(labels != NULL, "hdr_get_labels many: not NULL");
    ASSERT(strstr(labels, "INBOX") != NULL, "hdr_get_labels many: has INBOX");
    ASSERT(strstr(labels, "Work") != NULL, "hdr_get_labels many: has Work");
    ASSERT(strstr(labels, "Personal") != NULL, "hdr_get_labels many: has Personal");
    free(labels);
}

/* ── Archive / Trash label operations (#30) ──────────────────────── */

static void test_archive_removes_inbox(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://archive-test-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *uid = "18c9b46d67a6a001";
    label_idx_add("INBOX", uid);
    label_idx_add("Work", uid);
    ASSERT(label_idx_contains("INBOX", uid) == 1, "archive pre: in INBOX");

    /* Simulate archive: remove INBOX */
    label_idx_remove("INBOX", uid);
    ASSERT(label_idx_contains("INBOX", uid) == 0, "archive: removed from INBOX");
    ASSERT(label_idx_contains("Work", uid) == 1, "archive: Work preserved");
}

static void test_archive_nolabel_when_no_labels(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://archive-nolabel-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *uid = "18c9b46d67a6a002";
    label_idx_add("INBOX", uid);

    /* Archive: remove INBOX, no other labels → should go to _nolabel */
    label_idx_remove("INBOX", uid);
    /* Check: not in INBOX or any other label → add to _nolabel */
    label_idx_add("_nolabel", uid);
    ASSERT(label_idx_contains("_nolabel", uid) == 1, "archive nolabel: in _nolabel");
}

static void test_trash_removes_all_labels(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://trash-test-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *uid = "18c9b46d67a6a003";
    label_idx_add("INBOX", uid);
    label_idx_add("Work", uid);
    label_idx_add("STARRED", uid);

    /* Simulate trash: remove from all, add to _trash */
    char **all_labels = NULL;
    int all_count = 0;
    label_idx_list(&all_labels, &all_count);
    for (int i = 0; i < all_count; i++) {
        label_idx_remove(all_labels[i], uid);
        free(all_labels[i]);
    }
    free(all_labels);
    label_idx_add("_trash", uid);

    ASSERT(label_idx_contains("INBOX", uid) == 0, "trash: removed from INBOX");
    ASSERT(label_idx_contains("Work", uid) == 0, "trash: removed from Work");
    ASSERT(label_idx_contains("STARRED", uid) == 0, "trash: removed from STARRED");
    ASSERT(label_idx_contains("_trash", uid) == 1, "trash: in _trash");
}

/* ── Label picker toggle logic (#31) ─────────────────────────────── */

static void test_label_toggle_add_remove(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://label-toggle-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *uid = "18c9b46d67a6b001";

    /* Toggle ON: add label */
    label_idx_add("Work", uid);
    ASSERT(label_idx_contains("Work", uid) == 1, "toggle on: added");

    /* Toggle OFF: remove label */
    label_idx_remove("Work", uid);
    ASSERT(label_idx_contains("Work", uid) == 0, "toggle off: removed");

    /* Double add is no-op */
    label_idx_add("Work", uid);
    label_idx_add("Work", uid);
    ASSERT(label_idx_count("Work") == 1, "toggle: double add → count=1");

    /* Double remove is no-op */
    label_idx_remove("Work", uid);
    label_idx_remove("Work", uid);
    ASSERT(label_idx_count("Work") == 0, "toggle: double remove → count=0");
}

/* ── Trash label backup/restore (#25) ────────────────────────────── */

static void test_trash_labels_save_load(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://trash-lbl-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *uid = "18c9b46d67a6c001";
    ASSERT(local_trash_labels_load(uid) == NULL, "trash labels: initially NULL");

    ASSERT(local_trash_labels_save(uid, "INBOX,Work,STARRED") == 0,
           "trash labels: save ok");
    char *loaded = local_trash_labels_load(uid);
    ASSERT(loaded != NULL, "trash labels: load not NULL");
    ASSERT(strcmp(loaded, "INBOX,Work,STARRED") == 0, "trash labels: content matches");
    free(loaded);

    /* Remove */
    local_trash_labels_remove(uid);
    ASSERT(local_trash_labels_load(uid) == NULL, "trash labels: removed");
}

static void test_trash_restore_flow(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://trash-flow-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *uid = "18c9b46d67a6c002";

    /* Pre-trash state: message in INBOX + Work */
    label_idx_add("INBOX", uid);
    label_idx_add("Work", uid);

    /* Save labels before trash */
    local_trash_labels_save(uid, "INBOX,Work,UNREAD");

    /* Trash: remove all labels, add _trash */
    label_idx_remove("INBOX", uid);
    label_idx_remove("Work", uid);
    label_idx_add("_trash", uid);

    ASSERT(label_idx_contains("INBOX", uid) == 0, "trash flow: not in INBOX");
    ASSERT(label_idx_contains("_trash", uid) == 1, "trash flow: in _trash");

    /* Untrash: restore saved labels (skip UNREAD) */
    label_idx_remove("_trash", uid);
    char *saved = local_trash_labels_load(uid);
    ASSERT(saved != NULL, "trash flow: saved labels exist");
    /* Parse and restore */
    char *tok = saved, *sep;
    while (tok && *tok) {
        sep = strchr(tok, ',');
        size_t tl = sep ? (size_t)(sep - tok) : strlen(tok);
        char lb[64];
        if (tl >= sizeof(lb)) tl = sizeof(lb) - 1;
        memcpy(lb, tok, tl); lb[tl] = '\0';
        if (strcmp(lb, "UNREAD") != 0)
            label_idx_add(lb, uid);
        tok = sep ? sep + 1 : NULL;
    }
    free(saved);
    local_trash_labels_remove(uid);

    ASSERT(label_idx_contains("INBOX", uid) == 1, "untrash: back in INBOX");
    ASSERT(label_idx_contains("Work", uid) == 1, "untrash: back in Work");
    ASSERT(label_idx_contains("_trash", uid) == 0, "untrash: not in _trash");
    ASSERT(local_trash_labels_load(uid) == NULL, "untrash: backup removed");
}

/* ── local_hdr_update_flags (#37) ─────────────────────────────────── */

static void test_hdr_update_flags_basic(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://hdr-updflags-%d.example.com", getpid());
    local_store_init(url, NULL);

    /* Save a .hdr with flags=3 (UNSEEN|FLAGGED) */
    const char *hdr = "Alice\tHello\t2026-04-18\tINBOX,UNREAD,STARRED\t3";
    local_hdr_save("", "18c9b46d67a6f001", hdr, strlen(hdr));

    /* Update flags to 0 (mark as read + unstar) */
    ASSERT(local_hdr_update_flags("", "18c9b46d67a6f001", 0) == 0,
           "hdr_update_flags: rc=0");

    char *loaded = local_hdr_load("", "18c9b46d67a6f001");
    ASSERT(loaded != NULL, "hdr_update_flags: load ok");

    /* Verify the last tab field is now "0" */
    char *last_tab = strrchr(loaded, '\t');
    ASSERT(last_tab != NULL, "hdr_update_flags: has tab");
    ASSERT(atoi(last_tab + 1) == 0, "hdr_update_flags: flags=0");

    /* Verify the rest is unchanged */
    ASSERT(strstr(loaded, "Alice") != NULL, "hdr_update_flags: from preserved");
    ASSERT(strstr(loaded, "Hello") != NULL, "hdr_update_flags: subject preserved");
    free(loaded);
}

static void test_hdr_update_flags_toggle_unseen(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://hdr-toggle-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *uid = "18c9b46d67a6f002";
    const char *hdr = "Bob\tTest\t2026-04-18\tINBOX,UNREAD\t1";
    local_hdr_save("", uid, hdr, strlen(hdr));

    /* Mark as read: flags 1→0 */
    local_hdr_update_flags("", uid, 0);
    char *h1 = local_hdr_load("", uid);
    char *t1 = strrchr(h1, '\t');
    ASSERT(atoi(t1 + 1) == 0, "toggle unseen: now read (flags=0)");
    free(h1);

    /* Mark as unread again: flags 0→1 */
    local_hdr_update_flags("", uid, 1);
    char *h2 = local_hdr_load("", uid);
    char *t2 = strrchr(h2, '\t');
    ASSERT(atoi(t2 + 1) == 1, "toggle unseen: back to unread (flags=1)");
    free(h2);
}

static void test_hdr_update_flags_nonexistent(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://hdr-noexist-%d.example.com", getpid());
    local_store_init(url, NULL);

    ASSERT(local_hdr_update_flags("", "0000000000000000", 5) == -1,
           "hdr_update_flags nonexistent: returns -1");
}

/* ── Gmail flag toggle end-to-end (#37) ──────────────────────────── */

static void test_gmail_flag_toggle_unread(void) {
    /* Simulates what email_service does when user presses 'n' on Gmail */
    char url[256];
    snprintf(url, sizeof(url), "imaps://gmail-ftoggle-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *uid = "18c9b46d67a6e001";

    /* Initial state: message is in INBOX + UNREAD, flags=1 (UNSEEN) */
    const char *hdr = "Alice\tHello\t2026-04-18\tINBOX,UNREAD\t1";
    local_hdr_save("", uid, hdr, strlen(hdr));
    label_idx_add("INBOX", uid);
    label_idx_add("UNREAD", uid);

    ASSERT(label_idx_count("UNREAD") == 1, "toggle pre: UNREAD count=1");

    /* === User presses 'n' → mark as read === */
    /* 1. Update label index */
    label_idx_remove("UNREAD", uid);
    /* 2. Update .hdr flags (remove UNSEEN bit) */
    local_hdr_update_flags("", uid, 0);

    /* Verify: UNREAD count decreased */
    ASSERT(label_idx_count("UNREAD") == 0, "toggle read: UNREAD count=0");
    ASSERT(label_idx_contains("UNREAD", uid) == 0, "toggle read: not in UNREAD");
    /* Still in INBOX */
    ASSERT(label_idx_contains("INBOX", uid) == 1, "toggle read: still in INBOX");
    /* .hdr flags updated */
    char *h = local_hdr_load("", uid);
    char *lt = strrchr(h, '\t');
    ASSERT(atoi(lt + 1) == 0, "toggle read: .hdr flags=0");
    free(h);

    /* === User presses 'n' again → mark as unread === */
    label_idx_add("UNREAD", uid);
    local_hdr_update_flags("", uid, 1);

    ASSERT(label_idx_count("UNREAD") == 1, "toggle unread: UNREAD count=1");
    ASSERT(label_idx_contains("UNREAD", uid) == 1, "toggle unread: in UNREAD");
    h = local_hdr_load("", uid);
    lt = strrchr(h, '\t');
    ASSERT(atoi(lt + 1) == 1, "toggle unread: .hdr flags=1");
    free(h);
}

static void test_gmail_flag_toggle_starred(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://gmail-fstar-%d.example.com", getpid());
    local_store_init(url, NULL);

    const char *uid = "18c9b46d67a6e002";

    /* Initial: not starred, flags=0 */
    const char *hdr = "Bob\tTest\t2026-04-18\tINBOX\t0";
    local_hdr_save("", uid, hdr, strlen(hdr));
    label_idx_add("INBOX", uid);

    ASSERT(label_idx_count("STARRED") == 0, "star pre: STARRED count=0");

    /* User presses 'f' → add star */
    label_idx_add("STARRED", uid);
    local_hdr_update_flags("", uid, 2);  /* MSG_FLAG_FLAGGED = 2 */

    ASSERT(label_idx_count("STARRED") == 1, "star on: STARRED count=1");
    ASSERT(label_idx_contains("STARRED", uid) == 1, "star on: in STARRED");

    /* User presses 'f' again → remove star */
    label_idx_remove("STARRED", uid);
    local_hdr_update_flags("", uid, 0);

    ASSERT(label_idx_count("STARRED") == 0, "star off: STARRED count=0");
    ASSERT(label_idx_contains("STARRED", uid) == 0, "star off: not in STARRED");
}

static void test_gmail_flag_toggle_multiple_msgs(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://gmail-fmulti-%d.example.com", getpid());
    local_store_init(url, NULL);

    /* 3 unread messages */
    for (int i = 1; i <= 3; i++) {
        char uid[17];
        snprintf(uid, sizeof(uid), "18c9b46d67a6d%03d", i);
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "User%d\tMsg%d\t2026-04-18\tINBOX,UNREAD\t1", i, i);
        local_hdr_save("", uid, hdr, strlen(hdr));
        label_idx_add("INBOX", uid);
        label_idx_add("UNREAD", uid);
    }

    ASSERT(label_idx_count("UNREAD") == 3, "multi pre: UNREAD=3");

    /* Mark first message as read */
    label_idx_remove("UNREAD", "18c9b46d67a6d001");
    local_hdr_update_flags("", "18c9b46d67a6d001", 0);
    ASSERT(label_idx_count("UNREAD") == 2, "multi: UNREAD=2 after one read");

    /* Mark second as read */
    label_idx_remove("UNREAD", "18c9b46d67a6d002");
    local_hdr_update_flags("", "18c9b46d67a6d002", 0);
    ASSERT(label_idx_count("UNREAD") == 1, "multi: UNREAD=1 after two read");

    /* Mark first as unread again */
    label_idx_add("UNREAD", "18c9b46d67a6d001");
    local_hdr_update_flags("", "18c9b46d67a6d001", 1);
    ASSERT(label_idx_count("UNREAD") == 2, "multi: UNREAD=2 after re-unread");
}

/* ── Short UID regression (GML-short-id) ─────────────────────────── */

/*
 * Regression test: Gmail message IDs can be shorter than 16 characters
 * (e.g. 13, 14, 15 hex chars).  The old label_idx_write used "%.16s\n"
 * which produced variable-length records; the fixed-size fread then read
 * the newline into the UID, embedding "\n" in the string and breaking
 * URL construction.
 *
 * The fix: label_idx_write always writes exactly 16 NUL-padded bytes +
 * '\n' = 17 bytes per record; label_idx_load uses fgets which handles
 * both old (variable) and new (fixed) formats transparently.
 */
static void test_label_idx_short_ids(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://labelidx-short-%d.example.com", getpid());
    local_store_init(url, NULL);

    /* IDs from real-world Gmail error report (13–15 chars) */
    ASSERT(label_idx_add("INBOX", "d99192cdf1df3")   == 0, "add 13-char id");
    ASSERT(label_idx_add("INBOX", "db185943f7560a")  == 0, "add 14-char id");
    ASSERT(label_idx_add("INBOX", "e169e066dd2f3ee") == 0, "add 15-char id");
    ASSERT(label_idx_add("INBOX", "18c9b46d67a61234")== 0, "add 16-char id");

    ASSERT(label_idx_count("INBOX") == 4, "short ids: count=4");

    /* contains() must find each ID — no embedded newlines in stored keys */
    ASSERT(label_idx_contains("INBOX", "d99192cdf1df3")   == 1, "contains 13-char");
    ASSERT(label_idx_contains("INBOX", "db185943f7560a")  == 1, "contains 14-char");
    ASSERT(label_idx_contains("INBOX", "e169e066dd2f3ee") == 1, "contains 15-char");
    ASSERT(label_idx_contains("INBOX", "18c9b46d67a61234")== 1, "contains 16-char");

    /* Load and verify no embedded newlines or CR in any stored UID */
    char (*uids)[17] = NULL;
    int count = 0;
    ASSERT(label_idx_load("INBOX", &uids, &count) == 0, "load short ids ok");
    ASSERT(count == 4, "load count=4");
    int clean = 1;
    for (int i = 0; i < count; i++) {
        if (strchr(uids[i], '\n') || strchr(uids[i], '\r'))
            clean = 0;
    }
    ASSERT(clean, "no embedded newlines in loaded short IDs");
    free(uids);

    /* Remove and re-add cycle must work for short IDs */
    ASSERT(label_idx_remove("INBOX", "db185943f7560a") == 0, "remove 14-char ok");
    ASSERT(label_idx_contains("INBOX", "db185943f7560a") == 0, "14-char gone");
    ASSERT(label_idx_count("INBOX") == 3, "count=3 after remove");

    ASSERT(label_idx_add("INBOX", "db185943f7560a") == 0, "re-add 14-char ok");
    ASSERT(label_idx_contains("INBOX", "db185943f7560a") == 1, "14-char back");
    ASSERT(label_idx_count("INBOX") == 4, "count=4 after re-add");
}

/*
 * Verify that a label index file written in the OLD variable-length format
 * (where short IDs produced records shorter than 17 bytes) is read back
 * correctly by the new fgets-based loader.
 */
static void test_label_idx_old_format_compat(void) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://labelidx-compat-%d.example.com", getpid());
    local_store_init(url, NULL);

    /* Seed two 16-char IDs in new format first (establishes the labels/ dir) */
    label_idx_add("MIGR", "aaaa000000000001");
    label_idx_add("MIGR", "aaaa000000000002");

    /* Overwrite with an old-format file: variable-length lines (no NUL padding).
     * We do this by locating the .idx file via the account base.
     * label_idx_write path: <data>/email-cli/accounts/imap.<host>/labels/MIGR.idx */
    const char *home = getenv("HOME");
    ASSERT(home != NULL, "compat: HOME env set");
    char idxpath[2048];
    snprintf(idxpath, sizeof(idxpath),
             "%s/.local/share/email-cli/accounts/"
             "imap.labelidx-compat-%d.example.com/labels/MIGR.idx",
             home, getpid());

    /* Write old-format file manually: variable-length lines (no NUL padding),
     * in sorted order (old code also kept files sorted via label_idx_add). */
    FILE *fp = fopen(idxpath, "w");
    ASSERT(fp != NULL, "compat: open idx for write");
    /* 16-char ID: 16 bytes + '\n' = 17 bytes (same as new format for full IDs) */
    fputs("18c9b46d67a61234\n", fp);
    /* 13-char ID: 13 bytes + '\n' = 14 bytes (old broken record) */
    fputs("d99192cdf1df3\n", fp);
    /* 15-char ID: 15 bytes + '\n' = 16 bytes (old broken record) */
    fputs("e169e066dd2f3ee\n", fp);
    fclose(fp);

    /* Load must produce 3 clean UIDs */
    char (*uids)[17] = NULL;
    int count = 0;
    ASSERT(label_idx_load("MIGR", &uids, &count) == 0, "compat: load ok");
    ASSERT(count == 3, "compat: count=3");
    int clean = 1;
    for (int i = 0; i < count; i++) {
        if (strchr(uids[i], '\n') || strchr(uids[i], '\r'))
            clean = 0;
    }
    ASSERT(clean, "compat: no embedded newlines after migration");
    free(uids);

    /* contains() must work after migration */
    ASSERT(label_idx_contains("MIGR", "e169e066dd2f3ee") == 1, "compat: 15-char found");
    ASSERT(label_idx_contains("MIGR", "d99192cdf1df3")   == 1, "compat: 13-char found");
    ASSERT(label_idx_contains("MIGR", "18c9b46d67a61234")== 1, "compat: 16-char found");
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
    RUN_TEST(test_hdr_get_labels_normal);
    RUN_TEST(test_hdr_get_labels_missing);
    RUN_TEST(test_hdr_get_labels_empty);
    RUN_TEST(test_hdr_get_labels_single);
    RUN_TEST(test_hdr_get_labels_many);
    RUN_TEST(test_archive_removes_inbox);
    RUN_TEST(test_archive_nolabel_when_no_labels);
    RUN_TEST(test_trash_removes_all_labels);
    RUN_TEST(test_label_toggle_add_remove);
    RUN_TEST(test_trash_labels_save_load);
    RUN_TEST(test_trash_restore_flow);
    RUN_TEST(test_hdr_update_flags_basic);
    RUN_TEST(test_hdr_update_flags_toggle_unseen);
    RUN_TEST(test_hdr_update_flags_nonexistent);
    RUN_TEST(test_gmail_flag_toggle_unread);
    RUN_TEST(test_gmail_flag_toggle_starred);
    RUN_TEST(test_gmail_flag_toggle_multiple_msgs);
    RUN_TEST(test_label_idx_short_ids);
    RUN_TEST(test_label_idx_old_format_compat);
}
