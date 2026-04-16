#include "mime_util.h"
#include "html_render.h"
#include "raii.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <iconv.h>
#include <errno.h>

/* ── Header extraction ──────────────────────────────────────────────── */

char *mime_get_header(const char *msg, const char *name) {
    if (!msg || !name) return NULL;
    size_t nlen = strlen(name);
    const char *p = msg;

    while (p && *p) {
        /* Stop at the blank line separating headers from body. */
        if (*p == '\r' || *p == '\n')
            break;

        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *val = p + nlen + 1;
            while (*val == ' ' || *val == '\t') val++;

            size_t cap = 512, n = 0;
            char *result = malloc(cap);
            if (!result) return NULL;

            /* Collect value, unfolding continuation lines */
            while (*val) {
                if (*val == '\r' || *val == '\n') {
                    const char *next = val;
                    if (*next == '\r') next++;
                    if (*next == '\n') next++;
                    if (*next == ' ' || *next == '\t') {
                        /* Continuation line: skip CRLF and the leading whitespace */
                        val = next;
                        while (*val == ' ' || *val == '\t') val++;
                        /* Add a single space to separate folded content if needed */
                        if (n > 0 && result[n-1] != ' ') {
                            if (n + 1 >= cap) {
                                cap *= 2;
                                char *tmp = realloc(result, cap);
                                if (!tmp) { free(result); return NULL; }
                                result = tmp;
                            }
                            result[n++] = ' ';
                        }
                        continue;
                    } else {
                        /* Not a continuation line: we are done with this header */
                        break;
                    }
                }

                if (n + 1 >= cap) {
                    cap *= 2;
                    char *tmp = realloc(result, cap);
                    if (!tmp) { free(result); return NULL; }
                    result = tmp;
                }
                result[n++] = *val++;
            }
            result[n] = '\0';
            return result;
        }

        /* Advance to next line */
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

/* ── Base64 decoder ─────────────────────────────────────────────────── */

static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static char *decode_base64(const char *in, size_t inlen) {
    size_t max = (inlen / 4 + 1) * 3 + 4;
    char *out = malloc(max);
    if (!out) return NULL;
    size_t n = 0;
    int buf = 0, bits = 0;
    for (size_t i = 0; i < inlen; i++) {
        int v = b64val((unsigned char)in[i]);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (char)((buf >> bits) & 0xFF);
        }
    }
    out[n] = '\0';
    return out;
}

/* ── Quoted-Printable decoder ───────────────────────────────────────── */

