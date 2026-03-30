#ifndef LOCAL_STORE_H
#define LOCAL_STORE_H

#include <stddef.h>

/**
 * @file local_store.h
 * @brief Local copies of IMAP messages and headers.
 *
 * Full messages: ~/.local/share/email-cli/messages/<folder>/<uid>.eml
 * Headers only:  ~/.local/share/email-cli/headers/<folder>/<uid>.hdr
 * UI preferences: ~/.local/share/email-cli/ui.ini
 */

/**
 * @brief Checks whether a local copy of a message exists on disk.
 * @param folder Mailbox folder name (e.g. "INBOX").
 * @param uid    IMAP UID of the message.
 * @return 1 if the file exists, 0 otherwise.
 */
int local_msg_exists(const char *folder, int uid);

/**
 * @brief Writes raw message content to the local store.
 * @param folder  Mailbox folder name.
 * @param uid     IMAP UID of the message.
 * @param content Raw message bytes.
 * @param len     Number of bytes to write.
 * @return 0 on success, -1 on failure.
 */
int local_msg_save(const char *folder, int uid, const char *content, size_t len);

/**
 * @brief Reads a locally stored message from disk.
 * @param folder Mailbox folder name.
 * @param uid    IMAP UID of the message.
 * @return Heap-allocated NUL-terminated string, or NULL. Caller must free.
 */
char *local_msg_load(const char *folder, int uid);

/* ── Header local copies (headers/<folder>/<uid>.hdr) ─────────────────── */

/** @brief Checks whether a local header copy exists. */
int   local_hdr_exists(const char *folder, int uid);

/** @brief Writes header content to the local header store. */
int   local_hdr_save(const char *folder, int uid, const char *content, size_t len);

/**
 * @brief Reads a locally stored header from disk.
 * @return Heap-allocated NUL-terminated string, or NULL. Caller must free.
 */
char *local_hdr_load(const char *folder, int uid);

/* ── UI preferences ──────────────────────────────────────────────────── */

/**
 * @brief Reads an integer UI preference from ~/.local/share/email-cli/ui.ini.
 * @param key         Preference key (e.g. "folder_view_mode").
 * @param default_val Value to return if key is absent or file missing.
 * @return Stored integer value, or @p default_val.
 */
int ui_pref_get_int(const char *key, int default_val);

/**
 * @brief Writes an integer UI preference to ~/.local/share/email-cli/ui.ini.
 * @param key   Preference key.
 * @param value Integer value to store.
 * @return 0 on success, -1 on failure.
 */
int ui_pref_set_int(const char *key, int value);

/**
 * @brief Removes local header copies whose UID is not in @p keep_uids.
 *
 * Call this after a successful SEARCH ALL to remove headers for messages
 * that have been deleted from the server.
 *
 * @param folder     Mailbox folder name.
 * @param keep_uids  Array of UIDs that are still present on the server.
 * @param keep_count Number of entries in @p keep_uids.
 */
void  local_hdr_evict_stale(const char *folder,
                              const int *keep_uids, int keep_count);

#endif /* LOCAL_STORE_H */
