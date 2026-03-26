#ifndef CACHE_STORE_H
#define CACHE_STORE_H

#include <stddef.h>

/**
 * @file cache_store.h
 * @brief Local message/header cache for IMAP content.
 *
 * Full messages: ~/.cache/email-cli/messages/<folder>/<uid>.eml
 * Headers only:  ~/.cache/email-cli/headers/<folder>/<uid>.hdr
 */

/**
 * @brief Checks whether a cached copy of a message exists on disk.
 * @param folder Mailbox folder name (e.g. "INBOX").
 * @param uid    IMAP UID of the message.
 * @return 1 if the file exists, 0 otherwise.
 */
int cache_exists(const char *folder, int uid);

/**
 * @brief Writes raw message content to the local cache.
 * @param folder  Mailbox folder name.
 * @param uid     IMAP UID of the message.
 * @param content Raw message bytes.
 * @param len     Number of bytes to write.
 * @return 0 on success, -1 on failure.
 */
int cache_save(const char *folder, int uid, const char *content, size_t len);

/**
 * @brief Reads a cached message from disk.
 * @param folder Mailbox folder name.
 * @param uid    IMAP UID of the message.
 * @return Heap-allocated NUL-terminated string, or NULL. Caller must free.
 */
char *cache_load(const char *folder, int uid);

/* ── Header cache (headers/<folder>/<uid>.hdr) ────────────────────────── */

/** @brief Checks whether a cached header file exists. */
int   hcache_exists(const char *folder, int uid);

/** @brief Writes header content to the header cache. */
int   hcache_save(const char *folder, int uid, const char *content, size_t len);

/**
 * @brief Reads a cached header from disk.
 * @return Heap-allocated NUL-terminated string, or NULL. Caller must free.
 */
char *hcache_load(const char *folder, int uid);

/**
 * @brief Removes header cache files whose UID is not in @p keep_uids.
 *
 * Call this after a successful SEARCH ALL to evict headers for messages
 * that have been deleted from the server.
 *
 * @param folder     Mailbox folder name.
 * @param keep_uids  Array of UIDs that are still present on the server.
 * @param keep_count Number of entries in @p keep_uids.
 */
void  hcache_evict_stale(const char *folder,
                          const int *keep_uids, int keep_count);

#endif /* CACHE_STORE_H */
