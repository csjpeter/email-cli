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
 * Checks for a saved historyId.  If present, attempts incremental sync
 * via the History API.  If the historyId is stale or absent, performs
 * a full sync.
 *
 * Local store must be initialised (local_store_init) before calling.
 *
 * @param gc  Connected Gmail client.
 * @return 0 on success, -1 on error.
 */
int gmail_sync(GmailClient *gc);

/**
 * @brief Full sync: download all messages and rebuild label indexes.
 *
 * Expensive operation (O(total messages)).  Used on first sync or when
 * the saved historyId has expired.
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
 *         should fall back to full sync).
 */
int gmail_sync_incremental(GmailClient *gc);

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
