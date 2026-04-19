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
 * Reverse digit bucketing: d1 = uid[last char], d2 = uid[second-to-last char].
 * UIDs are fixed-width 16-character strings (IMAP: zero-padded decimal, Gmail: hex).
 * Reference format: "<folder>/<uid>" per line.
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
int local_store_init(const char *host_url, const char *username);

/* ── Message store ───────────────────────────────────────────────────── */

/** @brief Checks whether a locally stored message exists. */
int local_msg_exists(const char *folder, const char *uid);

/** @brief Writes raw message content to the local store. */
int local_msg_save(const char *folder, const char *uid, const char *content, size_t len);

/** @brief Reads a locally stored message. Caller must free. */
char *local_msg_load(const char *folder, const char *uid);

/** @brief Deletes a locally stored message and its index entries. */
int local_msg_delete(const char *folder, const char *uid);

/* ── Header store ────────────────────────────────────────────────────── */

/** @brief Checks whether a locally stored header exists. */
int   local_hdr_exists(const char *folder, const char *uid);

/** @brief Writes header content to the local store. */
int   local_hdr_save(const char *folder, const char *uid, const char *content, size_t len);

/** @brief Reads a locally stored header. Caller must free. */
char *local_hdr_load(const char *folder, const char *uid);

/** @brief Updates the flags integer (5th tab field) in a stored .hdr file. */
int   local_hdr_update_flags(const char *folder, const char *uid, int new_flags);

/** @brief Removes header files whose UID is not in @p keep_uids. */
void  local_hdr_evict_stale(const char *folder,
                              const char (*keep_uids)[17], int keep_count);

/**
 * @brief List all UIDs that have a .hdr file for the given folder.
 *
 * Scans all 100 bucket directories under headers/<folder>/ and collects
 * the UID (filename stem) for every .hdr file found.
 *
 * @param folder      IMAP folder name, or "" for Gmail.
 * @param uids_out    Set to heap-allocated array of char[17] UID strings (caller frees).
 * @param count_out   Set to the number of entries in *uids_out.
 * @return 0 on success, -1 on allocation failure.
 */
int local_hdr_list_all_uids(const char *folder,
                             char (**uids_out)[17], int *count_out);

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
int local_index_update(const char *folder, const char *uid, const char *raw_msg);

/* ── Folder manifest (fast list cache) ────────────────────────────────── */

