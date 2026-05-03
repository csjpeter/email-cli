#include "gmail_sync.h"
#include "gmail_client.h"
#include "local_store.h"
#include "mail_rules.h"
#include "mime_util.h"
#include "json_util.h"
#include "logger.h"
#include "raii.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

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
        if (strcmp(labels[i], "UNREAD")  == 0) flags |= MSG_FLAG_UNSEEN;
        if (strcmp(labels[i], "STARRED") == 0) flags |= MSG_FLAG_FLAGGED;
        if (strcmp(labels[i], "SPAM")    == 0) flags |= MSG_FLAG_JUNK;
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

/* ── Mail rules helper ───────────────────────────────────────────── */

/* Apply mail rules to a newly stored message.
 * Builds labels_csv from Gmail label IDs (resolving user labels to names),
 * calls mail_rules_apply(), then updates the local .hdr and label indexes. */
static void apply_rules_to_new_message(const MailRules *rules, const char *uid,
                                        const char *raw_msg,
                                        char **labels, int label_count)
{
    if (!rules || rules->count == 0) return;

    RAII_STRING char *from_raw = mime_get_header(raw_msg, "From");
    RAII_STRING char *subj_raw = mime_get_header(raw_msg, "Subject");
    RAII_STRING char *to_raw   = mime_get_header(raw_msg, "To");
    RAII_STRING char *from_dec = from_raw ? mime_decode_words(from_raw) : NULL;
    RAII_STRING char *subj_dec = subj_raw ? mime_decode_words(subj_raw) : NULL;
    RAII_STRING char *to_dec   = to_raw   ? mime_decode_words(to_raw)   : NULL;

    /* Build labels_csv using friendly names where available */
    size_t lcsz = 1;
    for (int i = 0; i < label_count; i++) {
        char *name = local_gmail_label_name_lookup(labels[i]);
        lcsz += strlen(name ? name : labels[i]) + 2;
        free(name);
    }
    char *lcsv = calloc(lcsz, 1);
    if (!lcsv) return;
    for (int i = 0; i < label_count; i++) {
        char *name = local_gmail_label_name_lookup(labels[i]);
        const char *display = name ? name : labels[i];
        if (lcsv[0]) strcat(lcsv, ",");
        strcat(lcsv, display);
        free(name);
    }

    char **add_out = NULL; int add_count = 0;
    char **rm_out  = NULL; int rm_count  = 0;
    int fired = mail_rules_apply(rules,
                                  from_dec, subj_dec, to_dec, lcsv,
                                  NULL, (time_t)0,   /* body/date unavailable during Gmail sync */
                                  &add_out, &add_count,
                                  &rm_out,  &rm_count);
    free(lcsv);
    if (fired <= 0) return;

    logger_log(LOG_INFO, "gmail_sync: rules fired=%d for %s (add=%d rm=%d)",
               fired, uid, add_count, rm_count);

    /* Update local .hdr and label indexes */
    local_hdr_update_labels("", uid,
                             (const char **)add_out, add_count,
                             (const char **)rm_out,  rm_count);
    for (int i = 0; i < add_count; i++) {
        label_idx_add(add_out[i], uid);
        free(add_out[i]);
    }
    for (int i = 0; i < rm_count; i++) {
        label_idx_remove(rm_out[i], uid);
        free(rm_out[i]);
    }
    free(add_out);
    free(rm_out);

    /* Update contact suggestion cache */
    {
        char *from_h = mime_get_header(raw_msg, "From");
        char *to_h   = mime_get_header(raw_msg, "To");
        char *cc_h   = mime_get_header(raw_msg, "Cc");
        local_contacts_update(from_h, to_h, cc_h);
        free(from_h); free(to_h); free(cc_h);
    }
}

/* ── Label index rebuild ──────────────────────────────────────────── */

typedef struct { char label[64]; char uid[17]; } LabelUidPair;

static int cmp_lbl_uid_pair(const void *a, const void *b) {
    const LabelUidPair *pa = a, *pb = b;
    int c = strcmp(pa->label, pb->label);
    return c ? c : strcmp(pa->uid, pb->uid);
}

