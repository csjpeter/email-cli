#ifndef MIME_UTIL_H
#define MIME_UTIL_H

/**
 * @file mime_util.h
 * @brief RFC 2822 header extraction and MIME body parsing utilities.
 */

/**
 * @brief Extracts the value of a named header field from a raw RFC 2822 message.
 *
 * Case-insensitive header name lookup. Stops searching at the blank line that
 * separates headers from the message body.
 *
 * @param msg  Raw message string (headers + optional body).
 * @param name Header field name without the trailing colon (e.g. "Subject").
 * @return Heap-allocated trimmed value, or NULL if not found. Caller must free.
 */
char *mime_get_header(const char *msg, const char *name);

/**
 * @brief Extracts a human-readable plain-text body from a raw RFC 2822 message.
 *
 * Handles:
 * - Simple text/plain messages.
 * - Content-Transfer-Encoding: base64 and quoted-printable.
 * - multipart/mixed and multipart/alternative (returns the first text/plain part).
 * - text/html fallback: strips HTML tags if no text/plain part is found.
 *
 * @param msg  Raw message string.
 * @return Heap-allocated text, or NULL on failure. Caller must free.
 */
char *mime_get_text_body(const char *msg);

/**
 * @brief Formats an RFC 2822 date string as "YYYY-MM-DD HH:MM" in local timezone.
 *
 * Supports dates with and without the leading day-of-week field.
 * Converts from the source timezone offset to the local system timezone.
 * If parsing fails, returns a heap-allocated copy of the raw input string.
 *
 * @param date RFC 2822 date string (e.g. "Tue, 10 Mar 2026 15:07:40 +0000 (UTC)").
 * @return Heap-allocated formatted string, or NULL if date is NULL. Caller must free.
 */
char *mime_format_date(const char *date);

/**
 * @brief Extracts the literal content from an IMAP FETCH response.
 *
 * Parses the `{size}` octet-count literal notation produced by IMAP servers:
 *   * N FETCH (BODY[...] {size}\r\n<content>\r\n)
 *
 * @param response  Raw IMAP response string.
 * @return Heap-allocated content of exactly 'size' bytes, or NULL. Caller must free.
 */
char *mime_extract_imap_literal(const char *response);

#endif /* MIME_UTIL_H */
