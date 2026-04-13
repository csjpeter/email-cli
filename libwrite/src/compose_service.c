#include "compose_service.h"
#include "mime_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

    /* Convert body line endings to CRLF */
    char *body_crlf = lf_to_crlf(p->body ? p->body : "");
    if (!body_crlf) return -1;

    /* Assemble message */
    char *msg = NULL;
    int len = asprintf(&msg,
        "Date: %s\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
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