/**
 * Rebuild ALL label .idx files from the .hdr files for the given UIDs.
 *
 * Efficient O(N log N) approach:
 *   1. Read each .hdr and collect (label, uid) pairs in memory.
 *   2. Sort the flat pair array.
 *   3. Write each label's .idx file in one grouped pass.
 *
 * This is called at the end of every full sync so that cached messages
 * (whose .idx entries were never written) are correctly indexed.
 */
static void rebuild_label_indexes(const char (*uids)[17], int uid_count) {
    if (uid_count <= 0) return;

    fprintf(stderr, "  Rebuilding label indexes...");
    fflush(stderr);

    /* Phase 1: collect (label, uid) pairs from all .hdr files */
    size_t cap = (size_t)uid_count * 5; /* ~5 labels per message */
    LabelUidPair *pairs = malloc(cap * sizeof(LabelUidPair));
    if (!pairs) {
        fprintf(stderr, " [out of memory]\n");
        return;
    }
    int npairs = 0;

    for (int i = 0; i < uid_count; i++) {
        /* Load full .hdr so we can both collect label pairs and sync the
         * flags integer in one read (avoid a separate local_hdr_get_labels
         * call followed by local_hdr_update_labels). */
        char *hdr = local_hdr_load("", uids[i]);
        if (!hdr) continue;

        /* Locate labels field (4th tab-separated token).
         * Track the tab pointer so we can NUL-terminate the prefix later. */
        char *t3_tab = hdr;
        for (int f = 0; f < 3; f++) {
            t3_tab = strchr(t3_tab, '\t');
            if (!t3_tab) break;
            if (f < 2) t3_tab++;
        }
        if (!t3_tab || t3_tab == hdr) { free(hdr); continue; }
        char *lbl_start = t3_tab + 1;   /* start of labels CSV field */

        /* Locate optional flags field (5th token) and read old value */
        char *t4 = strchr(lbl_start, '\t');
        int old_flags = 0;
        if (t4) {
            old_flags = atoi(t4 + 1);
            *t4 = '\0';   /* NUL-terminate labels field in-place */
        } else {
            char *nl = strchr(lbl_start, '\n');
            if (nl) *nl = '\0';
        }

        /* Derive new flags from labels (preserving non-label bits) */
        int new_flags = old_flags & ~(MSG_FLAG_UNSEEN | MSG_FLAG_FLAGGED);
        int has_real = 0;

        /* Iterate labels via a copy (tokenising modifies the string) */
        char *lbl_copy = strdup(lbl_start);
        if (!lbl_copy) { free(hdr); continue; }

        char *tok = lbl_copy;
        while (tok) {
            char *comma = strchr(tok, ',');
            if (comma) *comma = '\0';

            if (tok[0] && !gmail_sync_is_filtered_label(tok)) {
                const char *idx_name = tok;
                if      (strcmp(tok, "SPAM")  == 0) idx_name = "_spam";
                else if (strcmp(tok, "TRASH") == 0) idx_name = "_trash";

                if (npairs >= (int)cap) {
                    cap = cap * 2 + 1;
                    LabelUidPair *tmp = realloc(pairs, cap * sizeof(LabelUidPair));
                    if (!tmp) { free(lbl_copy); free(hdr); free(pairs); return; }
                    pairs = tmp;
                }
                strncpy(pairs[npairs].label, idx_name, 63);
                pairs[npairs].label[63] = '\0';
                strncpy(pairs[npairs].uid, uids[i], 16);
                pairs[npairs].uid[16] = '\0';
                npairs++;

                if (!is_category_label(tok)) has_real = 1;
                if (strcmp(tok, "UNREAD")  == 0) new_flags |= MSG_FLAG_UNSEEN;
                if (strcmp(tok, "STARRED") == 0) new_flags |= MSG_FLAG_FLAGGED;
            }
            tok = comma ? comma + 1 : NULL;
        }
        free(lbl_copy);

        /* Messages with no real (non-CATEGORY_) label → Archive.
         * Archived messages are always considered read. */
        if (!has_real) {
            if (npairs >= (int)cap) {
                cap = cap * 2 + 1;
                LabelUidPair *tmp = realloc(pairs, cap * sizeof(LabelUidPair));
                if (!tmp) { free(hdr); free(pairs); return; }
                pairs = tmp;
            }
            strncpy(pairs[npairs].label, "_nolabel", 63);
            strncpy(pairs[npairs].uid, uids[i], 16);
            pairs[npairs].uid[16] = '\0';
            npairs++;
            new_flags &= ~MSG_FLAG_UNSEEN;
        }

        /* Sync flags integer if it disagrees with the labels CSV.
         * lbl_start still points into hdr at the labels field.
         * NUL-terminate the prefix at t3_tab then reassemble. */
        if (new_flags != old_flags) {
            *t3_tab = '\0';
            char *updated = NULL;
            if (asprintf(&updated, "%s\t%s\t%d", hdr, lbl_start, new_flags) != -1) {
                local_hdr_save("", uids[i], updated, strlen(updated));
                free(updated);
            }
        }

        free(hdr);
    }

    /* Phase 2: sort by (label, uid) */
    qsort(pairs, (size_t)npairs, sizeof(LabelUidPair), cmp_lbl_uid_pair);

    /* Phase 3: group by label and write each .idx file */
    int labels_written = 0;
    int i = 0;
    while (i < npairs) {
        const char *cur_label = pairs[i].label;
        int j = i;
        while (j < npairs && strcmp(pairs[j].label, cur_label) == 0) j++;
        int run = j - i;

        char (*uid_arr)[17] = malloc((size_t)run * sizeof(char[17]));
        if (uid_arr) {
            int unique = 0;
            for (int k = i; k < j; k++) {
                if (unique == 0 ||
                    strcmp(uid_arr[unique - 1], pairs[k].uid) != 0) {
                    memcpy(uid_arr[unique++], pairs[k].uid, 17);
                }
            }
            label_idx_write(cur_label, (const char (*)[17])uid_arr, unique);
            free(uid_arr);
            labels_written++;
        }
        i = j;
    }
    free(pairs);

    fprintf(stderr, "\r\033[K  Label indexes rebuilt (%d labels)\n",
            labels_written);
    logger_log(LOG_INFO,
               "gmail_sync: rebuilt %d label indexes from %d messages",
               labels_written, uid_count);
}

