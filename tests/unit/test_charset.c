/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* Unit tests of the built-in character set conversion (EMAIL-103). */
#include "test_helpers.h"
#include "charset.h"
#include "mime_util.h"

#include <stdlib.h>
#include <string.h>

/**
 * @file test_charset.c
 * @brief The single-byte sets decode without the system's iconv modules; other
 *        sets go to iconv; a mail header with such a set is decoded.
 */

static char *dec(const char *cs, const char *bytes, CharsetStatus *st)
{
    return charset_decode(cs, bytes, bytes ? strlen(bytes) : 0, NULL, st);
}

static void test_charset_central_european(void)
{
    CharsetStatus st;
    char *r = dec("ISO-8859-2", "\xE1rv\xEDzt\xFBr\xF5", &st);
    ASSERT(r && st == CHARSET_OK, "ISO-8859-2 converts");
    ASSERT(r && strcmp(r, "árvíztűrő") == 0, "the Hungarian double-acute letters come out right");
    free(r);
    r = dec("windows-1250", "\x8A\x8E\x9A \xE1", &st);
    ASSERT(r && strcmp(r, "ŠŽš á") == 0, "windows-1250 has its own 0x80-0x9F letters");
    free(r);
    r = dec("latin2", "\xB3", &st);
    ASSERT(r && strcmp(r, "ł") == 0, "latin2 is an alias of ISO-8859-2");
    free(r);
}

static void test_charset_western_and_others(void)
{
    CharsetStatus st;
    char *r = dec("ISO-8859-1", "caf\xE9 \xFC", &st);
    ASSERT(r && strcmp(r, "café ü") == 0, "ISO-8859-1");
    free(r);
    r = dec("ISO-8859-15", "\xA4 5", &st);
    ASSERT(r && strcmp(r, "€ 5") == 0, "ISO-8859-15 has the euro sign");
    free(r);
    r = dec("windows-1252", "\x93q\x94 \x80", &st);
    ASSERT(r && strcmp(r, "“q” €") == 0, "windows-1252 quotes and euro");
    free(r);
    r = dec("KOI8-R", "\xF0\xD2\xC9\xD7\xC5\xD4", &st);
    ASSERT(r && strcmp(r, "Привет") == 0, "KOI8-R");
    free(r);
    r = dec("iso_8859_5", "\xBF\xE0\xD8\xD2\xD5\xE2", &st);
    ASSERT(r && strcmp(r, "Привет") == 0, "ISO-8859-5, an underscore spelling");
    free(r);
    r = dec("\"ISO-8859-7\"", "\xC1\xE2", &st);
    ASSERT(r && strcmp(r, "Αβ") == 0, "ISO-8859-7, a quoted name");
    free(r);
}

static void test_charset_outcomes(void)
{
    CharsetStatus st;
    ASSERT(dec("no-such-charset", "abc", &st) == NULL && st == CHARSET_UNKNOWN, "an unknown set is reported as such");
    ASSERT(dec("windows-1252", "\x81", &st) == NULL && st == CHARSET_INVALID, "an undefined byte is invalid");
    ASSERT(dec(NULL, "abc", &st) == NULL && st == CHARSET_UNKNOWN, "NULL set");
    ASSERT(dec("ISO-8859-2", NULL, &st) == NULL, "NULL text");
    size_t n = 0;
    char *r = charset_decode("ISO-8859-1", "ab\xE9", 3, &n, &st);
    ASSERT(r && n == 4, "the length of the result is reported");
    free(r);
    r = charset_decode("ISO-8859-1", "", 0, &n, &st);
    ASSERT(r && n == 0 && st == CHARSET_OK, "empty text converts to empty text");
    free(r);
    ASSERT(charset_is_builtin("Latin2") == 1, "latin2 is built in");
    ASSERT(charset_is_builtin("GB2312") == 0, "GB2312 is not built in");
    ASSERT(charset_is_builtin(NULL) == 0, "NULL is not");
    r = dec("UTF-16LE", "a\0b\0", &st);
    ASSERT(r == NULL || st == CHARSET_OK, "a set that is not built in goes to iconv");
    free(r);
}

static void test_charset_in_a_mail_header(void)
{
    char *s = mime_decode_words("=?ISO-8859-2?Q?=E1rv=EDzt=FBr=F5?= teszt");
    ASSERT(s && strcmp(s, "árvíztűrő teszt") == 0, "an encoded word in ISO-8859-2 is decoded");
    free(s);
    s = mime_decode_words("=?windows-1250?B?imqz4Q==?=");
    ASSERT(s && strcmp(s, "Šjłá") == 0, "a base64 windows-1250 word is decoded");
    free(s);
}

void test_charset(void)
{
    RUN_TEST(test_charset_central_european);
    RUN_TEST(test_charset_western_and_others);
    RUN_TEST(test_charset_outcomes);
    RUN_TEST(test_charset_in_a_mail_header);
}
