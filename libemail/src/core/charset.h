/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* Conversion of text from a declared character set to UTF-8. */
#ifndef CHARSET_H
#define CHARSET_H

#include <stddef.h>

/**
 * @file charset.h
 * @brief Character set conversion that works in a fully static program.
 *
 * The common single-byte sets (ISO-8859-x, windows-125x, KOI8-R/U) are built in
 * (charset_tables.h), so a binary on a system that has none of the C library's
 * iconv modules still decodes the mail of the usual Western, Central
 * European, Cyrillic and Greek senders.  Any other set (the East Asian
 * multi-byte ones, UTF-16, ...) goes to the system's iconv, which a static
 * program can use only when the system provides it.
 */

typedef enum {
    CHARSET_OK,        /**< converted */
    CHARSET_UNKNOWN,   /**< neither built in nor known to iconv */
    CHARSET_INVALID    /**< known, but the bytes are not valid in it */
} CharsetStatus;

/**
 * @brief Convert @p len bytes of @p s from @p from_charset to UTF-8.
 * @param from_charset  Name as declared in a mail (case-insensitive, aliases
 *                      such as latin2 or cp1250 accepted).
 * @param out_len       Receives the length of the result; may be NULL.
 * @param status        Receives the outcome; may be NULL.
 * @return a malloc'd NUL-terminated UTF-8 string, or NULL when not converted.
 */
char *charset_decode(const char *from_charset, const char *s, size_t len,
                      size_t *out_len, CharsetStatus *status);

/** @brief 1 when @p from_charset is converted without the system's iconv. */
int charset_is_builtin(const char *from_charset);

#endif /* CHARSET_H */