typedef struct {
    char  uid[17];   /**< 16-char UID string + NUL (IMAP: zero-padded decimal, Gmail: hex) */
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
ManifestEntry *manifest_find(const Manifest *m, const char *uid);

/** @brief Adds or updates a manifest entry (takes ownership of strings). */
void manifest_upsert(Manifest *m, const char *uid,
                     char *from, char *subject, char *date, int flags);

/** @brief Removes entries whose UID is not in keep_uids (sorted). */
void manifest_retain(Manifest *m, const char (*keep_uids)[17], int keep_count);

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
 * @param total_out    Set to total message count.
 * @param unseen_out   Set to unseen message count.
 * @param flagged_out  Set to flagged message count.
 */
void manifest_count_folder(const char *folder, int *total_out,
                            int *unseen_out, int *flagged_out);

/* ── Pending flag changes (server sync queue) ────────────────────────── */

/**
 * One pending IMAP flag change that has not yet been pushed to the server.
 */
typedef struct {
    char uid[17];        /**< 16-char UID string + NUL */
    char flag_name[64];  /**< e.g. "\\Seen", "\\Flagged", "$Done" */
    int  add;            /**< 1 = add flag, 0 = remove flag */
} PendingFlag;

/** @brief Appends a flag change to the pending queue for a folder. */
int local_pending_flag_add(const char *folder, const char *uid,
                            const char *flag_name, int add);

/** @brief Loads all pending flag changes for a folder.
 *  Returns heap-allocated array (caller must free), or NULL if none. */
PendingFlag *local_pending_flag_load(const char *folder, int *count_out);

/** @brief Removes the pending flag queue file for a folder. */
void local_pending_flag_clear(const char *folder);

/* ── Gmail label index files (.idx) ──────────────────────────────────── */

/**
 * Label index files store sorted 16-char UIDs (one per line, 17 bytes each).
 * Path: accounts/<email>/labels/<label>.idx
 *
 * Binary search: O(log N) via fixed 17-byte records.
 * A message with multiple labels appears in multiple .idx files.
 */

/** @brief Checks whether a UID is in a label's index (O(log N) binary search). */
int label_idx_contains(const char *label, const char *uid);

/** @brief Inserts a UID into a label's sorted index. No-op if already present. */
int label_idx_add(const char *label, const char *uid);

/** @brief Removes a UID from a label's index. No-op if not present. */
int label_idx_remove(const char *label, const char *uid);

/** @brief Returns the count of UIDs in a label's index. */
int label_idx_count(const char *label);

/**
 * @brief Count UIDs present in both label_a's index and a pre-loaded sorted array.
 *
 * Performs an O(N+M) merge-join.  Load the second array once with
 * label_idx_load() and reuse it across many calls.
 *
 * @param label_a   Label name — its .idx is loaded from disk.
 * @param b_uids    Pre-loaded, sorted UID array for the second label.
 * @param b_count   Number of entries in b_uids.
 * @return Number of UIDs common to both indexes, or 0 on error / empty.
 */
int label_idx_intersect_count(const char *label_a,
                               const char (*b_uids)[17], int b_count);

/**
 * @brief Load all UIDs from a label index.
 *
 * @param label      Label name (e.g. "INBOX", "Work", "_nolabel").
 * @param uids_out   Set to heap-allocated array of char[17]. Caller must free().
 * @param count_out  Set to number of UIDs.
 * @return 0 on success, -1 on error.
 */
int label_idx_load(const char *label, char (**uids_out)[17], int *count_out);

/**
 * @brief Write a complete sorted label index from an array of UIDs.
 *
 * Overwrites the existing .idx file.  The input array must already be sorted.
 *
 * @param label  Label name.
 * @param uids   Sorted array of 16-char UID strings.
 * @param count  Number of UIDs.
 * @return 0 on success, -1 on error.
 */
int label_idx_write(const char *label, const char (*uids)[17], int count);

/**
 * @brief Extract the labels field from a Gmail .hdr file.
 *
 * The .hdr format is: from\tsubject\tdate\tlabels\tflags
 * Returns a heap-allocated copy of the labels field (4th tab-separated field),
 * or NULL if the header is not in Gmail format.  Caller must free().
 *
 * @param folder  Folder/label name (empty string for Gmail flat store).
 * @param uid     16-char UID string.
 * @return Comma-separated labels string, or NULL.
 */
char *local_hdr_get_labels(const char *folder, const char *uid);

/**
 * @brief List all label names that have .idx files in the labels/ directory.
 *
 * Scans accounts/<email>/labels/ and returns label names (without .idx extension).
 *
 * @param labels_out  Set to heap-allocated array of strings. Caller frees each + array.
 * @param count_out   Set to number of labels.
 * @return 0 on success, -1 on error.
 */
int label_idx_list(char ***labels_out, int *count_out);

/**
 * @brief Save pre-trash labels for a message (for untrash restore).
 *
 * Before trashing, the comma-separated label string is saved so that
 * untrash can restore the original labels.
 *
 * @param uid     16-char UID string.
 * @param labels  Comma-separated label string (from .hdr).
 * @return 0 on success, -1 on error.
 */
int local_trash_labels_save(const char *uid, const char *labels);

/**
 * @brief Load pre-trash labels for untrash restore.
 * @return Heap-allocated comma-separated label string, or NULL. Caller frees.
 */
char *local_trash_labels_load(const char *uid);

/** @brief Remove pre-trash labels file after successful untrash. */
void local_trash_labels_remove(const char *uid);

/**
 * @brief Saves the gmail_history_id for incremental sync.
 * @return 0 on success, -1 on error.
 */
int local_gmail_history_save(const char *history_id);

/**
 * @brief Loads the gmail_history_id. Returns heap-allocated string or NULL.
 */
char *local_gmail_history_load(void);

/**
 * @brief Saves Gmail label ID→display-name mapping to local cache.
 * Format: one line per label: "<id>\t<name>\n"
 * @return 0 on success, -1 on error.
 */
int local_gmail_label_names_save(char **ids, char **names, int count);

/**
 * @brief Look up a Gmail label display name by ID from local cache.
 * Returns heap-allocated display name, or NULL if not found. Caller must free().
 */
char *local_gmail_label_name_lookup(const char *id);

/**
 * @brief Look up a Gmail label ID by display name from local cache (reverse lookup).
 * Returns heap-allocated label ID, or NULL if not found. Caller must free().
 */
char *local_gmail_label_id_lookup(const char *name);

/* ── UI preferences ──────────────────────────────────────────────────── */

/** @brief Reads an integer UI preference. */
int ui_pref_get_int(const char *key, int default_val);

/** @brief Writes an integer UI preference. */
int ui_pref_set_int(const char *key, int value);

/** @brief Reads a string UI preference.  Returns heap-allocated string or NULL. Caller must free(). */
char *ui_pref_get_str(const char *key);

/** @brief Writes a string UI preference. */
int ui_pref_set_str(const char *key, const char *value);

#endif /* LOCAL_STORE_H */
