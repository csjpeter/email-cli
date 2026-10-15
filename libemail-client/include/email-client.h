/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-15 */
/* Public interface of libemail-client: the mail client logic of email-cli. */
#ifndef EMAIL_CLIENT_H
#define EMAIL_CLIENT_H

#include <stddef.h>

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

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_CLIENT_H */