/**
 * Rebuild all label .idx files from locally cached .hdr files.
 * Does NOT contact the Gmail API.
 * Use this to repair missing or incomplete indexes without re-downloading.
 */
int gmail_sync_rebuild_indexes(void) {
    char (*uids)[17] = NULL;
    int count = 0;
    if (local_hdr_list_all_uids("", &uids, &count) != 0) {
        fprintf(stderr, "Error: could not scan local message store.\n");
        return -1;
    }
    fprintf(stderr, "  Found %d cached messages.\n", count);
    rebuild_label_indexes((const char (*)[17])uids, count);
    free(uids);
    return 0;
}

/* ── Single message fetch+store helper ───────────────────────────────── */

/**
 * Fetch one message from the Gmail API, save .eml + .hdr, apply rules,
 * update label indexes.
 * Returns 0 on success, -1 on fetch error (transient; caller should retry).
 */
static int store_fetched_message(GmailClient *gc, const char *uid,
                                  const MailRules *rules)
{
    char **labels = NULL;
    int label_count = 0;
    char *raw = gmail_fetch_message(gc, uid, &labels, &label_count);
    if (!raw) {
        logger_log(LOG_WARN, "gmail_sync: failed to fetch %s", uid);
        for (int j = 0; j < label_count; j++) free(labels[j]);
        free(labels);
        return -1;
    }

    local_msg_save("", uid, raw, strlen(raw));

    char *hdr = gmail_sync_build_hdr(raw, labels, label_count);
    if (hdr) { local_hdr_save("", uid, hdr, strlen(hdr)); free(hdr); }

    apply_rules_to_new_message(rules, uid, raw, labels, label_count);
    free(raw);

    int has_real_label = 0;
    for (int j = 0; j < label_count; j++) {
        if (gmail_sync_is_filtered_label(labels[j])) continue;
        const char *idx_name = labels[j];
        if      (strcmp(labels[j], "SPAM")  == 0) idx_name = "_spam";
        else if (strcmp(labels[j], "TRASH") == 0) idx_name = "_trash";
        label_idx_add(idx_name, uid);
        if (!is_category_label(labels[j])) has_real_label = 1;
    }
    if (!has_real_label) {
        label_idx_add("_nolabel", uid);
        int cur_flags = 0;
        for (int j = 0; j < label_count; j++) {
            if (strcmp(labels[j], "UNREAD")  == 0) cur_flags |= MSG_FLAG_UNSEEN;
            if (strcmp(labels[j], "STARRED") == 0) cur_flags |= MSG_FLAG_FLAGGED;
        }
        if (cur_flags & MSG_FLAG_UNSEEN)
            local_hdr_update_flags("", uid, cur_flags & ~MSG_FLAG_UNSEEN);
    }

    for (int j = 0; j < label_count; j++) free(labels[j]);
    free(labels);
    return 0;
}

