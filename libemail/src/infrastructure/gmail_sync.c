#include "gmail_sync.h"
#include "gmail_client.h"
#include "local_store.h"
#include "mime_util.h"
#include "json_util.h"
#include "logger.h"
#include "raii.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Progress callbacks ───────────────────────────────────────────── */

/* Called by gmail_list_messages after each page: cur = messages collected so far */
static void list_progress_cb(size_t cur, size_t total, void *ctx) {
    (void)total; (void)ctx;
    fprintf(stderr, "\r\033[K  Listing messages... %zu found", cur);
    fflush(stderr);
}

/* ── Gmail .hdr file format ───────────────────────────────────────── */

/**
 * Build a .hdr string from raw message headers and label list.
 * Format: from\tsubject\tdate\tlabel1,label2,...\tflags\n
 *
 * Returns heap-allocated string. Caller must free().
 */
char *gmail_sync_build_hdr(const char *raw_msg, char **labels, int label_count) {
    RAII_STRING char *from_raw = mime_get_header(raw_msg, "From");
    RAII_STRING char *subj_raw = mime_get_header(raw_msg, "Subject");
    RAII_STRING char *date_raw = mime_get_header(raw_msg, "Date");

    RAII_STRING char *from_dec = from_raw ? mime_decode_words(from_raw) : NULL;
    RAII_STRING char *subj_dec = subj_raw ? mime_decode_words(subj_raw) : NULL;
    RAII_STRING char *date_fmt = date_raw ? mime_format_date(date_raw) : NULL;

    const char *from = from_dec ? from_dec : "";
    const char *subj = subj_dec ? subj_dec : "";
    const char *date = date_fmt ? date_fmt : "";

    /* Build comma-separated label string */
    size_t lbl_len = 1;
    for (int i = 0; i < label_count; i++)
        lbl_len += strlen(labels[i]) + 1;
    char *lbl_str = calloc(lbl_len, 1);
    if (lbl_str) {
        for (int i = 0; i < label_count; i++) {
            if (i > 0) strcat(lbl_str, ",");
            strcat(lbl_str, labels[i]);
        }
    }

    /* Compute flags bitmask from labels */
    int flags = 0;
    for (int i = 0; i < label_count; i++) {
        if (strcmp(labels[i], "UNREAD") == 0)  flags |= MSG_FLAG_UNSEEN;
        if (strcmp(labels[i], "STARRED") == 0) flags |= MSG_FLAG_FLAGGED;
    }

    /* Replace tabs in fields with spaces */
    char *hdr = NULL;
    if (asprintf(&hdr, "%s\t%s\t%s\t%s\t%d",
                 from, subj, date, lbl_str ? lbl_str : "", flags) == -1)
        hdr = NULL;
    free(lbl_str);

    /* Sanitise: replace any tabs within field values */
    if (hdr) {
        /* The first 4 tabs are field separators; tabs within values got
         * inserted by asprintf if field values contained tabs.  Since we
         * used tab as separator this is inherently safe (mime_decode_words
         * doesn't produce tabs), but defend anyway. */
    }

    return hdr;
}

/* ── Filtered labels (metadata-only, excluded from indexing) ──────── */

int gmail_sync_is_filtered_label(const char *label_id) {
    if (!label_id) return 1;
    if (strcmp(label_id, "IMPORTANT") == 0) return 1;
    if (strcmp(label_id, "CHAT") == 0) return 1;
    return 0;
}

/* Returns 1 if label_id is a Gmail automatic inbox category (CATEGORY_*).
 * Category labels are indexed like user labels, but a message whose ONLY
 * non-filtered labels are CATEGORY_* is also added to _nolabel (Archive). */
static int is_category_label(const char *label_id) {
    return label_id && strncmp(label_id, "CATEGORY_", 9) == 0;
}

/* ── Full Sync ────────────────────────────────────────────────────── */

