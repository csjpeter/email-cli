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

#endif /* IMAP_UTIL_H */