/* ── Reconcile: discover missing UIDs and queue them ─────────────────── */

/**
 * List all server-side message IDs, compare with the local store, and
 * add any missing UIDs to pending_fetch.tsv.  Does NOT download messages.
 *
 * Also updates the historyId and label name mapping so subsequent
 * incremental syncs know where to resume from.
 *
 * Returns the number of UIDs added to the pending-fetch queue, or -1 on
 * a fatal error (e.g. the server cannot be reached).
 */
int gmail_sync_reconcile(GmailClient *gc) {
    logger_log(LOG_INFO, "gmail_sync: reconcile — listing server messages");

    fprintf(stderr, "  Listing messages...");
    fflush(stderr);
    gmail_set_progress(gc, list_progress_cb, NULL);

    char (*all_uids)[17] = NULL;
    int uid_count = 0;
    char *list_history_id = NULL;
    if (gmail_list_messages(gc, NULL, NULL, &all_uids, &uid_count, &list_history_id) != 0) {
        gmail_set_progress(gc, NULL, NULL);
        free(list_history_id);
        logger_log(LOG_ERROR, "gmail_sync: reconcile failed to list messages");
        return -1;
    }
    gmail_set_progress(gc, NULL, NULL);
    fprintf(stderr, "\r\033[K  %d messages on server\n", uid_count);

    /* Clear any stale pending_fetch entries before repopulating */
    local_pending_fetch_clear();

    int queued = 0, cached = 0;
    for (int i = 0; i < uid_count; i++) {
        const char *uid = all_uids[i];
        if (local_msg_exists("", uid) && local_hdr_exists("", uid)) {
            cached++;
            if (i % 500 == 0 || i == uid_count - 1) {
                fprintf(stderr, "\r\033[K  Scanning local store: %d/%d",
                        i + 1, uid_count);
                fflush(stderr);
            }
            continue;
        }
        local_pending_fetch_add(uid);
        queued++;
        if ((cached + queued) % 500 == 0 || i == uid_count - 1) {
            fprintf(stderr, "\r\033[K  Scanning local store: %d/%d",
                    i + 1, uid_count);
            fflush(stderr);
        }
    }
    if (uid_count > 0)
        fprintf(stderr, "\r\033[K  %d cached, %d queued for download\n",
                cached, queued);

    /* Save historyId so next run can use incremental sync.
     * Prefer the historyId from the messages.list response (always fresh);
     * fall back to the /profile endpoint only if that field was absent. */
    if (list_history_id) {
        fprintf(stderr, "  historyId from list response: %s\n", list_history_id);
        local_gmail_history_save(list_history_id);
        free(list_history_id);
        list_history_id = NULL;
    } else {
        RAII_STRING char *hid = gmail_get_history_id(gc);
        if (hid)
            local_gmail_history_save(hid);
        else
            logger_log(LOG_WARN, "gmail_sync: reconcile: could not retrieve historyId");
    }

    /* Save label ID→name mapping */
    {
        char **lbl_names = NULL, **lbl_ids = NULL;
        int lbl_count = 0;
        if (gmail_list_labels(gc, &lbl_names, &lbl_ids, &lbl_count) == 0) {
            local_gmail_label_names_save(lbl_ids, lbl_names, lbl_count);
            for (int i = 0; i < lbl_count; i++) { free(lbl_names[i]); free(lbl_ids[i]); }
            free(lbl_names); free(lbl_ids);
        }
    }

    free(all_uids);
    logger_log(LOG_INFO, "gmail_sync: reconcile done — %d cached, %d queued",
               cached, queued);
    return queued;
}

/* ── Fetch pending: download queued messages ─────────────────────────── */

/**
 * Download all message UIDs listed in pending_fetch.tsv.
 * Removes each entry from the queue on successful download.
 * Leaves failures in the queue for retry on the next sync.
 *
 * Returns number of messages successfully downloaded.
 */
