#include "compose_service.h"
#include "mime_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libgen.h>
#include <ctype.h>

/**
 * @file compose_service.c
 * @brief RFC 2822 message builder and reply metadata extractor.
 */

/* ── Internal helpers ────────────────────────────────────────────────── */

/** Convert LF-terminated body to CRLF. Returns heap string; caller frees. */
static char *lf_to_crlf(const char *body) {
    if (!body) return strdup("");
    /* Count LFs to pre-size the output */
    size_t lf_count = 0;
    for (const char *p = body; *p; p++)
        if (*p == '\n') lf_count++;
    size_t blen = strlen(body);
    char *out = malloc(blen + lf_count + 1);
    if (!out) return NULL;
    char *q = out;
    for (const char *p = body; *p; p++) {
        if (*p == '\n' && (p == body || *(p-1) != '\r'))
            *q++ = '\r';
        *q++ = *p;
    }
    *q = '\0';
    return out;
}

/** Trim leading/trailing whitespace in-place (returns pointer into s). */
static char *trim_ws(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *end = s + strlen(s);
    while (end > s && (*(end-1) == ' ' || *(end-1) == '\t' ||
                       *(end-1) == '\r' || *(end-1) == '\n'))
        end--;
    *end = '\0';
    return s;
}

/* ── Attachment helpers ───────────────────────────────────────────────── */

/** Base64 encoding table. */
static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * Base64-encode `src` of `srclen` bytes into `dst`.
 * Lines are wrapped at 76 characters with CRLF.
 * `dst` must be large enough: ((srclen+2)/3)*4 + (((srclen+2)/3)*4/76+2)*2 + 4 bytes.
 * Returns number of bytes written (excluding NUL).
 */
static size_t base64_encode(const unsigned char *src, size_t srclen,
                             char *dst)
{
    size_t col = 0;
    char *d = dst;

    for (size_t i = 0; i < srclen; i += 3) {
        unsigned int b0 = src[i];
        unsigned int b1 = (i + 1 < srclen) ? src[i + 1] : 0;
        unsigned int b2 = (i + 2 < srclen) ? src[i + 2] : 0;
        size_t avail = srclen - i; /* bytes remaining in this group */

        *d++ = b64tab[(b0 >> 2) & 0x3F];
        *d++ = b64tab[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)];
        *d++ = (avail >= 2) ? b64tab[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : '=';
        *d++ = (avail >= 3) ? b64tab[b2 & 0x3F] : '=';
        col += 4;

        if (col >= 76) {
            *d++ = '\r';
            *d++ = '\n';
            col = 0;
        }
    }

    /* Trailing CRLF if we have a partial line */
    if (col > 0) {
        *d++ = '\r';
        *d++ = '\n';
    }

    *d = '\0';
    return (size_t)(d - dst);
}

/**
 * Infer MIME content type from file extension (case-insensitive).
 */
static const char *infer_mime_type(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot) return "application/octet-stream";

    /* Copy extension to lowercase */
    char ext[32];
    size_t i;
    for (i = 0; i < sizeof(ext) - 1 && dot[1 + i]; i++)
        ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    ext[i] = '\0';

    if (strcmp(ext, "pdf")  == 0) return "application/pdf";
    if (strcmp(ext, "txt")  == 0) return "text/plain";
    if (strcmp(ext, "html") == 0) return "text/html";
    if (strcmp(ext, "htm")  == 0) return "text/html";
    if (strcmp(ext, "jpg")  == 0) return "image/jpeg";
    if (strcmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, "png")  == 0) return "image/png";
    if (strcmp(ext, "gif")  == 0) return "image/gif";
    if (strcmp(ext, "zip")  == 0) return "application/zip";
    if (strcmp(ext, "gz")   == 0) return "application/gzip";
    if (strcmp(ext, "csv")  == 0) return "text/csv";
    if (strcmp(ext, "json") == 0) return "application/json";
    if (strcmp(ext, "xml")  == 0) return "application/xml";
    return "application/octet-stream";
}

/* ── Public API ──────────────────────────────────────────────────────── */

