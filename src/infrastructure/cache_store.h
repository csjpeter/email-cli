#ifndef CACHE_STORE_H
#define CACHE_STORE_H

#include <stddef.h>

/**
 * @file cache_store.h
 * @brief Local message cache: save and load raw .eml files per folder/UID.
 *
 * Messages are stored at ~/.cache/email-cli/messages/<folder>/<uid>.eml.
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

#endif /* CACHE_STORE_H */
