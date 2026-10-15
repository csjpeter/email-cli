/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-15 */
/* Public interface of libemail-client: the mail client logic of email-cli. */
#ifndef EMAIL_CLIENT_H
#define EMAIL_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file email-client.h
 * @brief Send and read mail through IMAP, SMTP or the Gmail REST API.
 *
 * libemail-client holds the mail client logic of email-cli once, so that a
 * program of the workspace (oidc-server, infra-mgr) sends mail natively
 * instead of starting email-cli (policies/local-service-access-model.md, "The
 * client is the interface").
 *
 * - **No audit.** The library is a client of third-party mail servers; there
 *   is no service of ours behind it.  It returns the outcome and the message
 *   id (emc_send_result), and the calling program writes its own audit record.
 * - **No configuration files, no terminal.**  The caller hands the account in
 *   an emc_account; the library reads nothing from the user's home and never
 *   prompts.
 * - **TLS is mandatory** unless the caller asks for emc_account::insecure_tls
 *   (test servers with self-signed certificates only).
 * - A handle is not thread safe; use one handle per thread.
 *
 * The library does not open the network until an operation needs it.
 * Link with `pkg-config --cflags --libs libemail-client`.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Library version, "MAJOR.MINOR.PATCH" of the package it was built from. */
const char *emc_version(void);

/** Outcome of an operation. */
typedef enum {
    EMC_OK = 0,           /**< The operation succeeded. */
    EMC_ERR_ARGUMENT,     /**< A required argument is missing or invalid. */
    EMC_ERR_CONFIG,       /**< The account lacks what the operation needs. */
    EMC_ERR_CONNECT,      /**< The server could not be reached or refused the login. */
    EMC_ERR_SEND,         /**< The server did not accept the message. */
    EMC_ERR_PROTOCOL,     /**< The server answered something the client cannot use. */
    EMC_ERR_MEMORY        /**< Out of memory. */
} emc_status;

/** A short English description of @p status; never NULL. */
const char *emc_strerror(emc_status status);

/**
 * @brief The account an operation acts on.  Strings are copied by emc_open().
 *
 * IMAP accounts set @c host, @c user, @c pass; the SMTP fields default to the
 * IMAP ones (host derived: imaps://h becomes smtps://h).  Gmail accounts set
 * @c gmail and a refresh token.
 */
typedef struct {
    const char *host;        /**< IMAP URL, e.g. "imaps://imap.example.org". */
    const char *user;        /**< Login name, usually the address. */
    const char *pass;        /**< Password. */
    const char *folder;      /**< Default folder; NULL = "INBOX". */
    const char *sent_folder; /**< Folder a sent copy is appended to (IMAP); NULL = none. */
    const char *smtp_host;   /**< SMTP URL "smtps://…"; NULL = derive from @c host. */
    int         smtp_port;   /**< Port override; 0 = the protocol's default. */
    const char *smtp_user;   /**< SMTP login; NULL = @c user. */
    const char *smtp_pass;   /**< SMTP password; NULL = @c pass. */
    int         gmail;       /**< 1 = Gmail REST API (OAuth2), 0 = IMAP/SMTP. */
    const char *gmail_refresh_token; /**< OAuth2 refresh token (Gmail). */
    const char *gmail_client_id;     /**< OAuth2 client id; NULL = built-in. */
    const char *gmail_client_secret; /**< OAuth2 client secret; NULL = built-in. */
    int         insecure_tls; /**< 1 = do not verify the server certificate (tests only). */
} emc_account;

/** An opaque client handle. */
typedef struct emc_client emc_client;

/**
 * @brief Create a client for @p account.  No network traffic happens here.
 * @return EMC_OK, EMC_ERR_ARGUMENT (NULL arguments, no user and no host and
 *         not Gmail) or EMC_ERR_MEMORY.  On failure @p *out is NULL.
 */
emc_status emc_open(emc_client **out, const emc_account *account);

/** @brief Close the connection and free the handle.  NULL is allowed. */
void emc_close(emc_client *client);

/** @brief The description of the last failure of @p client ("" when none). */
const char *emc_last_error(const emc_client *client);

/** A message to send.  Text is UTF-8; the library encodes it for the wire. */
typedef struct {
    const char *from;        /**< Sender; NULL = the account's SMTP user or user. */
    const char *to;          /**< Recipient(s), comma separated; required. */
    const char *cc;          /**< Cc recipients, comma separated; may be NULL. */
    const char *bcc;         /**< Bcc recipients, comma separated; may be NULL. */
    const char *subject;     /**< Subject; required. */
    const char *body;        /**< Plain-text body; required. */
    const char *in_reply_to; /**< Message-ID being answered; may be NULL. */
    const char *const *attachments; /**< Paths of files to attach; may be NULL. */
    int         attachment_count;   /**< Number of entries in @c attachments. */
} emc_message;

/** What a send returns for the caller's own audit record. */
typedef struct {
    emc_status status;        /**< Outcome of the send. */
    char       message_id[256]; /**< The Message-ID the message carried; "" on failure. */
    int        saved_copy;    /**< 1 = a copy was appended to the sent folder. */
} emc_send_result;

/**
 * @brief Send @p message.
 *
 * IMAP accounts send through SMTP over TLS; Gmail accounts through the
 * Gmail API.  When the account names a sent folder, a copy is appended to it
 * after a successful IMAP send (a failure to do so leaves
 * emc_send_result::saved_copy 0 and does not fail the send).
 *
 * @param result  Receives the outcome and the message id; may be NULL.
 * @return the same status as @c result->status.
 */
emc_status emc_send(emc_client *client, const emc_message *message,
                    emc_send_result *result);

/**
 * @brief List the folders (IMAP) or labels (Gmail).
 * @param names  Receives a heap array of names; free with emc_free_list().
 * @param count  Receives the number of names.
 */
emc_status emc_list_folders(emc_client *client, char ***names, int *count);

/**
 * @brief List the message ids of a folder or label.
 * @param folder       Folder or label; NULL = the account's default.
 * @param unseen_only  1 = only unread messages.
 * @param ids          Receives a heap array of id strings; free with
 *                     emc_free_list().
 * @param count        Receives the number of ids.
 */
emc_status emc_list_messages(emc_client *client, const char *folder,
                             int unseen_only, char ***ids, int *count);

/**
 * @brief Fetch one message, complete and as received (RFC 2822).
 * @param raw  Receives a heap NUL-terminated string; release with free().
 */
emc_status emc_fetch_message(emc_client *client, const char *folder,
                             const char *id, char **raw);

/** @brief Free an array returned by emc_list_folders() or emc_list_messages(). */
void emc_free_list(char **items, int count);

/* ── Mail access: the full operation set ───────────────────────────────
 *
 * The functions below are what email-cli's CLI, TUI and sync daemon use for
 * all contact with a mail server.  Unless stated otherwise they return
 * EMC_OK or a failure status, and describe the failure in emc_last_error().
 * Messages are named by a 16-character identifier (@c char[17], with the
 * terminating NUL): a zero-padded decimal IMAP UID or a Gmail message id.
 */

/** Message flag bits, as emc_fetch_flags() and emc_flag_update report them. */
#define EMC_FLAG_UNSEEN    (1 << 0)  /**< \Seen is not set */
#define EMC_FLAG_FLAGGED   (1 << 1)  /**< \Flagged (starred) */
#define EMC_FLAG_DONE      (1 << 2)  /**< $Done keyword */
#define EMC_FLAG_ANSWERED  (1 << 4)  /**< \Answered */
#define EMC_FLAG_FORWARDED (1 << 5)  /**< $Forwarded */
#define EMC_FLAG_JUNK      (1 << 6)  /**< $Junk / SPAM */
#define EMC_FLAG_PHISHING  (1 << 7)  /**< $Phishing */

/** Which messages emc_search() finds. */
typedef enum {
    EMC_SEARCH_ALL,       /**< every message */
    EMC_SEARCH_UNREAD,    /**< IMAP UNSEEN; Gmail: the UNREAD label */
    EMC_SEARCH_FLAGGED,   /**< IMAP FLAGGED; Gmail: STARRED */
    EMC_SEARCH_DONE       /**< IMAP keyword $Done */
} emc_search_kind;

/** Result of emc_select_ext(); release vanished_uids with free(). */
typedef struct {
    uint32_t  uidvalidity;       /**< UIDVALIDITY of the folder */
    uint64_t  highestmodseq;     /**< HIGHESTMODSEQ (0 without CONDSTORE) */
    char    (*vanished_uids)[17]; /**< QRESYNC VANISHED set; NULL if none */
    int       vanished_count;    /**< entries in vanished_uids */
} emc_select_result;

/** One flag change, as emc_fetch_flags_changedsince() reports it. */
typedef struct {
    char uid[17];                /**< the message */
    int  flags;                  /**< EMC_FLAG_* bits */
} emc_flag_update;

/** Progress of a large download: bytes received of the announced total. */
typedef void (*emc_progress_fn)(size_t received, size_t total, void *ctx);

/** @brief Connect now (emc_open() does not).  EMC_ERR_CONNECT on failure. */
emc_status emc_connect(emc_client *client);

/** @brief 1 for a label-based account (Gmail), 0 for folders (IMAP). */
int emc_uses_labels(const emc_client *client);

/** @brief Install or clear (fn NULL) a download progress callback. */
void emc_set_progress(emc_client *client, emc_progress_fn fn, void *ctx);

/**
 * @brief List folders or labels.
 * @param sep  Receives the hierarchy separator ('/' for Gmail); may be NULL.
 */
emc_status emc_list_names(emc_client *client, char ***names, int *count, char *sep);

/** @brief List folders or labels with their ids (equal names for IMAP). */
emc_status emc_list_with_ids(emc_client *client, char ***names, char ***ids,
                             int *count);

/** @brief Select a folder (IMAP) or set the active label (Gmail). */
emc_status emc_select(emc_client *client, const char *name);

/**
 * @brief SELECT with CONDSTORE/QRESYNC where the server offers it.
 * @param known_uidval,known_modseq  Saved state, 0 on the first run.
 */
emc_status emc_select_ext(emc_client *client, const char *folder,
                          uint32_t known_uidval, uint64_t known_modseq,
                          emc_select_result *result);

/** @brief Search the selected folder; @p uids is released with free(). */
emc_status emc_search(emc_client *client, emc_search_kind kind,
                      char (**uids)[17], int *count);

/** @brief Raw headers of a message of the selected folder; NULL on failure. */
char *emc_fetch_headers(emc_client *client, const char *uid);

/** @brief The complete raw message of the selected folder; NULL on failure. */
char *emc_fetch_body(emc_client *client, const char *uid);

/** @brief EMC_FLAG_* bits of a message, or -1 on failure. */
int emc_fetch_flags(emc_client *client, const char *uid);

/** @brief Flag changes since @p modseq (IMAP; empty for Gmail). Free with free(). */
emc_status emc_fetch_flags_changedsince(emc_client *client, uint64_t modseq,
                                        emc_flag_update **updates, int *count);

/** @brief Set or clear an IMAP-style flag ("\\Seen", "\\Flagged", "$Done"). */
emc_status emc_set_flag(emc_client *client, const char *uid, const char *flag,
                        int add);
/** @brief Mark as junk (IMAP $Junk; Gmail SPAM label). */
emc_status emc_mark_junk(emc_client *client, const char *uid);
/** @brief Mark as not junk. */
emc_status emc_mark_notjunk(emc_client *client, const char *uid);
/** @brief Move to the trash. */
emc_status emc_trash(emc_client *client, const char *uid);
/** @brief Delete permanently. */
emc_status emc_delete(emc_client *client, const char *uid);
/** @brief Move to another IMAP folder (a no-op for Gmail). */
emc_status emc_move_to_folder(emc_client *client, const char *uid,
                              const char *target_folder);
/** @brief Add or remove a Gmail label (a no-op for IMAP). */
emc_status emc_modify_label(emc_client *client, const char *uid,
                            const char *label_id, int add);
/** @brief Create a Gmail label; @p id_out (may be NULL) is released with free(). */
emc_status emc_create_label(emc_client *client, const char *name, char **id_out);
/** @brief Delete a Gmail label by id. */
emc_status emc_delete_label(emc_client *client, const char *label_id);
/** @brief Create an IMAP folder. */
emc_status emc_create_folder(emc_client *client, const char *name);
/** @brief Delete an IMAP folder. */
emc_status emc_delete_folder(emc_client *client, const char *name);

/**
 * @brief Store a message: IMAP APPEND to @p folder; for Gmail the message is
 *        sent through the API (@p folder is ignored).
 */
emc_status emc_append(emc_client *client, const char *folder, const char *msg,
                      size_t msg_len);

/**
 * @brief Synchronise a Gmail account into the local store.
 * @param full  1 = reconcile everything, 0 = incremental by history id.
 *
 * IMAP accounts are synchronised folder by folder by the caller with the
 * operations above.
 */
emc_status emc_sync(emc_client *client, int full);

/** @brief Rebuild the label indexes of the local store of the account. */
emc_status emc_rebuild_indexes(const emc_account *account);

/**
 * @brief Authorize a Gmail account with the OAuth2 device flow (RFC 8628).
 *
 * Interactive: it prints the verification URL and code on standard output
 * and waits until the user has authorized.
 * @param refresh_token  Receives the new refresh token; release with free().
 */
emc_status emc_gmail_authorize(const emc_account *account, char **refresh_token);

/**
 * @brief Build the wire form (RFC 2822) of a message, with Date and
 *        Message-ID.  Release @p wire with free().
 */
emc_status emc_compose(const emc_message *message, char **wire, size_t *wire_len);

/**
 * @brief Send a message already in wire form.
 *
 * SMTP over TLS for IMAP accounts, the Gmail API for Gmail.  No copy is
 * saved; the caller decides what to do with it.
 * @param from,to  Envelope sender and recipient.
 * @param result   Outcome and the Message-ID of the wire message; may be NULL.
 */
emc_status emc_send_wire(emc_client *client, const char *from, const char *to,
                         const char *wire, size_t wire_len,
                         emc_send_result *result);

/**
 * @brief Reply metadata of a raw message: the address to answer, the subject
 *        with "Re: " and the Message-ID.  Release each with free().
 */
emc_status emc_reply_meta(const char *raw, char **reply_to, char **subject,
                          char **message_id);

/**
 * @brief Append the library's log to @p path.  Without a call nothing is
 *        logged to a file.  @p verbose 1 = debug level.  Errors are also
 *        written to standard error, as email-cli's own log does.
 */
void emc_log_open(const char *path, int verbose);

/** @brief Close the log. */
void emc_log_close(void);

/** @brief Automatic emc_close() of a handle leaving its scope. */
static inline void emc_close_cleanup(emc_client **c)
{
    if (c && *c) { emc_close(*c); *c = NULL; }
}
#define EMC_AUTO_CLOSE __attribute__((cleanup(emc_close_cleanup)))

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_CLIENT_H */
