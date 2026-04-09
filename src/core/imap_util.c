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

char *imap_utf7_encode(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    /* Upper bound: every input byte can expand to at most 8 output chars. */
    char *out = malloc(len * 8 + 4);
    if (!out) return NULL;
    char *dst = out;

    static const char mod64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";

    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p >= 0x20 && *p <= 0x7E && *p != '&') {
            /* Printable ASCII (except '&'): pass through */
            *dst++ = (char)*p++;
        } else if (*p == '&') {
            /* '&' is escaped as "&-" */
            *dst++ = '&';
            *dst++ = '-';
            p++;
        } else {
            /* Non-ASCII run: encode as UTF-16BE in modified Base64 */
            *dst++ = '&';
            unsigned int bits = 0;
            int bit_cnt = 0;

            while (*p && !(*p >= 0x20 && *p <= 0x7E)) {
                /* Decode one UTF-8 code point. */
                uint32_t cp;
                int seqlen;
                if      (*p < 0x80) { cp = *p;         seqlen = 1; }
                else if (*p < 0xC2) { cp = 0xFFFD;     seqlen = 1; }
                else if (*p < 0xE0) { cp = *p & 0x1Fu; seqlen = 2; }
                else if (*p < 0xF0) { cp = *p & 0x0Fu; seqlen = 3; }
                else                { cp = *p & 0x07u; seqlen = 4; }
                for (int i = 1; i < seqlen; i++) {
                    if ((p[i] & 0xC0) != 0x80) { seqlen = i; cp = 0xFFFD; break; }
                    cp = (cp << 6) | (p[i] & 0x3Fu);
                }
                p += seqlen;

                /* Emit as UTF-16BE (BMP char or surrogate pair). */
                uint16_t units[2];
                int nunit;
                if (cp <= 0xFFFFu) {
                    units[0] = (uint16_t)cp;
                    nunit = 1;
                } else {
                    cp -= 0x10000u;
                    units[0] = (uint16_t)(0xD800u | (cp >> 10));
                    units[1] = (uint16_t)(0xDC00u | (cp & 0x3FFu));
                    nunit = 2;
                }

                /* Feed each byte of UTF-16BE into the Base64 bit stream. */
                for (int j = 0; j < nunit; j++) {
                    uint8_t hi = (uint8_t)(units[j] >> 8);
                    uint8_t lo = (uint8_t)(units[j] & 0xFF);

                    bits = (bits << 8) | hi;
                    bit_cnt += 8;
                    while (bit_cnt >= 6) {
                        bit_cnt -= 6;
                        *dst++ = mod64[(bits >> bit_cnt) & 0x3F];
                        bits &= (1u << bit_cnt) - 1u;
                    }

                    bits = (bits << 8) | lo;
                    bit_cnt += 8;
                    while (bit_cnt >= 6) {
                        bit_cnt -= 6;
                        *dst++ = mod64[(bits >> bit_cnt) & 0x3F];
                        bits &= (1u << bit_cnt) - 1u;
                    }
                }
            }
            /* Flush remaining bits (zero-padded to the next 6-bit boundary). */
            if (bit_cnt > 0)
                *dst++ = mod64[(bits << (6 - bit_cnt)) & 0x3F];

            *dst++ = '-';
        }
    }
    *dst = '\0';
    return out;
}