int gmail_sync_fetch_pending(GmailClient *gc) {
    int count = 0;
    char (*uids)[17] = local_pending_fetch_load(&count);
    if (!uids || count == 0) {
        free(uids);
        return 0;
    }

    logger_log(LOG_INFO, "gmail_sync: fetch_pending — %d messages to download", count);
    fprintf(stderr, "  Downloading %d message(s)...\n", count);

    MailRules *rules = mail_rules_load(local_store_account_name());
    int fetched = 0;
#define PROGRESS_STEP 50
    for (int i = 0; i < count; i++) {
        const char *uid = uids[i];

        if (local_msg_exists("", uid) && local_hdr_exists("", uid)) {
            /* Already present — clean up stale pending entry */
            local_pending_fetch_remove(uid);
            continue;
        }

        if (store_fetched_message(gc, uid, rules) == 0) {
            local_pending_fetch_remove(uid);
            fetched++;
        }
        /* On failure: leave in queue for retry */

        if (i % PROGRESS_STEP == 0 || i == count - 1) {
            fprintf(stderr, "\r\033[K  [%d/%d] downloaded", fetched, count);
            fflush(stderr);
        }
    }
    if (count > 0)
        fprintf(stderr, "\r\033[K  %d of %d downloaded\n", fetched, count);

    mail_rules_free(rules);
    free(uids);
    logger_log(LOG_INFO, "gmail_sync: fetch_pending done — %d/%d downloaded",
               fetched, count);
    return fetched;
}

/* ── Full Sync ────────────────────────────────────────────────────── */

int gmail_sync_full(GmailClient *gc) {
    logger_log(LOG_INFO, "gmail_sync: starting full sync");

    int queued = gmail_sync_reconcile(gc);
    if (queued < 0) return -1;

    if (queued > 0)
        gmail_sync_fetch_pending(gc);

    /* Rebuild label indexes from .hdr files so that even when all messages
     * were already cached (queued == 0) the indexes are consistent. */
    {
        char (*all_uids)[17] = NULL;
        int all_count = 0;
        if (local_hdr_list_all_uids("", &all_uids, &all_count) == 0 && all_count > 0)
            rebuild_label_indexes((const char (*)[17])all_uids, all_count);
        free(all_uids);
    }

    return 0;
}

/* ── History delta processing ─────────────────────────────────────── */

struct history_ctx {
    GmailClient *gc;
    MailRules   *rules;
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

        apply_rules_to_new_message(hc->rules, id, raw, labels, label_count);
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
        if (!has_label) {
            label_idx_add("_nolabel", id);
            /* Archived messages are always read */
            int cur_flags = 0;
            for (int j = 0; j < label_count; j++) {
                if (strcmp(labels[j], "UNREAD")  == 0) cur_flags |= MSG_FLAG_UNSEEN;
                if (strcmp(labels[j], "STARRED") == 0) cur_flags |= MSG_FLAG_FLAGGED;
            }
            if (cur_flags & MSG_FLAG_UNSEEN)
                local_hdr_update_flags("", id, cur_flags & ~MSG_FLAG_UNSEEN);
        }

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

    /* Keep .hdr labels field in sync so rebuild_label_indexes stays accurate. */
    local_hdr_update_labels("", id,
                            (const char **)add_labels, add_count, NULL, 0);

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

    if (!has_real_label) {
        label_idx_add("_nolabel", id);
        /* Archived messages are always read: clear UNSEEN from .hdr flags */
        char *cur_hdr = local_hdr_load("", id);
        if (cur_hdr) {
            char *last_tab = strrchr(cur_hdr, '\t');
            if (last_tab) {
                int cur_flags = atoi(last_tab + 1);
                if (cur_flags & MSG_FLAG_UNSEEN)
                    local_hdr_update_flags("", id, cur_flags & ~MSG_FLAG_UNSEEN);
            }
            free(cur_hdr);
        }
    }

    /* Keep .hdr labels field in sync so rebuild_label_indexes stays accurate. */
    local_hdr_update_labels("", id,
                            NULL, 0, (const char **)rm_labels, rm_count);

    free(id);
    hc->label_changes++;
}

/* ── One-time repair: archived messages must not be unread ─────────── */