int compose_build_message(const ComposeParams *p, char **out, size_t *outlen) {
    if (!p || !p->from || !p->to || !p->subject || !out || !outlen)
        return -1;

    /* Date header in RFC 2822 format */
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    char date_str[64];
    strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S %z", &tm_local);

    /* Message-ID */
    char hostname[256] = "localhost";
    gethostname(hostname, sizeof(hostname));
    char msgid[512];
    snprintf(msgid, sizeof(msgid), "<%ld.%d@%s>", (long)now, (int)getpid(), hostname);

    /* In-Reply-To header (replies only) */
    char reply_hdr[512] = "";
    if (p->reply_to_msg_id && p->reply_to_msg_id[0])
        snprintf(reply_hdr, sizeof(reply_hdr),
                 "In-Reply-To: %s\r\nReferences: %s\r\n",
                 p->reply_to_msg_id, p->reply_to_msg_id);

    /* Cc/Bcc headers (optional) */
    char cc_hdr[600] = "";
    if (p->cc && p->cc[0])
        snprintf(cc_hdr, sizeof(cc_hdr), "Cc: %s\r\n", p->cc);

    char bcc_hdr[600] = "";
    if (p->bcc && p->bcc[0])
        snprintf(bcc_hdr, sizeof(bcc_hdr), "Bcc: %s\r\n", p->bcc);

    /* Convert body line endings to CRLF */
    char *body_crlf = lf_to_crlf(p->body ? p->body : "");
    if (!body_crlf) return -1;

    /* ── Multipart path (attachments present) ────────────────────────── */
    if (p->attachments != NULL && p->attach_count > 0) {

        /* 1. Validate all attachments before building anything */
        for (int ai = 0; ai < p->attach_count; ai++) {
            struct stat st;
            if (stat(p->attachments[ai], &st) != 0 || !S_ISREG(st.st_mode)) {
                free(body_crlf);
                *out = NULL;
                return -1;
            }
            if (st.st_size > 26214400) { /* 25 MB */
                free(body_crlf);
                *out = NULL;
                return -1;
            }
        }

        /* 2. Generate unique MIME boundary — include a monotonic counter so
         *    two calls in the same second get different boundaries. */
        static unsigned int boundary_seq = 0;
        unsigned int seq = __atomic_add_fetch(&boundary_seq, 1, __ATOMIC_RELAXED);
        char boundary[96];
        snprintf(boundary, sizeof(boundary), "=_%d_%ld_%u",
                 (int)getpid(), (long)now, seq);

        /* 3. Build multipart/mixed message using open_memstream */
        char *buf = NULL;
        size_t bufsz = 0;
        FILE *ms = open_memstream(&buf, &bufsz);
        if (!ms) {
            free(body_crlf);
            return -1;
        }

        /* Top-level headers */
        fprintf(ms, "Date: %s\r\n", date_str);
        fprintf(ms, "From: %s\r\n", p->from);
        fprintf(ms, "To: %s\r\n", p->to);
        if (cc_hdr[0])  fprintf(ms, "%s", cc_hdr);
        if (bcc_hdr[0]) fprintf(ms, "%s", bcc_hdr);
        fprintf(ms, "Subject: %s\r\n", p->subject);
        fprintf(ms, "Message-ID: %s\r\n", msgid);
        if (reply_hdr[0]) fprintf(ms, "%s", reply_hdr);
        fprintf(ms, "MIME-Version: 1.0\r\n");
        fprintf(ms, "Content-Type: multipart/mixed; boundary=\"%s\"\r\n", boundary);
        fprintf(ms, "\r\n");

        /* First MIME part: text/plain body */
        fprintf(ms, "--%s\r\n", boundary);
        fprintf(ms, "Content-Type: text/plain; charset=UTF-8\r\n");
        fprintf(ms, "Content-Transfer-Encoding: 8bit\r\n");
        fprintf(ms, "\r\n");
        fprintf(ms, "%s", body_crlf);
        fprintf(ms, "\r\n");

        /* Attachment parts */
        for (int ai = 0; ai < p->attach_count; ai++) {
            const char *fpath = p->attachments[ai];

            /* basename — work on a copy since basename() may modify */
            char path_copy[4096];
            snprintf(path_copy, sizeof(path_copy), "%s", fpath);
            const char *fname = basename(path_copy);

            const char *mime_type = infer_mime_type(fname);

            /* Read file contents */
            FILE *f = fopen(fpath, "rb");
            if (!f) {
                fclose(ms);
                free(buf);
                free(body_crlf);
                *out = NULL;
                return -1;
            }
            struct stat st;
            stat(fpath, &st);
            size_t fsize = (size_t)st.st_size;
            unsigned char *fdata = malloc(fsize);
            if (!fdata) {
                fclose(f);
                fclose(ms);
                free(buf);
                free(body_crlf);
                return -1;
            }
            size_t nread = fread(fdata, 1, fsize, f);
            fclose(f);

            /* Base64 encode: worst case = ceil(nread/3)*4 + 2*(ceil(nread*4/3)/76+2) */
            size_t b64_max = ((nread + 2) / 3) * 4 + ((((nread + 2) / 3) * 4) / 76 + 2) * 2 + 4;
            char *b64 = malloc(b64_max + 1);
            if (!b64) {
                free(fdata);
                fclose(ms);
                free(buf);
                free(body_crlf);
                return -1;
            }
            base64_encode(fdata, nread, b64);
            free(fdata);

            fprintf(ms, "--%s\r\n", boundary);
            fprintf(ms, "Content-Type: %s\r\n", mime_type);
            fprintf(ms, "Content-Transfer-Encoding: base64\r\n");
            fprintf(ms, "Content-Disposition: attachment; filename=\"%s\"\r\n", fname);
            fprintf(ms, "\r\n");
            fprintf(ms, "%s", b64);
            fprintf(ms, "\r\n");
            free(b64);
        }

        /* Closing boundary */
        fprintf(ms, "--%s--\r\n", boundary);

        fclose(ms);
        free(body_crlf);

        *out    = buf;
        *outlen = bufsz;
        return 0;
    }

    /* ── Plain text path (no attachments) ───────────────────────────── */

    /* Assemble message */
    char *msg = NULL;
    int len = asprintf(&msg,
        "Date: %s\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "%s"
        "%s"
        "Subject: %s\r\n"
        "Message-ID: %s\r\n"
        "%s"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: 8bit\r\n"
        "\r\n"
        "%s",
        date_str,
        p->from,
        p->to,
        cc_hdr,
        bcc_hdr,
        p->subject,
        msgid,
        reply_hdr,
        body_crlf);
    free(body_crlf);

    if (len < 0 || !msg) return -1;
    *out    = msg;
    *outlen = (size_t)len;
    return 0;
}

