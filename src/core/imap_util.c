#include "imap_util.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/**
 * @file imap_util.c
 * @brief IMAP Modified UTF-7 decoder (RFC 3501 §5.1.3).
 */

/* Modified base64 alphabet: A-Z(0-25), a-z(26-51), 0-9(52-61), +(62), ,(63) */
static int mod64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == ',') return 63;
    return -1;
}

/* Encode one Unicode code point as UTF-8.  Returns bytes written (1-4). */
static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

char *imap_utf7_decode(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);

    /* Worst case: each input byte expands to at most 4 UTF-8 bytes */
    char *out = malloc(len * 4 + 1);
    if (!out) return NULL;
    char *dst = out;

    const char *p = s;
    while (*p) {
        if (*p != '&') {
            *dst++ = *p++;
            continue;
        }
        p++; /* skip '&' */

        if (*p == '-') {
            /* "&-" is a literal '&' */
            *dst++ = '&';
            p++;
            continue;
        }

        /* Decode modified-base64 run into raw bytes (UTF-16BE) */
        uint8_t bytes[256];
        int byte_cnt = 0;
        int bits = 0, bit_cnt = 0;

        while (*p && *p != '-') {
            int v = mod64_value(*p++);
            if (v < 0) break;
            bits = (bits << 6) | v;
            bit_cnt += 6;
            if (bit_cnt >= 8) {
                bit_cnt -= 8;
                if (byte_cnt < (int)sizeof(bytes))
                    bytes[byte_cnt++] = (uint8_t)((unsigned)bits >> bit_cnt);
                bits &= (1 << bit_cnt) - 1;
            }
        }
        if (*p == '-') p++;

        /* Interpret raw bytes as UTF-16BE, emit as UTF-8 */
        for (int i = 0; i + 1 < byte_cnt; i += 2) {
            uint16_t unit = ((uint16_t)bytes[i] << 8) | bytes[i + 1];
            uint32_t cp;

            if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < byte_cnt) {
                /* High surrogate — pair with following low surrogate */
                uint16_t low = ((uint16_t)bytes[i + 2] << 8) | bytes[i + 3];
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000u
                         + ((uint32_t)(unit - 0xD800) << 10)
                         + (uint32_t)(low - 0xDC00);
                    i += 2;
                } else {
                    cp = unit; /* unpaired surrogate — pass through */
                }
            } else {
                cp = unit;
            }

            char utf8[4];
            int n = utf8_encode(cp, utf8);
            memcpy(dst, utf8, (size_t)n);
            dst += n;
        }
    }
    *dst = '\0';
    return out;
}
