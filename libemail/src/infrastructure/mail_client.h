#ifndef MAIL_CLIENT_H
#define MAIL_CLIENT_H

#include "config.h"
#include "imap_client.h"
#include <stddef.h>

/**
 * @file mail_client.h
 * @brief Unified dispatch layer for IMAP and Gmail backends.
 *
 * Sits between email_service.c and the protocol-specific clients.
 * cfg->gmail_mode selects the backend at connect time.
 */

typedef struct MailClient MailClient;

/** Semantic search criteria (protocol-independent). */
typedef enum {
    MAIL_SEARCH_ALL,
    MAIL_SEARCH_UNREAD,    /**< IMAP: UNSEEN; Gmail: has UNREAD label */
    MAIL_SEARCH_FLAGGED,   /**< IMAP: FLAGGED; Gmail: has STARRED label */
    MAIL_SEARCH_DONE,      /**< IMAP: KEYWORD $Done; Gmail: not used yet */
} MailSearchCriteria;

/** RAII cleanup helper. */
static inline void mail_client_free_ptr(MailClient **p);
#define RAII_MAIL __attribute__((cleanup(mail_client_free_ptr)))

/**
 * @brief Connect to the mail backend specified by cfg.
 *
 * If cfg->gmail_mode: uses Gmail REST API (OAuth2).
 * Otherwise: uses IMAP over TLS.
 *
 * @param cfg  Account configuration (borrowed, not copied).
 * @return New client handle, or NULL on failure.
 */
MailClient *mail_client_connect(Config *cfg);

/** @brief Disconnect and free the client. Safe to call with NULL. */
void mail_client_free(MailClient *c);

/** @brief Returns 1 for Gmail (label-based), 0 for IMAP (folder-based). */
int mail_client_uses_labels(const MailClient *c);

/**
 * @brief List folders (IMAP) or labels (Gmail).
 *
 * @param c          Connected client.
 * @param names_out  Heap-allocated array of name strings. Caller frees each + array.
 * @param count_out  Number of entries.
 * @param sep_out    Hierarchy separator (IMAP only; '/' for Gmail). May be NULL.
 * @return 0 on success, -1 on error.
 */
int mail_client_list(MailClient *c, char ***names_out, int *count_out, char *sep_out);

/**
 * @brief Select a folder (IMAP) or set the active label filter (Gmail).
 *
 * Must be called before search/fetch operations.
 */
int mail_client_select(MailClient *c, const char *name);

/**
 * @brief Search for messages matching criteria within the selected folder/label.
 *
 * @param c          Connected client.
 * @param criteria   Semantic search criteria.
 * @param uids_out   Heap-allocated array of 16-char UID strings. Caller must free().
 * @param count_out  Number of UIDs.
 * @return 0 on success, -1 on error.
 */
int mail_client_search(MailClient *c, MailSearchCriteria criteria,
                       char (**uids_out)[17], int *count_out);

/**
 * @brief Fetch raw RFC 2822 headers for a message.
 * @return Heap-allocated headers string, or NULL. Caller must free().
 */
char *mail_client_fetch_headers(MailClient *c, const char *uid);

/**
 * @brief Fetch the complete raw RFC 2822 message.
 * @return Heap-allocated message string, or NULL. Caller must free().
 */
char *mail_client_fetch_body(MailClient *c, const char *uid);

/**
 * @brief Fetch IMAP flags / Gmail label-derived flags as bitmask.
 * @return MSG_FLAG_* bitmask, or -1 on error.
 */
int mail_client_fetch_flags(MailClient *c, const char *uid);

/**
 * @brief Set or clear a flag on a message.
 *
 * IMAP: sends UID STORE.
 * Gmail: translates flag to label modify (e.g. \\Seen → remove UNREAD).
 *
 * @param flag  IMAP-style flag name (e.g. "\\Seen", "\\Flagged", "$Done").
 * @param add   1 = add, 0 = remove.
 */
int mail_client_set_flag(MailClient *c, const char *uid,
                         const char *flag, int add);

/**
 * @brief Move a message to Trash.
 *
 * IMAP: sets \\Deleted flag (caller may need EXPUNGE).
 * Gmail: compound trash operation (removes all labels, adds TRASH).
 */
int mail_client_trash(MailClient *c, const char *uid);

/**
 * @brief Append/send a message.
 *
 * IMAP: APPEND to the specified folder.
 * Gmail: messages.send() via REST API (folder parameter ignored).
 */
int mail_client_append(MailClient *c, const char *folder,
                       const char *msg, size_t msg_len);

/**
 * @brief List folders (IMAP) or labels (Gmail), returning both display names and IDs.
 * For IMAP, names and IDs are the same string.
 * @param names_out  Heap-alloc'd array of display names. Caller frees each + array.
 * @param ids_out    Heap-alloc'd array of IDs (parallel). Caller frees each + array.
 * @param count_out  Number of entries.
 * @return 0 on success, -1 on error.
 */
int mail_client_list_with_ids(MailClient *c, char ***names_out,
                              char ***ids_out, int *count_out);

/**
 * @brief Create a label (Gmail) or folder (IMAP).
 * @param name   Display name / folder path.
 * @param id_out Optional: receives new label ID (Gmail only). Caller frees.
 * @return 0 on success, -1 on error.
 */
int mail_client_create_label(MailClient *c, const char *name, char **id_out);

/**
 * @brief Delete a label (Gmail) or folder (IMAP).
 * @param label_id  Label ID for Gmail; folder name for IMAP.
 * @return 0 on success, -1 on error.
 */
int mail_client_delete_label(MailClient *c, const char *label_id);

/**
 * @brief Add or remove a label on a Gmail message (no-op for IMAP).
 *
 * @param label_id  Gmail label ID (e.g. "INBOX", "STARRED", "Work").
 * @param add       1 = add label, 0 = remove label.
 * @return 0 on success, -1 on error. Always returns 0 for IMAP (no-op).
 */
int mail_client_modify_label(MailClient *c, const char *uid,
                             const char *label_id, int add);

/**
 * @brief Install a progress callback for large downloads.
 */
void mail_client_set_progress(MailClient *c, ImapProgressFn fn, void *ctx);

/**
 * @brief Synchronise the account (Gmail: full/incremental sync).
 *
 * For IMAP this is a no-op (sync is handled by email_service_sync).
 * For Gmail this delegates to gmail_sync().
 */
int mail_client_sync(MailClient *c);

/* ── Inline RAII cleanup ─────────────────────────────────────��────── */

static inline void mail_client_free_ptr(MailClient **p) {
    if (p && *p) {
        mail_client_free(*p);
        *p = NULL;
    }
}

#endif /* MAIL_CLIENT_H */