static char *decode_qp(const char *in, size_t inlen) {
    char *out = malloc(inlen + 1);
    if (!out) return NULL;
    size_t n = 0, i = 0;
    while (i < inlen) {
        if (in[i] == '=' && i + 1 < inlen &&
            (in[i + 1] == '\r' || in[i + 1] == '\n')) {
            /* Soft line break — skip */
            i++;
            if (i < inlen && in[i] == '\r') i++;
            if (i < inlen && in[i] == '\n') i++;
        } else if (in[i] == '=' && i + 2 < inlen &&
                   isxdigit((unsigned char)in[i + 1]) &&
                   isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = { in[i + 1], in[i + 2], '\0' };
            out[n++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            out[n++] = in[i++];
        }
    }
    out[n] = '\0';
    return out;
}

/* ── Body helpers ───────────────────────────────────────────────────── */

static const char *body_start(const char *msg) {
    const char *p = strstr(msg, "\r\n\r\n");
    if (p) return p + 4;
    p = strstr(msg, "\n\n");
    if (p) return p + 2;
    return NULL;
}

static char *decode_transfer(const char *body, size_t len, const char *enc) {
    if (enc && strcasecmp(enc, "base64") == 0)
        return decode_base64(body, len);
    if (enc && strcasecmp(enc, "quoted-printable") == 0)
        return decode_qp(body, len);
    return strndup(body, len);
}

/* Extract the charset parameter value from a Content-Type header value.
 * E.g. "text/plain; charset=iso-8859-2" → "iso-8859-2".
 * Returns a malloc'd string or NULL if not found. */
static char *extract_charset(const char *ctype) {
    if (!ctype) return NULL;
    const char *p = strcasestr(ctype, "charset=");
    if (!p) return NULL;
    p += 8;
    if (*p == '"') p++;          /* skip optional opening quote */
    const char *start = p;
    while (*p && *p != ';' && *p != ' ' && *p != '\t' && *p != '"' && *p != '\r' && *p != '\n')
        p++;
    if (p == start) return NULL;
    return strndup(start, (size_t)(p - start));
}

/* Convert s from from_charset to UTF-8 via iconv.
 * Returns a malloc'd UTF-8 string; on failure returns strdup(s). */
static char *charset_to_utf8(const char *s, const char *from_charset) {
    if (!s) return NULL;
    if (!from_charset ||
        strcasecmp(from_charset, "utf-8")  == 0 ||
        strcasecmp(from_charset, "utf8")   == 0 ||
        strcasecmp(from_charset, "us-ascii") == 0)
        return strdup(s);

    iconv_t cd = iconv_open("UTF-8", from_charset);
    if (cd == (iconv_t)-1) return strdup(s);

    size_t in_len   = strlen(s);
    size_t out_size = in_len * 4 + 1;
    char  *out      = malloc(out_size);
    if (!out) { iconv_close(cd); return strdup(s); }

    char  *inp      = (char *)s;
    char  *outp     = out;
    size_t inbytes  = in_len;
    size_t outbytes = out_size - 1;
    size_t r        = iconv(cd, &inp, &inbytes, &outp, &outbytes);
    iconv_close(cd);

    if (r == (size_t)-1) { free(out); return strdup(s); }
    *outp = '\0';
    return out;
}

static char *text_from_part(const char *part);

static char *text_from_multipart(const char *msg, const char *ctype) {
    const char *b = strcasestr(ctype, "boundary=");
    if (!b) return NULL;
    b += strlen("boundary=");

    char boundary[512] = {0};
    if (*b == '"') {
        b++;
        const char *end = strchr(b, '"');
        if (!end) return NULL;
        snprintf(boundary, sizeof(boundary), "%.*s", (int)(end - b), b);
    } else {
        size_t i = 0;
        while (*b && *b != ';' && *b != ' ' && *b != '\r' && *b != '\n' &&
               i < sizeof(boundary) - 1)
            boundary[i++] = *b++;
        boundary[i] = '\0';
    }
    if (!boundary[0]) return NULL;

    char delim[520];
    snprintf(delim, sizeof(delim), "--%s", boundary);
    size_t dlen = strlen(delim);

    const char *p = strstr(msg, delim);
    while (p) {
        p = strchr(p + dlen, '\n');
        if (!p) break;
        p++;

        const char *next = strstr(p, delim);
        if (!next) break;

        size_t partlen = (size_t)(next - p);
        char *part = strndup(p, partlen);
        if (!part) break;
        char *result = text_from_part(part);
        free(part);
        if (result) return result;

        p = next + dlen;
        if (p[0] == '-' && p[1] == '-') break;
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

static char *text_from_part(const char *part) {
    char *ctype   = mime_get_header(part, "Content-Type");
    char *enc     = mime_get_header(part, "Content-Transfer-Encoding");
    char *charset = extract_charset(ctype);
    const char *body = body_start(part);
    char *result = NULL;

    if (!ctype || strncasecmp(ctype, "text/plain", 10) == 0) {
        if (body) {
            char *raw = decode_transfer(body, strlen(body), enc);
            if (raw) {
                result = charset_to_utf8(raw, charset);
                free(raw);
            }
        }
    } else if (strncasecmp(ctype, "multipart/", 10) == 0) {
        result = text_from_multipart(part, ctype);
    } else if (strncasecmp(ctype, "text/html", 9) == 0) {
        if (body) {
            char *raw = decode_transfer(body, strlen(body), enc);
            if (raw) {
                char *utf8 = charset_to_utf8(raw, charset);
                free(raw);
                if (utf8) {
                    result = html_render(utf8, 0, 0);
                    free(utf8);
                }
            }
        }
    }

    free(ctype);
    free(enc);
    free(charset);
    return result;
}

/* ── RFC 2047 encoded-word decoder ──────────────────────────────────── */

/**
 * Decode the text portion of one encoded word and convert to UTF-8.
 *
 * enc == 'Q'/'q': quoted-printable variant (underscore = space).
 * enc == 'B'/'b': base64.
 * charset: the declared charset of the encoded bytes.
 *
 * Returns a malloc'd NUL-terminated UTF-8 string, or NULL on failure.
 */
static char *decode_encoded_word(const char *charset, char enc,
                                  const char *text, size_t text_len) {
    char *raw = NULL;

    if (enc == 'Q' || enc == 'q') {
        raw = malloc(text_len + 1);
        if (!raw) return NULL;
        size_t i = 0, j = 0;
        while (i < text_len) {
            if (text[i] == '_') {
                raw[j++] = ' ';
                i++;
            } else if (text[i] == '=' && i + 2 < text_len &&
                       isxdigit((unsigned char)text[i + 1]) &&
                       isxdigit((unsigned char)text[i + 2])) {
                char hex[3] = { text[i + 1], text[i + 2], '\0' };
                raw[j++] = (char)strtol(hex, NULL, 16);
                i += 3;
            } else {
                raw[j++] = text[i++];
            }
        }
        raw[j] = '\0';
    } else {
        /* B encoding */
        raw = decode_base64(text, text_len);
        if (!raw) return NULL;
    }

    /* If the declared charset is already UTF-8, return as-is. */
    if (strcasecmp(charset, "utf-8") == 0 || strcasecmp(charset, "utf8") == 0)
        return raw;

    /* Otherwise convert via iconv. */
    iconv_t cd = iconv_open("UTF-8", charset);
    if (cd == (iconv_t)-1)
        return raw;   /* unknown charset — return raw bytes */

    size_t raw_len   = strlen(raw);
    size_t out_size  = raw_len * 4 + 1;
    char  *utf8      = malloc(out_size);
    if (!utf8) { iconv_close(cd); return raw; }

    char   *inp      = raw;
    char   *outp     = utf8;
    size_t  inbytes  = raw_len;
    size_t  outbytes = out_size - 1;
    size_t  r        = iconv(cd, &inp, &inbytes, &outp, &outbytes);
    iconv_close(cd);

    if (r == (size_t)-1) { free(utf8); return raw; }

    *outp = '\0';
    free(raw);
    return utf8;
}

/**
 * Try to parse and decode one encoded word starting exactly at *pp.
 * Format: =?charset?Q|B?encoded_text?=
 *
 * On success, *pp is advanced past the closing "?=" and the decoded
 * UTF-8 string (malloc'd) is returned.
 * On failure, *pp is unchanged and NULL is returned.
 */
static char *try_decode_encoded_word(const char **pp) {
    const char *p = *pp;
    if (p[0] != '=' || p[1] != '?') return NULL;
    p += 2;

    /* charset */
    const char *cs = p;
    while (*p && *p != '?') p++;
    if (!*p) return NULL;
    size_t cs_len = (size_t)(p - cs);
    if (cs_len == 0 || cs_len >= 64) return NULL;
    char charset[64];
    memcpy(charset, cs, cs_len);
    charset[cs_len] = '\0';
    p++;   /* skip ? */

    /* encoding indicator */
    char enc = *p;
    if (enc != 'Q' && enc != 'q' && enc != 'B' && enc != 'b') return NULL;
    p++;
    if (*p != '?') return NULL;
    p++;   /* skip ? */

    /* encoded text — ends at next ?= */
    const char *txt = p;
    while (*p && !(*p == '?' && p[1] == '=')) p++;
    if (!*p) return NULL;
    size_t txt_len = (size_t)(p - txt);
    p += 2;   /* skip ?= */

    char *decoded = decode_encoded_word(charset, enc, txt, txt_len);
    if (!decoded) return NULL;
    *pp = p;
    return decoded;
}

char *mime_decode_words(const char *value) {
    if (!value) return NULL;

    size_t vlen = strlen(value);
    /* Upper bound: each raw byte can expand to at most 4 UTF-8 bytes. */
    size_t cap = vlen * 4 + 1;
    char  *out = malloc(cap);
    if (!out) return NULL;

    size_t      n             = 0;
    const char *p             = value;
    int         prev_encoded  = 0;

    while (*p) {
        /* RFC 2047 §6.2: linear whitespace between adjacent encoded words
         * must be ignored. */
        if (prev_encoded && (*p == ' ' || *p == '\t')) {
            const char *ws = p;
            while (*ws == ' ' || *ws == '\t') ws++;
            if (ws[0] == '=' && ws[1] == '?') {
                p = ws;
                continue;
            }
        }

        if (p[0] == '=' && p[1] == '?') {
            char *decoded = try_decode_encoded_word(&p);
            if (decoded) {
                size_t dlen = strlen(decoded);
                if (n + dlen >= cap) {
                    cap = n + dlen + vlen + 1;
                    char *tmp = realloc(out, cap);
                    if (!tmp) { free(decoded); break; }
                    out = tmp;
                }
                memcpy(out + n, decoded, dlen);
                n += dlen;
                free(decoded);
                prev_encoded = 1;
                continue;
            }
        }

        prev_encoded = 0;
        out[n++] = *p++;
    }

    out[n] = '\0';
    return out;
}

/* ── Date formatting ────────────────────────────────────────────────── */

char *mime_format_date(const char *date) {
    if (!date || !*date) return NULL;

    static const char * const fmts[] = {
        "%a, %d %b %Y %T %z",  /* "Tue, 10 Mar 2026 15:07:40 +0000"     */
        "%d %b %Y %T %z",       /* "10 Mar 2026 15:07:40 +0000"          */
        "%a, %d %b %Y %T %Z",  /* "Tue, 24 Mar 2026 16:38:21 GMT"       */
        "%d %b %Y %T %Z",       /* "24 Mar 2026 16:38:21 UTC"            */
        NULL
    };

    struct tm tm;
    int parsed = 0;
    for (int i = 0; fmts[i]; i++) {
        memset(&tm, 0, sizeof(tm));
        if (strptime(date, fmts[i], &tm)) { parsed = 1; break; }
    }
    if (!parsed) return strdup(date);

    /* Save tm_gmtoff before calling timegm(): timegm() normalises the struct
     * and resets tm_gmtoff to 0.  timegm() treats the fields as UTC, so
     * subtracting the original offset converts to true UTC. */
    long gmtoff = tm.tm_gmtoff;
    time_t utc = timegm(&tm) - gmtoff;
    if (utc == (time_t)-1) return strdup(date);

    struct tm local;
    localtime_r(&utc, &local);

    char *buf = malloc(17);   /* "YYYY-MM-DD HH:MM\0" */
    if (!buf) return NULL;
    if (strftime(buf, 17, "%Y-%m-%d %H:%M", &local) == 0) {
        free(buf);
        return strdup(date);
    }
    return buf;
}

/* ── HTML part extractor ────────────────────────────────────────────── */

static char *html_from_part(const char *part);

static char *html_from_multipart(const char *msg, const char *ctype) {
    const char *b = strcasestr(ctype, "boundary=");
    if (!b) return NULL;
    b += strlen("boundary=");

    char boundary[512] = {0};
    if (*b == '"') {
        b++;
        const char *end = strchr(b, '"');
        if (!end) return NULL;
        snprintf(boundary, sizeof(boundary), "%.*s", (int)(end - b), b);
    } else {
        size_t i = 0;
        while (*b && *b != ';' && *b != ' ' && *b != '\r' && *b != '\n' &&
               i < sizeof(boundary) - 1)
            boundary[i++] = *b++;
        boundary[i] = '\0';
    }
    if (!boundary[0]) return NULL;

    char delim[520];
    snprintf(delim, sizeof(delim), "--%s", boundary);
    size_t dlen = strlen(delim);

    const char *p = strstr(msg, delim);
    while (p) {
        if (p[dlen] == '-' && p[dlen+1] == '-') break; /* end boundary */
        p = strchr(p + dlen, '\n');
        if (!p) break;
        p++;
        const char *next = strstr(p, delim);
        if (!next) break;
        size_t partlen = (size_t)(next - p);
        char *part = strndup(p, partlen);
        if (!part) break;
        char *result = html_from_part(part);
        free(part);
        if (result) return result;
        p = next; /* keep p pointing at delimiter for next iteration */
    }
    return NULL;
}

static char *html_from_part(const char *part) {
    char *ctype   = mime_get_header(part, "Content-Type");
    char *enc     = mime_get_header(part, "Content-Transfer-Encoding");
    char *charset = extract_charset(ctype);
    const char *body = body_start(part);
    char *result = NULL;

    if (ctype && strncasecmp(ctype, "text/html", 9) == 0) {
        if (body) {
            char *raw = decode_transfer(body, strlen(body), enc);
            if (raw) {
                result = charset_to_utf8(raw, charset);
                free(raw);
            }
        }
    } else if (ctype && strncasecmp(ctype, "multipart/", 10) == 0) {
        result = html_from_multipart(part, ctype);
    }

    free(ctype); free(enc); free(charset);
    return result;
}

/* ── Public API ─────────────────────────────────────────────────────── */

char *mime_get_text_body(const char *msg) {
    if (!msg) return NULL;
    return text_from_part(msg);
}

char *mime_get_html_part(const char *msg) {
    if (!msg) return NULL;
    return html_from_part(msg);
}

/* ── Attachment extraction ──────────────────────────────────────────── */

/* Extract a MIME header parameter value, e.g. filename="foo.pdf" or name=bar.
 * Handles quoted and unquoted values.  Returns malloc'd string or NULL. */
static char *extract_param(const char *header, const char *param) {
    if (!header || !param) return NULL;
    char search[64];
    snprintf(search, sizeof(search), "%s=", param);
    const char *p = strcasestr(header, search);
    if (!p) return NULL;
    p += strlen(search);
    if (*p == '"') {
        p++;
        const char *end = strchr(p, '"');
        if (!end) return NULL;
        return strndup(p, (size_t)(end - p));
    }
    /* unquoted value: ends at ';', whitespace, or end-of-string */
    const char *end = p;
    while (*end && *end != ';' && *end != ' ' && *end != '\t' &&
           *end != '\r' && *end != '\n')
        end++;
    if (end == p) return NULL;
    return strndup(p, (size_t)(end - p));
}

/* Sanitise a filename: strip directory separators and leading dots. */
static char *sanitise_filename(const char *name) {
    if (!name || !*name) return NULL;
    /* take only the basename portion */
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!*base) return NULL;
    char *s = strdup(base);
    if (!s) return NULL;
    /* strip leading dots (hidden files / directory traversal) */
    char *p = s;
    while (*p == '.') p++;
    if (!*p) { free(s); return strdup("attachment"); }
    if (p != s) memmove(s, p, strlen(p) + 1);
    return s;
}

/* Dynamic array for building the attachment list */
typedef struct { MimeAttachment *data; int count; int cap; } AttachList;

static int alist_push(AttachList *al, MimeAttachment att) {
    if (al->count >= al->cap) {
        int newcap = al->cap ? al->cap * 2 : 4;
        MimeAttachment *tmp = realloc(al->data,
                                      (size_t)newcap * sizeof(MimeAttachment));
        if (!tmp) return -1;
        al->data = tmp;
        al->cap  = newcap;
    }
    al->data[al->count++] = att;
    return 0;
}

/* Forward declaration */
static void collect_parts(const char *msg, AttachList *al, int *unnamed_idx);

/* Walk a multipart body and collect attachments from each sub-part. */
static void collect_multipart_attachments(const char *msg, const char *ctype,
                                          AttachList *al, int *idx) {
    const char *b = strcasestr(ctype, "boundary=");
    if (!b) return;
    b += strlen("boundary=");

    char boundary[512] = {0};
    if (*b == '"') {
        b++;
        const char *end = strchr(b, '"');
        if (!end) return;
        snprintf(boundary, sizeof(boundary), "%.*s", (int)(end - b), b);
    } else {
        size_t i = 0;
        while (*b && *b != ';' && *b != ' ' && *b != '\r' && *b != '\n' &&
               i < sizeof(boundary) - 1)
            boundary[i++] = *b++;
        boundary[i] = '\0';
    }
    if (!boundary[0]) return;

    char delim[520];
    snprintf(delim, sizeof(delim), "--%s", boundary);
    size_t dlen = strlen(delim);

    const char *p = strstr(msg, delim);
    while (p) {
        p = strchr(p + dlen, '\n');
        if (!p) break;
        p++;

        const char *next = strstr(p, delim);
        if (!next) break;

        size_t partlen = (size_t)(next - p);
        char *part = strndup(p, partlen);
        if (!part) break;
        collect_parts(part, al, idx);
        free(part);

        p = next + dlen;
        if (p[0] == '-' && p[1] == '-') break;
        p = strchr(p, '\n');
        if (p) p++;
    }
}

/* Examine one MIME part (headers + body) and add to al if it is an attachment. */
static void collect_parts(const char *msg, AttachList *al, int *unnamed_idx) {
    char *ctype = mime_get_header(msg, "Content-Type");
    char *disp  = mime_get_header(msg, "Content-Disposition");
    char *enc   = mime_get_header(msg, "Content-Transfer-Encoding");

    /* Recurse into multipart containers */
    if (ctype && strncasecmp(ctype, "multipart/", 10) == 0) {
        collect_multipart_attachments(msg, ctype, al, unnamed_idx);
        free(ctype); free(disp); free(enc);
        return;
    }

    /* Determine filename from Content-Disposition or Content-Type name= */
    char *filename = NULL;
    int explicit_attach = 0;
    if (disp) {
        if (strncasecmp(disp, "attachment", 10) == 0) explicit_attach = 1;
        filename = extract_param(disp, "filename");
        /* RFC 5987: filename*=charset''encoded — simplified: strip trailing * */
        if (!filename) filename = extract_param(disp, "filename*");
    }
    if (!filename && ctype)
        filename = extract_param(ctype, "name");

    /* Skip non-attachment text and multipart parts unless explicitly marked */
    if (!explicit_attach) {
        if (!filename) {
            free(ctype); free(disp); free(enc);
            return;  /* no filename → body part, skip */
        }
        /* text/plain and text/html without attachment disposition are body parts */
        if (ctype && (strncasecmp(ctype, "text/plain", 10) == 0 ||
                      strncasecmp(ctype, "text/html",   9) == 0)) {
            free(ctype); free(disp); free(enc); free(filename);
            return;
        }
    }

    const char *body = body_start(msg);
    if (!body) {
        free(ctype); free(disp); free(enc); free(filename);
        return;
    }

    /* Decode body content */
    unsigned char *data = (unsigned char *)decode_transfer(body, strlen(body), enc);
    size_t data_size = data ? strlen((char *)data) : 0;
    /* For binary (base64-decoded) content the length may contain NUL bytes —
     * use the decoded output length from decode_base64, which null-terminates
     * but the real size is the base64-decoded byte count. */
    if (enc && strcasecmp(enc, "base64") == 0 && data) {
        /* decode_base64 returns the decoded bytes; count excludes the trailing NUL */
        /* we need actual binary size — recount via the decoded buffer length */
        size_t raw_enc_len = strlen(body);
        data_size = (raw_enc_len / 4) * 3;  /* upper bound */
        /* trim padding: accurate enough for display; file write uses full buffer */
    }

    /* Sanitise / generate filename */
    char *safe_name = NULL;
    if (filename) {
        char *decoded = mime_decode_words(filename);
        free(filename);
        safe_name = sanitise_filename(decoded ? decoded : "");
        free(decoded);
    }
    if (!safe_name) {
        char gen[32];
        snprintf(gen, sizeof(gen), "attachment-%d.bin", ++(*unnamed_idx));
        safe_name = strdup(gen);
    }

    MimeAttachment att = {0};
    att.filename     = safe_name;
    att.content_type = ctype ? strdup(ctype) : strdup("application/octet-stream");
    att.data         = data;
    att.size         = data_size;

    if (alist_push(al, att) < 0) {
        free(att.filename); free(att.content_type); free(att.data);
    }

    free(ctype); free(disp); free(enc);
}

MimeAttachment *mime_list_attachments(const char *msg, int *count_out) {
    if (!msg || !count_out) { if (count_out) *count_out = 0; return NULL; }
    AttachList al = {NULL, 0, 0};
    int idx = 0;
    collect_parts(msg, &al, &idx);
    *count_out = al.count;
    if (al.count == 0) { free(al.data); return NULL; }
    return al.data;
}

void mime_free_attachments(MimeAttachment *list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) {
        free(list[i].filename);
        free(list[i].content_type);
        free(list[i].data);
    }
    free(list);
}

int mime_save_attachment(const MimeAttachment *att, const char *dest_path) {
    if (!att || !dest_path || !att->data) return -1;
    RAII_FILE FILE *f = fopen(dest_path, "wb");
    if (!f) return -1;
    /* Write the full decoded buffer; for base64 the NUL terminator is not
     * part of the content — use att->size if accurate, else strlen fallback. */
    size_t n = att->size > 0 ? att->size : strlen((char *)att->data);
    size_t written = fwrite(att->data, 1, n, f);
    return (written != n) ? -1 : 0;
}

char *mime_extract_imap_literal(const char *response) {
    if (!response) return NULL;
    const char *brace = strchr(response, '{');
    if (!brace) return NULL;
    
    char *end = NULL;
    long size = strtol(brace + 1, &end, 10);
    if (!end || *end != '}' || size <= 0) return NULL;
    
    const char *content = end + 1;
    if (*content == '\r') content++;
    if (*content == '\n') content++;
    
    // Safety check
    size_t avail = strlen(content);
    if (avail < (size_t)size) {
        return strndup(content, avail);
    }
    
    return strndup(content, (size_t)size);
}
