#ifndef EMAIL_SERVICE_H
#define EMAIL_SERVICE_H

#include "config_store.h"

/**
 * @file email_service.h
 * @brief High-level service for listing and reading email messages.
 */

/**
 * @brief Lists unread (UNSEEN) messages in the configured folder.
 *
 * Performs UID SEARCH UNSEEN, then fetches the header block for each
 * message (BODY.PEEK[HEADER]) without marking them as \Seen.
 * Prints a table of UID, From, Subject, and Date to stdout.
 *
 * @param cfg  Pointer to the connection configuration.
 * @return 0 on success, -1 on failure.
 */
int email_service_list_unseen(const Config *cfg);

/**
 * @brief Reads and displays one message identified by its IMAP UID.
 *
 * Checks the local cache (~/.cache/email-cli/messages/<folder>/<uid>.eml)
 * first; fetches from the server only on a cache miss. The full RFC 2822
 * message is MIME-parsed and its readable plain-text body is printed.
 *
 * @param cfg  Pointer to the connection configuration.
 * @param uid  IMAP UID of the message to display.
 * @return 0 on success, -1 on failure.
 */
int email_service_read(const Config *cfg, int uid);

#endif /* EMAIL_SERVICE_H */
