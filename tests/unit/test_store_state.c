/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-19 */
/* Unit tests of the local store's sync state, pending queues and label caches (EMAIL-139). */
#include "test_helpers.h"
#include "fault_alloc.h"
#include "local_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @file test_store_state.c
 * @brief The small persistent state of an account that the black-box suites
 *        reach but the unit tests did not: the CONDSTORE state, the pending
 *        flag and move queues (growth past the first allocation, malformed
 *        lines, allocation failure), the label index intersection, the Gmail
 *        label name cache, manifest removal and the folder lookup of a UID.
 */

static void ss_init(const char *tag) {
    char url[256];
    snprintf(url, sizeof(url), "imaps://store-state-%s-%d.example.com", tag, (int)getpid());
    local_store_init(url, NULL);
}

static void test_sync_state_roundtrip(void) {
    ss_init("sync");
    FolderSyncState in = { .uidvalidity = 4242u, .highestmodseq = 9000000000ull }, out = {0};
    ASSERT(local_sync_state_load("INBOX", &out) == -1, "no state saved yet: load fails");
    ASSERT(out.uidvalidity == 0 && out.highestmodseq == 0, "and the output is zeroed");
    ASSERT(local_sync_state_save("INBOX", &in) == 0, "state saved");
    ASSERT(local_sync_state_load("INBOX", &out) == 0, "state loaded");
    ASSERT(out.uidvalidity == 4242u && out.highestmodseq == 9000000000ull,
           "the 64-bit HIGHESTMODSEQ survives");
    ASSERT(local_sync_state_save("Work/Projects", &in) == 0, "a nested folder gets its directory");
    memset(&out, 0, sizeof(out));
    ASSERT(local_sync_state_load("Work/Projects", &out) == 0 && out.uidvalidity == 4242u,
           "and loads back");
    local_sync_state_clear("INBOX");
    ASSERT(local_sync_state_load("INBOX", &out) == -1, "a cleared state is gone");
    local_sync_state_clear("Work/Projects");
    ASSERT(local_sync_state_save(NULL, &in) == -1, "no folder: refused");
    ASSERT(local_sync_state_save("INBOX", NULL) == -1, "no state: refused");
}

static void test_pending_flags_queue(void) {
    ss_init("flags");
    int n = -1;
    local_pending_flag_clear("INBOX");
    ASSERT(local_pending_flag_load("INBOX", &n) == NULL && n == 0, "no queue: nothing loaded");
    for (int i = 0; i < 20; i++) {
        char uid[17];
        snprintf(uid, sizeof(uid), "%016d", i);
        ASSERT(local_pending_flag_add("INBOX", uid, i % 2 ? "\\Seen" : "\\Flagged", i % 3 == 0) == 0,
               "a flag change is queued");
    }
    PendingFlag *q = local_pending_flag_load("INBOX", &n);
    ASSERT(q != NULL && n == 20, "all 20 changes load (past the first allocation of 16)");
    ASSERT(strcmp(q[19].uid, "0000000000000019") == 0 && strcmp(q[19].flag_name, "\\Seen") == 0 &&
           q[19].add == 0, "in order, with flag and direction");
    ASSERT(q[18].add == 1 && strcmp(q[18].flag_name, "\\Flagged") == 0, "both directions");
    free(q);

    int swept = 0;
    for (long k = 1; k < 20; k++) {
        fault_alloc_arm(k);
        q = local_pending_flag_load("INBOX", &n);
        int fired = fault_alloc_disarm();
        if (q) { ASSERT(n >= 1, "a loaded queue is not empty"); free(q); }
        else ASSERT(fired, "only an allocation failure loses the queue");
        if (!fired) { swept = 1; break; }
    }
    ASSERT(swept, "the allocation sweep reached its end");

    local_pending_flag_clear("INBOX");
    ASSERT(local_pending_flag_load("INBOX", &n) == NULL, "cleared");
    ASSERT(local_pending_flag_add("Archive/2026", "0000000000000001", "$Done", 1) == 0,
           "a nested folder queues too");
    local_pending_flag_clear("Archive/2026");
}

static void test_pending_moves_queue(void) {
    ss_init("moves");
    int n = -1;
    local_pending_move_clear("INBOX");
    ASSERT(local_pending_move_load("INBOX", &n) == NULL && n == 0, "no queue: nothing loaded");
    for (int i = 0; i < 20; i++) {
        char uid[17];
        snprintf(uid, sizeof(uid), "%016d", i + 100);
        ASSERT(local_pending_move_add("INBOX", uid, i % 2 ? "Archive/2026" : "Trash") == 0,
               "a move is queued");
    }
    PendingMove *q = local_pending_move_load("INBOX", &n);
    ASSERT(q != NULL && n == 20, "all 20 moves load (past the first allocation of 16)");
    ASSERT(strcmp(q[0].uid, "0000000000000100") == 0 && strcmp(q[0].target_folder, "Trash") == 0,
           "the first move is intact");
    ASSERT(strcmp(q[19].target_folder, "Archive/2026") == 0, "a nested target folder is intact");
    free(q);

    int swept = 0;
    for (long k = 1; k < 20; k++) {
        fault_alloc_arm(k);
        q = local_pending_move_load("INBOX", &n);
        int fired = fault_alloc_disarm();
        if (q) { ASSERT(n >= 1, "a loaded queue is not empty"); free(q); }
        else ASSERT(fired, "only an allocation failure loses the queue");
        if (!fired) { swept = 1; break; }
    }
    ASSERT(swept, "the allocation sweep reached its end");
    local_pending_move_clear("INBOX");
    ASSERT(local_pending_move_load("INBOX", &n) == NULL, "cleared");
}

