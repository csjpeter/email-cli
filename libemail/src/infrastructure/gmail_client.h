#ifndef GMAIL_CLIENT_H
#define GMAIL_CLIENT_H

#include "config.h"
#include <stddef.h>

/**
 * @file gmail_client.h
 * @brief Gmail REST API v1 client.
 *
 * Thin wrapper around the Gmail API endpoints needed by email-cli.
 * Uses libcurl for HTTP and json_util for response parsing.
 * Authentication is handled via gmail_auth (OAuth2 token refresh).
 */

typedef struct GmailClient GmailClient;

/** RAII cleanup helper. */
static inline void gmail_disconnect_ptr(GmailClient **p);
#define RAII_GMAIL __attribute__((cleanup(gmail_disconnect_ptr)))

/**
 * @brief Connect to Gmail API and obtain a valid access token.
 *
 * Refreshes the OAuth2 access token from the stored refresh_token.
 * If the refresh token is invalid, returns NULL (caller should trigger
 * re-authorization via gmail_auth_device_flow).
 *
 * @param cfg  Config with gmail_refresh_token set.
 * @return New client handle, or NULL on failure.
 */
GmailClient *gmail_connect(Config *cfg);

/** @brief Free the client handle. Safe to call with NULL. */
void gmail_disconnect(GmailClient *c);

/**
 * @brief List all labels (system + user).
 *
 * @param c          Connected client handle.
 * @param names_out  Heap-allocated array of label display names. Caller frees each + array.
 * @param ids_out    Heap-allocated array of label API IDs (parallel to names). Caller frees each + array.
 * @param count_out  Number of labels returned.
 * @return 0 on success, -1 on error.
 */
int gmail_list_labels(GmailClient *c, char ***names_out,
                      char ***ids_out, int *count_out);

/**
 * @brief List message IDs matching a label (or all messages).
 *
 * Paginates automatically; returns all matching UIDs.
 *
 * @param c          Connected client handle.
 * @param label_id   Label ID filter (e.g. "INBOX"), or NULL for all messages.
 * @param query      Optional Gmail search query (e.g. "has:nouserlabels -in:sent"),
 *                   or NULL for no additional filter.
 * @param uids_out   Heap-allocated array of 16-char UID strings. Caller must free().
 * @param count_out  Number of UIDs returned.
 * @return 0 on success, -1 on error.
 */
int gmail_list_messages(GmailClient *c, const char *label_id,
                        const char *query,
                        char (**uids_out)[17], int *count_out);

/**
 * @brief Fetch a message: decoded RFC 2822 body + label IDs.
 *
 * Uses format=raw to retrieve the full message.  The base64url-encoded
 * body is decoded and returned.  If labels_out is non-NULL, the message's
 * labelIds array is also returned.
 *
 * @param c               Connected client handle.
 * @param uid             16-char message ID.
 * @param labels_out      Optional: set to heap-allocated array of label ID strings.
 * @param label_count_out Optional: set to number of labels.
 * @return Heap-allocated decoded RFC 2822 message, or NULL on error.
 *         Caller must free().
 */
char *gmail_fetch_message(GmailClient *c, const char *uid,
                          char ***labels_out, int *label_count_out);

/**
 * @brief Modify labels on a message.
 *
 * @param c              Connected client handle.
 * @param uid            16-char message ID.
 * @param add_labels     Array of label IDs to add (may be NULL if add_count==0).
 * @param add_count      Number of labels to add.
 * @param remove_labels  Array of label IDs to remove (may be NULL).
 * @param remove_count   Number of labels to remove.
 * @return 0 on success, -1 on error.
 */
int gmail_modify_labels(GmailClient *c, const char *uid,
                        const char **add_labels, int add_count,
                        const char **remove_labels, int remove_count);

/**
 * @brief Move a message to Trash (compound: removes all labels, adds TRASH).
 *
 * @return 0 on success, -1 on error.
 */
int gmail_trash(GmailClient *c, const char *uid);

/**
 * @brief Remove a message from Trash.
 *
 * Note: does NOT restore original labels — only removes TRASH.
 *
 * @return 0 on success, -1 on error.
 */
int gmail_untrash(GmailClient *c, const char *uid);

/**
 * @brief Send a message via the Gmail API.
 *
 * The raw RFC 2822 message is base64url-encoded and sent.
 * Gmail auto-adds the SENT label.
 *
 * @param c        Connected client handle.
 * @param raw_msg  Raw RFC 2822 message bytes.
 * @param len      Length of raw_msg.
 * @return 0 on success, -1 on error.
 */
int gmail_send(GmailClient *c, const char *raw_msg, size_t len);

/**
 * @brief Fetch sync history since the given historyId.
 *
 * Used for incremental synchronisation.  Caller must process the
 * returned JSON (contains messagesAdded, messagesDeleted, etc.).
 *
 * @param c           Connected client handle.
 * @param history_id  Starting historyId string.
 * @return Heap-allocated JSON response body, or NULL on error.
 *         Caller must free().
 */
char *gmail_get_history(GmailClient *c, const char *history_id);

/* ── Progress callback ────────────────────────────────────────────── */

typedef void (*GmailProgressFn)(size_t current, size_t total, void *ctx);

/** @brief Install a progress callback (called during list/sync operations). */
void gmail_set_progress(GmailClient *c, GmailProgressFn fn, void *ctx);

/* ── Inline RAII cleanup ──────────────────────────────────────────── */

static inline void gmail_disconnect_ptr(GmailClient **p) {
    if (p && *p) {
        gmail_disconnect(*p);
        *p = NULL;
    }
}

#endif /* GMAIL_CLIENT_H */