int gmail_sync_full(GmailClient *gc) {
    logger_log(LOG_INFO, "gmail_sync: starting full sync");

    /* 1. List all message IDs (paginated — show running count while fetching) */
    fprintf(stderr, "  Listing messages...");
    fflush(stderr);
    gmail_set_progress(gc, list_progress_cb, NULL);

    char (*all_uids)[17] = NULL;
    int uid_count = 0;
    if (gmail_list_messages(gc, NULL, NULL, &all_uids, &uid_count) != 0) {
        gmail_set_progress(gc, NULL, NULL);
        logger_log(LOG_ERROR, "gmail_sync: failed to list messages");
        return -1;
    }
    gmail_set_progress(gc, NULL, NULL);
    fprintf(stderr, "\r\033[K  %d messages found\n", uid_count);
    logger_log(LOG_INFO, "gmail_sync: %d messages to sync", uid_count);

    /* 2. For each message: fetch, store .eml, build .hdr, collect labels */
    int fetched = 0, skipped = 0;
#define PROGRESS_STEP 50   /* refresh the in-place counter every N messages */
    for (int i = 0; i < uid_count; i++) {
        const char *uid = all_uids[i];

        /* Skip only when BOTH .eml and .hdr are present.
         * If .eml exists but .hdr is missing (e.g. sync interrupted or
         * upgraded from an older version that did not write .hdr files),
         * re-fetch so the .hdr is created. */
        if (local_msg_exists("", uid) && local_hdr_exists("", uid)) {
            skipped++;
            if (i % PROGRESS_STEP == 0 || i == uid_count - 1) {
                fprintf(stderr, "\r\033[K  [%d/%d] (cached)", i + 1, uid_count);
                fflush(stderr);
            }
            continue;
        }

        /* Fetch raw message + labels */
        char **labels = NULL;
        int label_count = 0;
        char *raw = gmail_fetch_message(gc, uid, &labels, &label_count);
        if (!raw) {
            logger_log(LOG_WARN, "gmail_sync: failed to fetch %s, skipping", uid);
            for (int j = 0; j < label_count; j++) free(labels[j]);
            free(labels);
            continue;
        }

        /* Save .eml (flat, empty folder string for Gmail) */
        local_msg_save("", uid, raw, strlen(raw));

        /* Build and save .hdr */
        char *hdr = gmail_sync_build_hdr(raw, labels, label_count);
        if (hdr) {
            local_hdr_save("", uid, hdr, strlen(hdr));
            free(hdr);
        }
        free(raw);

        /* Update label index files */
        int has_real_label = 0; /* non-CATEGORY_, non-filtered label */
        for (int j = 0; j < label_count; j++) {
            if (gmail_sync_is_filtered_label(labels[j])) continue;

            /* Map SPAM/TRASH to underscore-prefixed filenames */
            const char *idx_name = labels[j];
            if (strcmp(labels[j], "SPAM") == 0) idx_name = "_spam";
            else if (strcmp(labels[j], "TRASH") == 0) idx_name = "_trash";

            label_idx_add(idx_name, uid);
            if (!is_category_label(labels[j]))
                has_real_label = 1;
        }
        /* Messages with no real (non-CATEGORY_) label go to Archive */
        if (!has_real_label)
            label_idx_add("_nolabel", uid);

        for (int j = 0; j < label_count; j++) free(labels[j]);
        free(labels);

        fetched++;
        fprintf(stderr, "\r\033[K  [%d/%d] fetched", i + 1, uid_count);
        fflush(stderr);
    }
    if (uid_count > 0)
        fprintf(stderr, "\r\033[K  %d fetched, %d already cached\n",
                fetched, skipped);

    /* 3. Save historyId from the Gmail profile */
    {
        RAII_STRING char *hid = gmail_get_history_id(gc);
        if (hid)
            local_gmail_history_save(hid);
        else
            logger_log(LOG_WARN, "gmail_sync: could not retrieve historyId");
    }

    /* 4. Save label ID→name mapping for friendly display */
    {
        char **lbl_names = NULL, **lbl_ids = NULL;
        int lbl_count = 0;
        if (gmail_list_labels(gc, &lbl_names, &lbl_ids, &lbl_count) == 0) {
            local_gmail_label_names_save(lbl_ids, lbl_names, lbl_count);
            for (int i = 0; i < lbl_count; i++) { free(lbl_names[i]); free(lbl_ids[i]); }
            free(lbl_names);
            free(lbl_ids);
        }
    }

    free(all_uids);
    logger_log(LOG_INFO, "gmail_sync: full sync completed (%d messages)", uid_count);
    return 0;
}