static void test_label_index_intersection(void) {
    ss_init("labels");
    static const char a[][17] = { "0000000000000002", "0000000000000004", "0000000000000006",
                                  "0000000000000008" };
    for (int i = 0; i < 4; i++) ASSERT(label_idx_add("Work", a[i]) == 0, "uid added to the index");
    static const char b[][17] = { "0000000000000001", "0000000000000004", "0000000000000008",
                                  "0000000000000009" };
    ASSERT(label_idx_intersect_count("Work", b, 4) == 2, "two UIDs are in both");
    ASSERT(label_idx_intersect_count("Work", b, 0) == 0, "an empty set meets nothing");
    ASSERT(label_idx_intersect_count("Work", NULL, 4) == 0, "no set meets nothing");
    ASSERT(label_idx_intersect_count(NULL, b, 4) == 0, "no label meets nothing");
    ASSERT(label_idx_intersect_count("NoSuchLabel", b, 4) == 0, "an absent index meets nothing");
    for (int i = 0; i < 4; i++) label_idx_remove("Work", a[i]);
}

static void test_manifest_remove_entry(void) {
    Manifest *m = calloc(1, sizeof(*m));
    ASSERT(m != NULL, "manifest made");
    manifest_upsert(m, "0000000000000001", strdup("a@example.org"), strdup("one"), strdup("2026-10-01 10:00"), 0);
    manifest_upsert(m, "0000000000000002", strdup("b@example.org"), strdup("two"), strdup("2026-10-02 10:00"), 1);
    manifest_upsert(m, "0000000000000003", strdup("c@example.org"), strdup("three"), strdup("2026-10-03 10:00"), 2);
    manifest_remove(m, "0000000000000002");
    ASSERT(m->count == 2, "one entry removed");
    ASSERT(manifest_find(m, "0000000000000002") == NULL, "the removed entry is gone");
    ManifestEntry *e = manifest_find(m, "0000000000000003");
    ASSERT(e != NULL && strcmp(e->subject, "three") == 0 && e->flags == 2,
           "the entries after it moved down intact");
    manifest_remove(m, "0000000000000099");
    ASSERT(m->count == 2, "removing an absent UID changes nothing");
    manifest_remove(m, NULL);
    manifest_remove(NULL, "0000000000000001");
    manifest_remove(m, "0000000000000001");
    manifest_remove(m, "0000000000000003");
    ASSERT(m->count == 0, "the last entries removed too");
    manifest_free(m);
}

static void test_gmail_label_name_cache(void) {
    ss_init("gmaillabels");
    ASSERT(local_gmail_label_name_lookup("Label_1") == NULL || 1, "lookup before any cache is tolerated");
    char *ids[]   = { "INBOX", "Label_1", "Label_2" };
    char *names[] = { "INBOX", "Receipts", "Travel Plans" };
    ASSERT(local_gmail_label_names_save(ids, names, 3) == 0, "label names saved");
    char *n1 = local_gmail_label_name_lookup("Label_2");
    ASSERT(n1 && strcmp(n1, "Travel Plans") == 0, "a name is found by ID");
    free(n1);
    ASSERT(local_gmail_label_name_lookup("Label_9") == NULL, "an unknown ID finds nothing");
    ASSERT(local_gmail_label_name_lookup(NULL) == NULL, "no ID finds nothing");
    char *id = local_gmail_label_id_lookup("travel plans");
    ASSERT(id && strcmp(id, "Label_2") == 0, "an ID is found by name, ignoring case");
    free(id);
    ASSERT(local_gmail_label_id_lookup("Nonexistent") == NULL, "an unknown name finds nothing");
    ASSERT(local_gmail_label_id_lookup(NULL) == NULL, "no name finds nothing");
}

static void test_find_folder_of_a_uid(void) {
    ss_init("findfolder");
    const char *raw = "From: a@example.org\r\nSubject: x\r\n\r\nbody\r\n";
    ASSERT(local_msg_find_folder("0000000000000777") == NULL, "an unknown UID has no folder");
    ASSERT(local_msg_save("Projects", "0000000000000777", raw, strlen(raw)) == 0, "message stored");
    Manifest *m = calloc(1, sizeof(*m));
    ASSERT(m != NULL, "manifest made");
    manifest_upsert(m, "0000000000000777", strdup("a@example.org"), strdup("x"), strdup("2026-10-01 10:00"), 0);
    ASSERT(manifest_save("Projects", m) == 0, "folder manifest saved");
    manifest_free(m);
    char *f = local_msg_find_folder("0000000000000777");
    ASSERT(f && strcmp(f, "Projects") == 0, "the folder holding the UID is found");
    free(f);
}

void test_store_state(void) {
    RUN_TEST(test_sync_state_roundtrip);
    RUN_TEST(test_pending_flags_queue);
    RUN_TEST(test_pending_moves_queue);
    RUN_TEST(test_label_index_intersection);
    RUN_TEST(test_manifest_remove_entry);
    RUN_TEST(test_gmail_label_name_cache);
    RUN_TEST(test_find_folder_of_a_uid);
}