int compose_extract_reply_meta(const char *raw_msg,
                                char **reply_to_out,
                                char **subject_out,
                                char **msg_id_out) {
    if (!raw_msg || !reply_to_out || !subject_out || !msg_id_out)
        return -1;

    *reply_to_out = NULL;
    *subject_out  = NULL;
    *msg_id_out   = NULL;

    /* Prefer Reply-To header; fall back to From */
    char *rt = mime_get_header(raw_msg, "Reply-To");
    if (!rt || !rt[0]) {
        free(rt);
        rt = mime_get_header(raw_msg, "From");
    }
    *reply_to_out = rt ? mime_decode_words(rt) : strdup("");
    free(rt);

    /* Subject: prefix with "Re: " (strip duplicates) */
    char *subj_raw = mime_get_header(raw_msg, "Subject");
    char *subj_dec = subj_raw ? mime_decode_words(subj_raw) : strdup("");
    free(subj_raw);
    char *s = subj_dec ? trim_ws(subj_dec) : NULL;
    /* Strip all leading "Re: " / "re: " prefixes */
    while (s && (strncasecmp(s, "re: ", 4) == 0 || strncasecmp(s, "re:", 3) == 0)) {
        if (strncasecmp(s, "re: ", 4) == 0) s += 4;
        else s += 3;
        while (*s == ' ') s++;
    }
    char *subj_out = NULL;
    if (asprintf(&subj_out, "Re: %s", s ? s : "") < 0)
        subj_out = strdup("Re: ");
    free(subj_dec);
    *subject_out = subj_out;

    /* Message-ID */
    char *msgid = mime_get_header(raw_msg, "Message-ID");
    *msg_id_out = msgid ? msgid : strdup("");

    return 0;
}
