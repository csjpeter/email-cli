#include "json_util.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Internal helpers ───────────────────────────────────────────────── */

/** Skip whitespace, return pointer to first non-ws char. */
static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/**
 * Skip a JSON value starting at *p (string, number, object, array,
 * true/false/null).  Returns pointer past the value, or NULL on error.
 */
static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (!*p) return NULL;

    if (*p == '"') {
        /* String: advance past closing quote, handling escapes */
        p++;
        while (*p && *p != '"') {
            if (*p == '\\') { p++; if (!*p) return NULL; }
            p++;
        }
        return *p == '"' ? p + 1 : NULL;
    }
    if (*p == '{') {
        /* Object: match braces */
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\') { p++; if (!*p) return NULL; }
                    p++;
                }
                if (!*p) return NULL;
            }
            p++;
        }
        return depth == 0 ? p : NULL;
    }
    if (*p == '[') {
        /* Array: match brackets */
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\') { p++; if (!*p) return NULL; }
                    p++;
                }
                if (!*p) return NULL;
            }
            p++;
        }
        return depth == 0 ? p : NULL;
    }
    /* Number, true, false, null: advance past alphanumeric + signs */
    while (*p && (isalnum((unsigned char)*p) || *p == '.' ||
                  *p == '+' || *p == '-'))
        p++;
    return p;
}

/**
 * Find a key at the current object nesting level.
 * p must point inside a '{' ... '}' block (past the opening '{').
 * Returns pointer to the value (after the ':' and whitespace), or NULL.
 */
static const char *find_key(const char *p, const char *key) {
    size_t klen = strlen(key);
    p = skip_ws(p);

    while (*p && *p != '}') {
        /* Expect a quoted key */
        if (*p != '"') return NULL;
        p++;
        const char *ks = p;
        while (*p && *p != '"') {
            if (*p == '\\') { p++; if (!*p) return NULL; }
            p++;
        }
        if (!*p) return NULL;
        size_t found_len = (size_t)(p - ks);
        int match = (found_len == klen && memcmp(ks, key, klen) == 0);
        p++; /* past closing quote */

        p = skip_ws(p);
        if (*p != ':') return NULL;
        p++;
        p = skip_ws(p);

        if (match) return p;

        /* Skip the value */
        p = skip_value(p);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ',') p++;
        p = skip_ws(p);
    }
    return NULL;
}

/**
 * Unescape a JSON string from src[0..len-1] into a heap-allocated buffer.
 * Handles: \\, \", \/, \n, \r, \t, \b, \f, \uXXXX (BMP only, as ASCII ?).
 */
static char *unescape(const char *src, size_t len) {
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len) {
            i++;
            switch (src[i]) {
            case '"':  buf[out++] = '"';  break;
            case '\\': buf[out++] = '\\'; break;
            case '/':  buf[out++] = '/';  break;
            case 'n':  buf[out++] = '\n'; break;
            case 'r':  buf[out++] = '\r'; break;
            case 't':  buf[out++] = '\t'; break;
            case 'b':  buf[out++] = '\b'; break;
            case 'f':  buf[out++] = '\f'; break;
            case 'u':
                /* \uXXXX — emit '?' for non-ASCII, decode ASCII range */
                if (i + 4 < len) {
                    char hex[5] = {src[i+1], src[i+2], src[i+3], src[i+4], 0};
                    unsigned long cp = strtoul(hex, NULL, 16);
                    if (cp < 0x80)
                        buf[out++] = (char)cp;
                    else
                        buf[out++] = '?'; /* non-ASCII BMP placeholder */
                    i += 4;
                }
                break;
            default:
                buf[out++] = src[i];
                break;
            }
        } else {
            buf[out++] = src[i];
        }
    }
    buf[out] = '\0';
    return buf;
}

/* ── Public API ─────────────────────────────────────────────────────── */

char *json_get_string(const char *json, const char *key) {
    if (!json || !key) return NULL;

    const char *p = skip_ws(json);
    if (*p != '{') return NULL;
    p++;

    const char *val = find_key(p, key);
    if (!val || *val != '"') return NULL;

    val++; /* past opening quote */
    const char *end = val;
    while (*end && *end != '"') {
        if (*end == '\\') { end++; if (!*end) return NULL; }
        end++;
    }
    if (!*end) return NULL;

    return unescape(val, (size_t)(end - val));
}

int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return -1;

    const char *p = skip_ws(json);
    if (*p != '{') return -1;
    p++;

    const char *val = find_key(p, key);
    if (!val) return -1;

    /* Accept number or quoted number */
    if (*val == '"') {
        val++;
        char *end;
        long v = strtol(val, &end, 10);
        if (end == val) return -1;
        *out = (int)v;
        return 0;
    }
    if (*val == '-' || isdigit((unsigned char)*val)) {
        char *end;
        long v = strtol(val, &end, 10);
        if (end == val) return -1;
        *out = (int)v;
        return 0;
    }
    return -1;
}

int json_get_string_array(const char *json, const char *key,
                          char ***out, int *count_out) {
    if (!json || !key || !out || !count_out) return -1;
    *out = NULL;
    *count_out = 0;

    const char *p = skip_ws(json);
    if (*p != '{') return -1;
    p++;

    const char *val = find_key(p, key);
    if (!val || *val != '[') return -1;

    val++; /* past '[' */
    val = skip_ws(val);

    int cap = 8;
    char **arr = malloc((size_t)cap * sizeof(char *));
    if (!arr) return -1;
    int count = 0;

    while (*val && *val != ']') {
        if (*val != '"') { val = skip_ws(val); if (*val == ']') break; goto fail; }
        val++; /* past opening quote */
        const char *end = val;
        while (*end && *end != '"') {
            if (*end == '\\') { end++; if (!*end) goto fail; }
            end++;
        }
        if (!*end) goto fail;

        char *s = unescape(val, (size_t)(end - val));
        if (!s) goto fail;

        if (count >= cap) {
            cap *= 2;
            char **tmp = realloc(arr, (size_t)cap * sizeof(char *));
            if (!tmp) { free(s); goto fail; }
            arr = tmp;
        }
        arr[count++] = s;

        val = end + 1; /* past closing quote */
        val = skip_ws(val);
        if (*val == ',') val++;
        val = skip_ws(val);
    }

    *out = arr;
    *count_out = count;
    return 0;

fail:
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
    return -1;
}

int json_foreach_object(const char *json, const char *key,
                        JsonObjectCb cb, void *ctx) {
    if (!json || !key || !cb) return -1;

    const char *p = skip_ws(json);
    if (*p != '{') return -1;
    p++;

    const char *val = find_key(p, key);
    if (!val || *val != '[') return -1;

    val++; /* past '[' */
    val = skip_ws(val);
    int index = 0;

    while (*val && *val != ']') {
        if (*val != '{') { val = skip_ws(val); if (*val == ']') break; return -1; }

        /* Find the extent of this object */
        const char *obj_start = val;
        const char *obj_end = skip_value(val);
        if (!obj_end) return -1;

        /* Create a NUL-terminated copy for the callback */
        size_t obj_len = (size_t)(obj_end - obj_start);
        char *obj_copy = malloc(obj_len + 1);
        if (!obj_copy) return -1;
        memcpy(obj_copy, obj_start, obj_len);
        obj_copy[obj_len] = '\0';

        cb(obj_copy, index, ctx);
        free(obj_copy);
        index++;

        val = skip_ws(obj_end);
        if (*val == ',') val++;
        val = skip_ws(val);
    }

    return index;
}
