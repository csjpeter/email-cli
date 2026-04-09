#ifndef IMAP_UTIL_H
#define IMAP_UTIL_H

/**
 * @file imap_util.h
 * @brief IMAP protocol utilities (Modified UTF-7 decoding, RFC 3501 §5.1.3).
 */

/**
 * @brief Decodes an IMAP Modified UTF-7 encoded string to UTF-8.
 *
 * IMAP folder names are transmitted in Modified UTF-7 where non-ASCII
 * characters are encoded as &<modified-base64>-.  The modified base64
 * alphabet replaces '/' with ',' and omits '=' padding.
 *
 * @param s  NUL-terminated Modified UTF-7 string (may be NULL).
 * @return   Heap-allocated UTF-8 string, or NULL on allocation failure.
 *           Caller must free() the result.
 */
char *imap_utf7_decode(const char *s);

/**
 * @brief Encodes a UTF-8 string to IMAP Modified UTF-7 (RFC 3501 §5.1.3).
 *
 * Printable US-ASCII characters (0x20-0x7E) except '&' are passed through
 * unchanged.  '&' is escaped as "&-".  All other characters are encoded
 * as UTF-16BE in the modified Base64 alphabet, wrapped in &...-.
 *
 * Use this before inserting a folder name into an IMAP URL path so that
 * servers which do not support RFC 6855 UTF8=ACCEPT still recognise the
 * mailbox name.
 *
 * @param s  NUL-terminated UTF-8 string (may be NULL).
 * @return   Heap-allocated Modified UTF-7 string, or NULL on allocation
 *           failure.  Caller must free() the result.
 */
char *imap_utf7_encode(const char *s);

#endif /* IMAP_UTIL_H */
