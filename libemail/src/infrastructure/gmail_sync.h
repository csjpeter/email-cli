#ifndef GMAIL_SYNC_H
#define GMAIL_SYNC_H

#include "gmail_client.h"

/**
 * @file gmail_sync.h
 * @brief Gmail synchronisation: full and incremental sync to local store.
 *
 * Full sync downloads all messages and builds label index files.
 * Incremental sync uses the History API for O(delta) efficiency.
 */

/**
 * @brief Synchronise a Gmail account to the local store.
 *
 * Smart multi-phase flow:
 *  1. Drain any queued downloads from pending_fetch.tsv (resumable).
 *  2. If a valid historyId exists: fast incremental sync (O(1) API calls).
 *     This is attempted even after draining pending downloads, because the
 *     historyId snapshot already covers them; incremental catches anything
 *     that arrived on the server after the snapshot.
 *  3. Otherwise (no historyId, or historyId expired): full reconcile —
 *     list all server UIDs, queue missing ones, then download the queue.
 *
 * Local store must be initialised (local_store_init) before calling.
 *
 * @param gc  Connected Gmail client.
 * @return 0 on success, -1 on error.
 */
int gmail_sync(GmailClient *gc);

/**
 * @brief Full sync: reconcile + fetch all missing messages.
 *
 * Equivalent to gmail_sync_reconcile() followed by gmail_sync_fetch_pending().
 * Expensive on first run (O(total messages)); subsequent calls only download
 * what has changed since the last sync.
 *
 * @param gc  Connected Gmail client.
 * @return 0 on success, -1 on error.
 */
int gmail_sync_full(GmailClient *gc);

/**
 * @brief Incremental sync using the Gmail History API.
 *
 * Processes messagesAdded, messagesDeleted, labelsAdded, labelsRemoved
 * deltas since the saved historyId.
 *
 * @param gc  Connected Gmail client.
 * @return 0 on success, -1 on error, -2 on expired historyId (caller
 *         should fall back to reconcile).
 */
int gmail_sync_incremental(GmailClient *gc);

/**
 * @brief Reconcile: list all server message IDs and queue missing ones.
 *
 * Calls gmail_list_messages(), compares with the local store, and appends
 * any missing UIDs to pending_fetch.tsv.  Does NOT download messages.
 * Also saves the current historyId and label name mapping.
 *
 * @param gc  Connected Gmail client.
 * @return Number of UIDs queued (>=0), or -1 on fatal error.
 */
int gmail_sync_reconcile(GmailClient *gc);

/**
 * @brief Download all messages listed in pending_fetch.tsv.
 *
 * Iterates the pending-fetch queue, downloads each missing message,
 * saves .eml + .hdr, applies mail rules, updates label indexes, and
 * removes the entry from the queue.  Failed downloads are left in the
 * queue for retry on the next call.
 *
 * @param gc  Connected Gmail client.
 * @return Number of messages successfully downloaded.
 */
int gmail_sync_fetch_pending(GmailClient *gc);

/**
 * @brief Rebuild all label .idx files from locally cached .hdr files.
 *
 * Does NOT contact the Gmail API.  Reads every .hdr file in the local
 * message store and reconstructs the label index files from scratch.
 *
 * Use this to repair missing or incomplete indexes without re-downloading
 * messages (e.g. after upgrading from a version that did not write .idx
 * files, or to recover from a corrupted index).
 *
 * Local store must be initialised (local_store_init) before calling.
 *
 * @return 0 on success, -1 on error.
 */
int gmail_sync_rebuild_indexes(void);

/**
 * @brief Repair pass: clear MSG_FLAG_UNSEEN on all archived (_nolabel) messages.
 *
 * Iterates every UID in the _nolabel index and clears the UNSEEN bit in the
 * corresponding .hdr file.  Called automatically at the end of incremental
 * sync to fix messages that arrived from Gmail already in archive state with
 * the UNREAD label set.
 *
 * Local store must be initialised (local_store_init) before calling.
 */
void gmail_sync_repair_archive_flags(void);

/**
 * @brief Check whether a Gmail label should be filtered out from display/indexing.
 *
 * Filters out CATEGORY_* labels and IMPORTANT (opaque Google ML classification).
 *
 * @param label_id  Gmail label ID string.
 * @return 1 if the label should be filtered (excluded), 0 otherwise.
 */
int gmail_sync_is_filtered_label(const char *label_id);

/**
 * @brief Build a .hdr cache string from raw RFC 2822 message and label list.
 *
 * Format: from\tsubject\tdate\tlabel1,label2,...\tflags
 *
 * @param raw_msg      Raw RFC 2822 message content.
 * @param labels       Array of Gmail label ID strings.
 * @param label_count  Number of labels in the array.
 * @return Heap-allocated string (caller must free), or NULL on error.
 */
char *gmail_sync_build_hdr(const char *raw_msg, char **labels, int label_count);

#endif /* GMAIL_SYNC_H */
