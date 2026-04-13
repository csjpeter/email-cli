#ifndef COMPOSE_SERVICE_H
#define COMPOSE_SERVICE_H

#include <stddef.h>

/**
 * @file compose_service.h
 * @brief Build RFC 2822 messages and extract reply/forward metadata.
 */

/**
 * @brief Parameters for building an outgoing message.
 */
typedef struct {
    const char *from;           /**< Sender address (e.g. "user@example.com"). */
    const char *to;             /**< Recipient address. */
    const char *subject;        /**< Subject line (UTF-8, unencoded). */
    const char *body;           /**< Plain-text body (UTF-8, may use LF or CRLF). */
    const char *reply_to_msg_id; /**< Message-ID of the original message, for In-Reply-To.
                                      NULL for new (non-reply) messages. */
} ComposeParams;

/**
 * @brief Build a complete RFC 2822 message from ComposeParams.
 *
 * Generates Date, Message-ID, MIME-Version, and Content-Type headers.
 * Body lines are converted from LF to CRLF per RFC 2822.
 *
 * @param p       Message parameters (all fields except reply_to_msg_id are required).
 * @param out     On success, set to a heap-allocated NUL-terminated RFC 2822 message.
 *                Caller must free().
 * @param outlen  On success, set to the byte length of the message (excluding NUL).
 * @return 0 on success, -1 on error.
 */
int compose_build_message(const ComposeParams *p, char **out, size_t *outlen);

/**
 * @brief Extract reply metadata from a raw RFC 2822 message.
 *
 * Parses From (or Reply-To), Subject, and Message-ID from raw_msg headers.
 * The subject is prefixed with "Re: " (duplicates removed).
 * Caller must free all non-NULL output strings.
 *
 * @param raw_msg        Raw RFC 2822 message.
 * @param reply_to_out   Set to the reply-to address (Reply-To or From header).
 * @param subject_out    Set to the reply subject ("Re: <original subject>").
 * @param msg_id_out     Set to the original Message-ID (for In-Reply-To).
 * @return 0 on success, -1 on error.
 */
int compose_extract_reply_meta(const char *raw_msg,
                                char **reply_to_out,
                                char **subject_out,
                                char **msg_id_out);

#endif /* COMPOSE_SERVICE_H */
