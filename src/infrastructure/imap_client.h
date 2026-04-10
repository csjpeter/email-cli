#ifndef IMAP_CLIENT_H
#define IMAP_CLIENT_H

#include <stddef.h>

/**
 * @file imap_client.h
 * @brief Minimal IMAP4rev1 client over TLS (OpenSSL) or plain TCP.
 *
 * Supports the operations needed by email-cli:
 *   - LIST "" "*"           — enumerate folders
 *   - SELECT <folder>       — open a folder
 *   - UID SEARCH <criteria> — find messages
 *   - UID FETCH <uid> BODY.PEEK[]        — fetch full message (no \Seen flag)
 *   - UID FETCH <uid> BODY.PEEK[HEADER]  — fetch headers only (no \Seen flag)
 *
 * Connection lifecycle:
 *   1. imap_connect()  — TCP connect, TLS handshake, read server greeting, LOGIN
 *   2. imap_select()   — SELECT a folder (required before SEARCH/FETCH)
 *   3. imap commands…
 *   4. imap_disconnect() — LOGOUT + close
 *
 * The URL format matches libcurl's IMAP convention:
 *   imaps://hostname           → TLS on port 993
 *   imaps://hostname:port      → TLS on custom port
 *   imap://hostname            → plain TCP on port 143
 *   imap://hostname:port       → plain TCP on custom port
 */

typedef struct ImapClient ImapClient;

/**
 * RAII cleanup helper — store pointer via RAII_IMAP so the client is
 * automatically disconnected when the variable goes out of scope.
 *
 * Usage:
 *   RAII_IMAP ImapClient *c = imap_connect(...);
 */
static inline void imap_disconnect_ptr(ImapClient **p);
#define RAII_IMAP __attribute__((cleanup(imap_disconnect_ptr)))

/**
 * @brief Connect to an IMAP server and authenticate.
 *
 * @param host_url   Server URL, e.g. "imaps://imap.example.com" or
 *                   "imaps://imap.example.com:993".
 * @param user       IMAP username.
 * @param pass       IMAP password.
 * @param verify_tls 1 = verify TLS certificate (production); 0 = accept
 *                   self-signed (test environments only).
 * @return New client handle, or NULL on failure.  Caller must call
 *         imap_disconnect() when done (or use RAII_IMAP).
 */
ImapClient *imap_connect(const char *host_url, const char *user,
                         const char *pass, int verify_tls);

/**
 * @brief Send LOGOUT and close the connection.
 * Safe to call with NULL.
 */
void imap_disconnect(ImapClient *c);

/**
 * @brief List all folders on the server (LIST "" "*").
 *
 * Folder names are decoded from IMAP Modified UTF-7 to UTF-8.
 *
 * @param c          Connected client handle.
 * @param folders_out  On success, set to a heap-allocated array of
 *                     heap-allocated folder name strings.  Caller frees each
 *                     element and then the array itself.
 * @param count_out  Number of folders returned.
 * @param sep_out    If non-NULL, set to the hierarchy separator character
 *                   (e.g. '.' or '/'); '.' if no separator found.
 * @return 0 on success, -1 on error.
 */
int imap_list(ImapClient *c, char ***folders_out, int *count_out, char *sep_out);

/**
 * @brief SELECT a folder.
 *
 * Must be called before imap_uid_search(), imap_uid_fetch_headers(), or
 * imap_uid_fetch_body().  Calling again with a different folder re-selects.
 *
 * @param c       Connected client handle.
 * @param folder  Folder name in UTF-8 (will be encoded to IMAP Modified UTF-7
 *                internally before sending to the server).
 * @return 0 on success, -1 on error.
 */
int imap_select(ImapClient *c, const char *folder);

/**
 * @brief UID SEARCH with caller-supplied criteria string.
 *
 * Typical criteria: "ALL", "UNSEEN".
 * Requires a folder to be selected first.
 *
 * @param c          Connected client handle.
 * @param criteria   IMAP search criteria string (e.g. "ALL", "UNSEEN").
 * @param uids_out   On success, set to a heap-allocated array of UIDs.
 *                   Caller must free().  Set to NULL if count is 0.
 * @param count_out  Number of UIDs returned (0 = empty result, not an error).
 * @return 0 on success (including empty result), -1 on error.
 */
int imap_uid_search(ImapClient *c, const char *criteria,
                    int **uids_out, int *count_out);

/**
 * @brief Fetch just the RFC 2822 headers for a message (BODY.PEEK[HEADER]).
 *
 * Does NOT set the \Seen flag.
 * Requires a folder to be selected first.
 *
 * @param c    Connected client handle.
 * @param uid  IMAP UID of the message.
 * @return Heap-allocated NUL-terminated string containing the raw headers,
 *         or NULL on error.  Caller must free().
 */
char *imap_uid_fetch_headers(ImapClient *c, int uid);

/**
 * @brief Fetch the complete RFC 2822 message (BODY.PEEK[]).
 *
 * Does NOT set the \Seen flag.
 * Requires a folder to be selected first.
 *
 * @param c    Connected client handle.
 * @param uid  IMAP UID of the message.
 * @return Heap-allocated NUL-terminated string containing the full raw message,
 *         or NULL on error.  Caller must free().
 */
char *imap_uid_fetch_body(ImapClient *c, int uid);

/* ── Inline RAII cleanup ─────────────────────────────────────────────── */

static inline void imap_disconnect_ptr(ImapClient **p) {
    if (p && *p) {
        imap_disconnect(*p);
        *p = NULL;
    }
}

#endif /* IMAP_CLIENT_H */
