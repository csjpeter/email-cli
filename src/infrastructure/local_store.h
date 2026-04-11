#ifndef LOCAL_STORE_H
#define LOCAL_STORE_H

#include <stddef.h>

/* ── Message flag bitmask constants ─────────────────────────────────────── */

#define MSG_FLAG_UNSEEN   (1 << 0)  /* \Seen not set on server */
#define MSG_FLAG_FLAGGED  (1 << 1)  /* \Flagged — starred / important */
#define MSG_FLAG_DONE     (1 << 2)  /* $Done keyword */
#define MSG_FLAG_ATTACH   (1 << 3)  /* message has at least one attachment */

/**
 * @file local_store.h
 * @brief Provider-native local storage with reverse digit bucketing and text indexes.
 *
 * Account layout (IMAP):
 *   ~/.local/share/email-cli/accounts/imap.<host>/
 *     store/<folder>/<d1>/<d2>/<uid>.eml
 *     headers/<folder>/<d1>/<d2>/<uid>.hdr
 *     index/from/<domain>/<localpart>     (one ref per line)
 *     index/date/<year>/<month>/<day>     (one ref per line)
 *
 * Reverse digit bucketing: d1 = uid % 10, d2 = (uid/10) % 10.
 * Reference format (IMAP): "<folder>/<uid>" per line.
 */

/**
 * @brief Initialises the account base path from an IMAP host URL.
 *
 * Must be called once before any other local_* function.
 * Extracts the hostname from the URL and sets the account directory.
 *
 * @param host_url  IMAP host URL (e.g. "imaps://mail.example.com:993").
 * @return 0 on success, -1 on failure.
 */
int local_store_init(const char *host_url);

/* ── Message store ───────────────────────────────────────────────────── */

/** @brief Checks whether a locally stored message exists. */
int local_msg_exists(const char *folder, int uid);

/** @brief Writes raw message content to the local store. */
int local_msg_save(const char *folder, int uid, const char *content, size_t len);

/** @brief Reads a locally stored message. Caller must free. */
char *local_msg_load(const char *folder, int uid);

/** @brief Deletes a locally stored message and its index entries. */
int local_msg_delete(const char *folder, int uid);

/* ── Header store ────────────────────────────────────────────────────── */

/** @brief Checks whether a locally stored header exists. */
int   local_hdr_exists(const char *folder, int uid);

/** @brief Writes header content to the local store. */
int   local_hdr_save(const char *folder, int uid, const char *content, size_t len);

/** @brief Reads a locally stored header. Caller must free. */
char *local_hdr_load(const char *folder, int uid);

/** @brief Removes header files whose UID is not in @p keep_uids. */
void  local_hdr_evict_stale(const char *folder,
                              const int *keep_uids, int keep_count);

/* ── Index ───────────────────────────────────────────────────────────── */

/**
 * @brief Updates from/ and date/ indexes for a stored message.
 *
 * Parses From and Date headers from raw_msg and appends the reference
 * "<folder>/<uid>" to the appropriate index files.
 * Duplicate references are silently skipped.
 *
 * @param folder   Mailbox folder name.
 * @param uid      IMAP UID.
 * @param raw_msg  Raw RFC 2822 message content.
 * @return 0 on success, -1 on error.
 */
int local_index_update(const char *folder, int uid, const char *raw_msg);

/* ── Folder manifest (fast list cache) ────────────────────────────────── */

typedef struct {
    int   uid;
    char *from;      /**< MIME-decoded, display-ready */
    char *subject;   /**< MIME-decoded, display-ready */
    char *date;      /**< Formatted "YYYY-MM-DD HH:MM" */
    int   flags;     /**< Bitmask of MSG_FLAG_* constants */
} ManifestEntry;

typedef struct {
    ManifestEntry *entries;
    int count;
    int capacity;
} Manifest;

/** @brief Loads the manifest for a folder. Returns NULL if not found. */
Manifest *manifest_load(const char *folder);

/** @brief Saves the manifest for a folder. Returns 0 on success. */
int manifest_save(const char *folder, const Manifest *m);

/** @brief Frees a manifest and all its entries. */
void manifest_free(Manifest *m);

/** @brief Finds an entry by UID. Returns pointer into manifest or NULL. */
ManifestEntry *manifest_find(const Manifest *m, int uid);

/** @brief Adds or updates a manifest entry (takes ownership of strings). */
void manifest_upsert(Manifest *m, int uid,
                     char *from, char *subject, char *date, int flags);

/** @brief Removes entries whose UID is not in keep_uids (sorted). */
void manifest_retain(Manifest *m, const int *keep_uids, int keep_count);

/* ── Folder list cache ───────────────────────────────────────────────── */

/**
 * @brief Saves the list of folder names to a local cache file.
 *
 * @param folders  Array of UTF-8 folder name strings.
 * @param count    Number of entries.
 * @param sep      Hierarchy separator character (e.g. '.').
 * @return 0 on success, -1 on error.
 */
int local_folder_list_save(const char **folders, int count, char sep);

/**
 * @brief Loads the cached folder list.
 *
 * @param count_out  Set to the number of folders returned.
 * @param sep_out    Set to the hierarchy separator (may be NULL).
 * @return Heap-allocated array of heap-allocated strings, or NULL if not cached.
 *         Caller must free each string and then the array.
 */
char **local_folder_list_load(int *count_out, char *sep_out);

/**
 * @brief Counts total and unseen messages in a folder from its manifest.
 *
 * Loads the manifest (if present) and counts entries.  If the manifest does
 * not exist both outputs are set to 0.
 *
 * @param folder     Folder name.
 * @param total_out  Set to total message count.
 * @param unseen_out Set to unseen message count.
 */
void manifest_count_folder(const char *folder, int *total_out, int *unseen_out);

/* ── UI preferences ──────────────────────────────────────────────────── */

/** @brief Reads an integer UI preference. */
int ui_pref_get_int(const char *key, int default_val);

/** @brief Writes an integer UI preference. */
int ui_pref_set_int(const char *key, int value);

#endif /* LOCAL_STORE_H */