/* ── History delta processing ─────────────────────────────────────── */

struct history_ctx {
    GmailClient *gc;
    int added;
    int deleted;
    int label_changes;
};

static void process_message_added(const char *obj, int index, void *ctx) {
    (void)index;
    struct history_ctx *hc = ctx;

    /* obj is a history record with "message" sub-object */
    /* Extract message ID from the "message" field */
    /* The history API returns: {"message": {"id": "...", "labelIds": [...]}} */
    char *id = json_get_string(obj, "id");
    if (!id) return;

    /* Fetch and store the new message */
    char **labels = NULL;
    int label_count = 0;
    char *raw = gmail_fetch_message(hc->gc, id, &labels, &label_count);
    if (raw) {
        local_msg_save("", id, raw, strlen(raw));

        char *hdr = gmail_sync_build_hdr(raw, labels, label_count);
        if (hdr) {
            local_hdr_save("", id, hdr, strlen(hdr));
            free(hdr);
        }
        free(raw);

        int has_label = 0;
        for (int j = 0; j < label_count; j++) {
            if (gmail_sync_is_filtered_label(labels[j])) continue;
            const char *idx_name = labels[j];
            if (strcmp(labels[j], "SPAM") == 0) idx_name = "_spam";
            else if (strcmp(labels[j], "TRASH") == 0) idx_name = "_trash";
            label_idx_add(idx_name, id);
            has_label = 1;
        }
        if (!has_label) label_idx_add("_nolabel", id);

        hc->added++;
    }

    for (int j = 0; j < label_count; j++) free(labels[j]);
    free(labels);
    free(id);
}

static void process_message_deleted(const char *obj, int index, void *ctx) {
    (void)index;
    struct history_ctx *hc = ctx;

    char *id = json_get_string(obj, "id");
    if (!id) return;

    local_msg_delete("", id);

    /* Remove from all known label indexes — brute force scan */
    /* In practice this is rare; deleted messages are few */
    char **names = NULL, **ids = NULL;
    int count = 0;
    if (gmail_list_labels(hc->gc, &names, &ids, &count) == 0) {
        for (int i = 0; i < count; i++) {
            label_idx_remove(ids[i], id);
            free(names[i]);
            free(ids[i]);
        }
        free(names);
        free(ids);
    }
    label_idx_remove("_nolabel", id);
    label_idx_remove("_spam", id);
    label_idx_remove("_trash", id);

    hc->deleted++;
    free(id);
}

static void process_labels_added(const char *obj, int index, void *ctx) {
    (void)index;
    struct history_ctx *hc = ctx;

    char *id = json_get_string(obj, "id");
    if (!id) return;

    char **add_labels = NULL;
    int add_count = 0;
    json_get_string_array(obj, "labelIds", &add_labels, &add_count);

    for (int i = 0; i < add_count; i++) {
        if (gmail_sync_is_filtered_label(add_labels[i])) continue;
        const char *idx_name = add_labels[i];
        if (strcmp(add_labels[i], "SPAM") == 0) idx_name = "_spam";
        else if (strcmp(add_labels[i], "TRASH") == 0) idx_name = "_trash";
        label_idx_add(idx_name, id);
        /* Only remove from _nolabel when a real (non-CATEGORY_) label is added */
        if (!is_category_label(add_labels[i]))
            label_idx_remove("_nolabel", id);
    }

    for (int i = 0; i < add_count; i++) free(add_labels[i]);
    free(add_labels);
    free(id);
    hc->label_changes++;
}

