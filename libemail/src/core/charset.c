/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* Conversion of text from a declared character set to UTF-8. */
#include "charset.h"
#include "charset_tables.h"

#include <ctype.h>
#include <iconv.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file charset.c
 * @brief See charset.h.
 */

typedef struct {
    const char     *names;     /* space separated, upper case, normalised */
    const uint16_t *table;     /* 0x80-0xFF, NULL = identity (ISO-8859-1) */
} Single;

/* Names are compared after upper-casing; '_' and ' ' count as '-'. */
static const Single k_sets[] = {
    { "ISO-8859-1 LATIN1 L1 ISO-IR-100 IBM819 CP819",            cs_iso_8859_1 },
    { "ISO-8859-2 LATIN2 L2 ISO-IR-101",                         cs_iso_8859_2 },
    { "ISO-8859-3 LATIN3 L3 ISO-IR-109",                         cs_iso_8859_3 },
    { "ISO-8859-4 LATIN4 L4 ISO-IR-110",                         cs_iso_8859_4 },
    { "ISO-8859-5 CYRILLIC ISO-IR-144",                          cs_iso_8859_5 },
    { "ISO-8859-6 ARABIC ISO-IR-127",                            cs_iso_8859_6 },
    { "ISO-8859-7 GREEK ISO-IR-126",                             cs_iso_8859_7 },
    { "ISO-8859-8 HEBREW ISO-IR-138",                            cs_iso_8859_8 },
    { "ISO-8859-9 LATIN5 L5 ISO-IR-148",                         cs_iso_8859_9 },
    { "ISO-8859-10 LATIN6 L6 ISO-IR-157",                        cs_iso_8859_10 },
    { "ISO-8859-13 LATIN7 L7",                                   cs_iso_8859_13 },
    { "ISO-8859-15 LATIN-9 LATIN9 L9",                           cs_iso_8859_15 },
    { "ISO-8859-16 LATIN10 L10",                                 cs_iso_8859_16 },
    { "WINDOWS-1250 CP1250 MS-EE",                               cs_windows_1250 },
    { "WINDOWS-1251 CP1251 MS-CYRL",                             cs_windows_1251 },
    { "WINDOWS-1252 CP1252 MS-ANSI",                             cs_windows_1252 },
    { "WINDOWS-1253 CP1253 MS-GREEK",                            cs_windows_1253 },
    { "WINDOWS-1254 CP1254 MS-TURK",                             cs_windows_1254 },
    { "WINDOWS-1255 CP1255 MS-HEBR",                             cs_windows_1255 },
    { "WINDOWS-1256 CP1256 MS-ARAB",                             cs_windows_1256 },
    { "WINDOWS-1257 CP1257 WINBALTRIM",                          cs_windows_1257 },
    { "WINDOWS-1258 CP1258",                                     cs_windows_1258 },
    { "KOI8-R CSKOI8R",                                          cs_koi8_r },
    { "KOI8-U",                                                  cs_koi8_u },
};

static void normalise(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    while (*in == ' ' || *in == '"' || *in == '\'') in++;
    for (; *in && n + 1 < cap; in++) {
        char c = (char)toupper((unsigned char)*in);
        if (c == '_' || c == ' ') c = '-';
        out[n++] = c;
    }
    while (n && (out[n - 1] == '"' || out[n - 1] == '\'' || out[n - 1] == '-')) n--;
    out[n] = '\0';
}

static const Single *find_single(const char *name)
{
    char want[64];
    normalise(name, want, sizeof(want));
    size_t wl = strlen(want);
    if (wl == 0) return NULL;
    for (size_t i = 0; i < sizeof(k_sets) / sizeof(k_sets[0]); i++) {
        const char *p = k_sets[i].names;
        while (*p) {
            size_t l = strcspn(p, " ");
            if (l == wl && strncmp(p, want, l) == 0) return &k_sets[i];
            p += l;
            while (*p == ' ') p++;
        }
    }
    return NULL;
}

int charset_is_builtin(const char *from_charset)
{
    return from_charset && find_single(from_charset) != NULL;
}

static size_t put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    o[0] = (char)(0xE0 | (cp >> 12));
    o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static char *via_iconv(const char *from, const char *s, size_t len, size_t *out_len,
                       CharsetStatus *status)
{
    iconv_t cd = iconv_open("UTF-8", from);
    if (cd == (iconv_t)-1) { if (status) *status = CHARSET_UNKNOWN; return NULL; }
    size_t cap = len * 4 + 1;
    char *out = malloc(cap);
    if (!out) { iconv_close(cd); if (status) *status = CHARSET_INVALID; return NULL; }
    char *ip = (char *)s, *op = out;
    size_t il = len, ol = cap - 1;
    size_t r = iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    if (r == (size_t)-1) {
        free(out);
        if (status) *status = CHARSET_INVALID;
        return NULL;
    }
    *op = '\0';
    if (out_len) *out_len = (size_t)(op - out);
    if (status) *status = CHARSET_OK;
    return out;
}

char *charset_decode(const char *from_charset, const char *s, size_t len,
                      size_t *out_len, CharsetStatus *status)
{
    if (!from_charset || !s) { if (status) *status = CHARSET_UNKNOWN; return NULL; }
    const Single *set = find_single(from_charset);
    if (!set) return via_iconv(from_charset, s, len, out_len, status);

    char *out = malloc(len * 3 + 1);
    if (!out) { if (status) *status = CHARSET_INVALID; return NULL; }
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)s[i];
        if (b < 0x80) { out[n++] = (char)b; continue; }
        uint16_t cp = set->table[b - 0x80];
        if (cp == 0) {                       /* a byte the set does not define */
            free(out);
            if (status) *status = CHARSET_INVALID;
            return NULL;
        }
        n += put_utf8(out + n, cp);
    }
    out[n] = '\0';
    if (out_len) *out_len = n;
    if (status) *status = CHARSET_OK;
    return out;
}