void gmail_sync_repair_archive_flags(void) {
    char (*uids)[17] = NULL;
    int count = 0;
    if (label_idx_load("_nolabel", &uids, &count) != 0 || count == 0) {
        free(uids);
        return;
    }
    for (int i = 0; i < count; i++) {
        char *hdr = local_hdr_load("", uids[i]);
        if (!hdr) continue;
        char *last_tab = strrchr(hdr, '\t');
        if (last_tab) {
            int flags = atoi(last_tab + 1);
            if (flags & MSG_FLAG_UNSEEN)
                local_hdr_update_flags("", uids[i], flags & ~MSG_FLAG_UNSEEN);
        }
        free(hdr);
    }
    free(uids);
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
        fprintf(stderr, "  Incremental: History API returned error/404 (historyId expired or network issue).\n");
        logger_log(LOG_WARN, "gmail_sync: history expired or error");
        return -2;  /* Signal: need full sync */
    }

    MailRules *inc_rules = mail_rules_load(local_store_account_name());
    struct history_ctx hc = { .gc = gc, .rules = inc_rules, .added = 0, .deleted = 0, .label_changes = 0 };

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

    /* Ensure no archived message is marked unread (repair existing data too) */
    gmail_sync_repair_archive_flags();

    mail_rules_free(inc_rules);
    logger_log(LOG_INFO, "gmail_sync: incremental done — added=%d deleted=%d labels=%d",
               hc.added, hc.deleted, hc.label_changes);
    return 0;
}

/* ── Auto Sync (public entry point) ───────────────────────────────── */

/**
 * Smart sync flow:
 *
 *   1. If pending_fetch.tsv is non-empty, download those first (resuming an
 *      interrupted previous sync or initial download).
 *   2. If the local store was already complete (no pending at start) AND a
 *      valid historyId exists, use the fast incremental path (1–2 API calls).
 *   3. Otherwise, run reconcile (full UID listing) to discover any missing
 *      messages, then download them.
 *
 * This ensures:
 *   - First run or expired historyId: O(N) reconcile → O(missing) downloads.
 *   - Subsequent runs on a mature store: O(1) incremental (no listing at all).
 *   - Interrupted downloads: resume from pending_fetch.tsv without re-listing.
 */
int gmail_sync(GmailClient *gc) {
    /* Step 1: check readiness before downloading anything */
    int had_pending = local_pending_fetch_count() > 0;

    /* Step 2: drain any queued downloads from a previous (possibly interrupted) sync */
    if (had_pending)
        gmail_sync_fetch_pending(gc);

    /* Step 3: try fast incremental path if we have a saved historyId.
     * We do this regardless of whether there were pending downloads —
     * draining pending_fetch.tsv already brought the local store up to the
     * reconcile snapshot; incremental then catches anything that arrived
     * on the server after that snapshot. */
    char *history_id = local_gmail_history_load();
    int have_history = (history_id != NULL);
    free(history_id);

    if (have_history) {
        fprintf(stderr, "  Incremental sync (historyId present)...\n");
        int rc = gmail_sync_incremental(gc);
        if (rc == 0) {
            fprintf(stderr, "  Incremental sync: up to date.\n");
            return 0; /* fast path — done */
        }
        if (rc != -2) return rc; /* unexpected error */
        fprintf(stderr, "  Incremental sync: historyId expired — falling back to full reconcile.\n");
        logger_log(LOG_INFO, "gmail_sync: historyId expired, falling back to reconcile");
    } else {
        fprintf(stderr, "  No saved historyId — full reconcile needed.\n");
    }

    /* Step 4: reconcile (discover what is missing) */
    int queued = gmail_sync_reconcile(gc);
    if (queued < 0) return -1;

    /* Step 5: download what reconcile found */
    if (queued > 0)
        gmail_sync_fetch_pending(gc);

    /* Step 6: rebuild label indexes from .hdr files.
     * Necessary when all messages were already cached (queued == 0) but
     * label .idx files were deleted or are missing (e.g. manual deletion
     * or upgrade from an older version). */
    {
        char (*all_uids)[17] = NULL;
        int all_count = 0;
        if (local_hdr_list_all_uids("", &all_uids, &all_count) == 0 && all_count > 0)
            rebuild_label_indexes((const char (*)[17])all_uids, all_count);
        free(all_uids);
    }

    return 0;
}