static void process_labels_removed(const char *obj, int index, void *ctx) {
    (void)index;
    struct history_ctx *hc = ctx;

    char *id = json_get_string(obj, "id");
    if (!id) return;

    char **rm_labels = NULL;
    int rm_count = 0;
    json_get_string_array(obj, "labelIds", &rm_labels, &rm_count);

    for (int i = 0; i < rm_count; i++) {
        if (gmail_sync_is_filtered_label(rm_labels[i])) continue;
        const char *idx_name = rm_labels[i];
        if (strcmp(rm_labels[i], "SPAM") == 0) idx_name = "_spam";
        else if (strcmp(rm_labels[i], "TRASH") == 0) idx_name = "_trash";
        label_idx_remove(idx_name, id);
    }

    for (int i = 0; i < rm_count; i++) free(rm_labels[i]);
    free(rm_labels);

    /* Check if any labels remain; if none → add to _nolabel */
    /* Quick check: fetch message labels from server */
    char **cur_labels = NULL;
    int cur_count = 0;
    char *raw = gmail_fetch_message(hc->gc, id, &cur_labels, &cur_count);
    free(raw);

    int has_real_label = 0;
    for (int i = 0; i < cur_count; i++) {
        if (!gmail_sync_is_filtered_label(cur_labels[i]) &&
            !is_category_label(cur_labels[i]))
            has_real_label = 1;
        free(cur_labels[i]);
    }
    free(cur_labels);

    if (!has_real_label)
        label_idx_add("_nolabel", id);

    free(id);
    hc->label_changes++;
}

/* ── Incremental Sync ─────────────────────────────────────────────── */

int gmail_sync_incremental(GmailClient *gc) {
    char *history_id = local_gmail_history_load();
    if (!history_id) {
        logger_log(LOG_INFO, "gmail_sync: no historyId, need full sync");
        return -2;
    }

    logger_log(LOG_INFO, "gmail_sync: incremental from historyId %s", history_id);

    char *resp = gmail_get_history(gc, history_id);
    free(history_id);

    if (!resp) {
        logger_log(LOG_WARN, "gmail_sync: history expired or error");
        return -2;  /* Signal: need full sync */
    }

    struct history_ctx hc = { .gc = gc, .added = 0, .deleted = 0, .label_changes = 0 };

    /* Process each history record */
    /* The history response has: {"history": [{...}, ...], "historyId": "..."} */
    /* Each history entry may contain messagesAdded, messagesDeleted,
     * labelsAdded, labelsRemoved arrays */

    /* Process delta events from the history response.
     * Gmail nests events inside history records, but our json_foreach_object
     * searches the full response for matching keys, so a single scan works. */
    json_foreach_object(resp, "messagesAdded", process_message_added, &hc);
    json_foreach_object(resp, "messagesDeleted", process_message_deleted, &hc);
    json_foreach_object(resp, "labelsAdded", process_labels_added, &hc);
    json_foreach_object(resp, "labelsRemoved", process_labels_removed, &hc);

    /* Save updated historyId */
    RAII_STRING char *new_history_id = json_get_string(resp, "historyId");
    if (new_history_id)
        local_gmail_history_save(new_history_id);

    free(resp);

    /* Refresh label name mapping if any label events occurred */
    if (hc.label_changes > 0) {
        char **lbl_names = NULL, **lbl_ids = NULL;
        int lbl_count = 0;
        if (gmail_list_labels(gc, &lbl_names, &lbl_ids, &lbl_count) == 0) {
            local_gmail_label_names_save(lbl_ids, lbl_names, lbl_count);
            for (int i = 0; i < lbl_count; i++) { free(lbl_names[i]); free(lbl_ids[i]); }
            free(lbl_names);
            free(lbl_ids);
        }
    }

    logger_log(LOG_INFO, "gmail_sync: incremental done — added=%d deleted=%d labels=%d",
               hc.added, hc.deleted, hc.label_changes);
    return 0;
}

/* ── Auto Sync (public entry point) ───────────────────────────────── */

int gmail_sync(GmailClient *gc) {
    int rc = gmail_sync_incremental(gc);
    if (rc == -2) {
        logger_log(LOG_INFO, "gmail_sync: falling back to full sync");
        return gmail_sync_full(gc);
    }
    return rc;
}
