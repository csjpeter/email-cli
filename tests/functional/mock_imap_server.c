#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/**
 * @file mock_imap_server.c
 * @brief Minimal TLS IMAP server for integration testing.
 *
 * Environment variables:
 *   MOCK_IMAP_PORT    - TCP port to listen on (default 9993)
 *   MOCK_IMAP_SUBJECT - Subject returned in FETCH responses (default "Test Message")
 *   MOCK_IMAP_COUNT   - Number of messages to simulate (default 1, backward compat)
 *
 * Launching two instances with different ports and subjects allows testing
 * that account isolation works correctly (each account connects to its own
 * server and sees its own messages).
 *
 * When MOCK_IMAP_COUNT > 1:
 *   - Messages are numbered 1..N
 *   - Subject: "Message X", From: "Sender X <senderX@example.com>"
 *   - UIDs 1..20 are UNSEEN; UIDs 21..N are \Seen
 *   - Messages where X % 10 == 0 have a notes_X.txt attachment
 */

static int         g_port       = 9993;
static const char *g_subject    = "Test Message";
static int         g_count      = 1;
static const char *g_msg_prefix = "Message"; /* MOCK_IMAP_MSG_PREFIX */
static const char *g_long_url   = NULL;      /* MOCK_IMAP_LONG_URL */

/*
 * CONDSTORE / QRESYNC support (RFC 4551 / RFC 5162):
 *   MOCK_IMAP_CAPS          - "CONDSTORE" or "QRESYNC" (QRESYNC implies CONDSTORE)
 *   MOCK_IMAP_MODSEQ        - HIGHESTMODSEQ to report in SELECT (default 0 = disabled)
 *   MOCK_IMAP_UIDVAL        - UIDVALIDITY override (default 1)
 *   MOCK_IMAP_VANISHED      - space-separated UIDs for QRESYNC VANISHED response
 *   MOCK_IMAP_CHANGED_COUNT - N first UIDs returned by CHANGEDSINCE (default 0 = none)
 */
static int         g_cap_condstore    = 0;
static int         g_cap_qresync      = 0;
static long long   g_modseq           = 0;   /* 0 = CONDSTORE disabled */
static int         g_uidval           = 1;
static const char *g_vanished         = NULL; /* space-separated UIDs */
static int         g_changed_count    = 0;

/**
 * Read one CRLF-terminated line from ssl into buf (max len-1 chars + NUL).
 * Strips the trailing CR and LF.  Returns the number of characters placed
 * in buf (excluding NUL), or -1 on EOF/error.
 */
static int readline_crlf_ssl(SSL *ssl, char *buf, int len) {
    int n = 0;
    char c;
    while (n < len - 1) {
        int r = SSL_read(ssl, &c, 1);
        if (r <= 0) return -1;
        if (c == '\r') continue;   /* skip CR */
        if (c == '\n') break;      /* LF = end of line */
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* ── Base64 encode (standard alphabet) for attachment content ─────── */

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * Encode data as standard base64 with = padding.
 * Returns heap-allocated NUL-terminated string.
 */
static char *base64_encode(const unsigned char *data, size_t len) {
    size_t alloc = ((len + 2) / 3) * 4 + 1;
    char *out = malloc(alloc);
    if (!out) return NULL;

    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = ((unsigned int)data[i]) << 16;
        if (i + 1 < len) n |= ((unsigned int)data[i + 1]) << 8;
        if (i + 2 < len) n |= ((unsigned int)data[i + 2]);

        out[o++] = b64_chars[(n >> 18) & 0x3F];
        out[o++] = b64_chars[(n >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? b64_chars[(n >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? b64_chars[n & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}

/**
 * Extract the UID number from a FETCH command line.
 * Handles "UID FETCH X" and "FETCH X" patterns.
 * Returns the UID, or 1 if not parseable.
 */
static int extract_uid(const char *buffer) {
    const char *uid_fetch = strstr(buffer, "UID FETCH ");
    if (uid_fetch) {
        int uid = 0;
        if (sscanf(uid_fetch + 10, "%d", &uid) == 1 && uid > 0)
            return uid;
    }
    const char *fetch_p = strstr(buffer, " FETCH ");
    if (fetch_p) {
        int uid = 0;
        if (sscanf(fetch_p + 7, "%d", &uid) == 1 && uid > 0)
            return uid;
    }
    return 1;
}

/**
 * Build the content for message UID X (multi-message mode).
 * Returns heap-allocated string; caller must free.
 */
static char *build_message_content(int uid, int is_header, int is_flags_only,
                                   const char **section_out) {
    /* Compute a date: start 2020-01-01, add uid days.
     * We use a fixed base and just format a plausible date string. */
    /* Days per month (non-leap year) */
    static const int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    static const char *month_names[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    static const char *day_names[] = {
        "Wed","Thu","Fri","Sat","Sun","Mon","Tue"
    };
    /* 2020-01-01 is a Wednesday, day-of-week index 0 */
    int base_year = 2020;
    int day_of_year = uid; /* offset from Jan 1 2020 */
    int year = base_year;
    int month = 0;
    int day = day_of_year;

    while (1) {
        int days_this_year = (year % 4 == 0) ? 366 : 365;
        if (day <= days_this_year) break;
        day -= days_this_year;
        year++;
    }
    month = 0;
    while (month < 11) {
        int dim = days_in_month[month];
        if (month == 1 && year % 4 == 0) dim = 29;
        if (day <= dim) break;
        day -= dim;
        month++;
    }
    int dow = (uid + 2) % 7; /* approximate day of week */

    char subject[128];
    char from_name[128];
    char from_addr[128];
    char date_str[64];

    snprintf(subject,   sizeof(subject),   "%s %d", g_msg_prefix, uid);
    snprintf(from_name, sizeof(from_name), "Sender %d", uid);
    snprintf(from_addr, sizeof(from_addr), "sender%d@example.com", uid);
    snprintf(date_str, sizeof(date_str), "%s, %02d %s %04d 12:00:00 +0000",
             day_names[dow], day, month_names[month], year);

    if (is_flags_only) {
        /* Caller handles FLAGS specially; should not reach here */
        *section_out = "FLAGS";
        return NULL;
    }

    if (is_header) {
        *section_out = "BODY[HEADER]";
        char *hdr = NULL;
        if (asprintf(&hdr,
                     "From: %s <%s>\r\n"
                     "Subject: %s\r\n"
                     "Date: %s\r\n"
                     "\r\n",
                     from_name, from_addr, subject, date_str) == -1)
            return NULL;
        return hdr;
    }

    *section_out = "BODY[]";

    int has_attachment = (uid % 10 == 0);

    if (!has_attachment) {
        /* Simple plain-text message */
        char *msg = NULL;
        char body_text[128];
        snprintf(body_text, sizeof(body_text), "Body of message %d", uid);
        if (asprintf(&msg,
                     "From: %s <%s>\r\n"
                     "Subject: %s\r\n"
                     "Date: %s\r\n"
                     "MIME-Version: 1.0\r\n"
                     "Content-Type: text/plain; charset=UTF-8\r\n"
                     "\r\n"
                     "%s\r\n",
                     from_name, from_addr, subject, date_str, body_text) == -1)
            return NULL;
        return msg;
    }

    /* Multipart message with text/plain + attachment */
    char boundary[32];
    snprintf(boundary, sizeof(boundary), "B%04d", uid);

    char attach_name[64];
    snprintf(attach_name, sizeof(attach_name), "notes_%d.txt", uid);

    char attach_content[128];
    snprintf(attach_content, sizeof(attach_content), "Attachment for msg %d", uid);
    char *attach_b64 = base64_encode(
        (const unsigned char *)attach_content, strlen(attach_content));
    if (!attach_b64) return NULL;

    char body_text[128];
    snprintf(body_text, sizeof(body_text), "Body of message %d", uid);

    char *msg = NULL;
    int rc = asprintf(&msg,
        "From: %s <%s>\r\n"
        "Subject: %s\r\n"
        "Date: %s\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/mixed; boundary=\"%s\"\r\n"
        "\r\n"
        "--%s\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Type: text/plain; name=\"%s\"\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "%s\r\n"
        "--%s--\r\n",
        from_name, from_addr, subject, date_str,
        boundary,
        boundary,
        body_text,
        boundary,
        attach_name, attach_name,
        attach_b64,
        boundary);
    free(attach_b64);
    if (rc == -1) return NULL;
    return msg;
}

/**
 * Build message content for UID in single-message (legacy) mode.
 * Uses g_subject and the original full multipart body.
 */
static char *build_legacy_content(int is_header, const char **section_out) {
    /* Plain-text message mode: serve a message with a long URL in the body */
    if (g_long_url) {
        const char *hdr =
            "From: Test User <test@example.com>\r\n"
            "Subject: Long URL Test\r\n"
            "Date: Thu, 26 Mar 2026 12:00:00 +0000\r\n"
            "\r\n";
        if (is_header) {
            *section_out = "BODY[HEADER]";
            return strdup(hdr);
        }
        *section_out = "BODY[]";
        char *full_msg = NULL;
        if (asprintf(&full_msg,
                     "From: Test User <test@example.com>\r\n"
                     "Subject: Long URL Test\r\n"
                     "Date: Thu, 26 Mar 2026 12:00:00 +0000\r\n"
                     "MIME-Version: 1.0\r\n"
                     "Content-Type: text/plain; charset=UTF-8\r\n"
                     "\r\n"
                     "Szia Peti!\r\n"
                     "\r\n"
                     "Check this link:\r\n"
                     "%s\r\n",
                     g_long_url) == -1)
            return NULL;
        return full_msg;
    }

    char headers[512];
    snprintf(headers, sizeof(headers),
             "From: Test User <test@example.com>\r\n"
             "Subject: %s\r\n"
             "Date: Thu, 26 Mar 2026 12:00:00 +0000\r\n"
             "\r\n",
             g_subject);

    char *full_msg = NULL;
    if (asprintf(&full_msg,
                 "From: Test User <test@example.com>\r\n"
                 "Subject: %s\r\n"
                 "Date: Thu, 26 Mar 2026 12:00:00 +0000\r\n"
                 "MIME-Version: 1.0\r\n"
                 "Content-Type: multipart/mixed; boundary=\"B001\"\r\n"
                 "\r\n"
                 "--B001\r\n"
                 "Content-Type: text/html; charset=UTF-8\r\n"
                 "\r\n"
                 "<html>"
                 "<head><style>body { color: #222; font-size: 14px; }</style></head>"
                 "<body>"
                 "<h2>Hello &amp; Welcome &#169; &#x2022;</h2>"
                 "<p style=\"color: red; font-weight: bold\">Bold &lt;styled&gt; &bull; text</p>"
                 "<p style=\"color: #333; font-style: italic\">Dark italic &auml; text</p>"
                 "<div style=\"text-decoration: underline; background-color: yellow\">"
                 "<u>Underline</u> <s>strike</s> <del>deleted</del>"
                 "</div>"
                 "<blockquote>Quoted <b>section</b>. \xc3\x9cnicode.</blockquote>"
                 "<ul><li>Item one</li><li>Item two</li></ul>"
                 "<ol><li>First</li><li>Second</li></ol>"
                 "<hr>"
                 "<pre>Code sample here</pre>"
                 "<table><tr><td>Cell A</td><td>Cell B</td></tr></table>"
                 "<img alt=\"Test image\" />"
                 "Line 2<br>Line 3<br>Line 4<br>Line 5<br>"
                 "Line 6<br>Line 7<br>Line 8<br>Line 9<br>"
                 "<a href=\"https://click.example.com/test\">Click here</a>"
                 "</body>"
                 "</html>\r\n"
                 "--B001\r\n"
                 "Content-Type: text/plain; name=\"notes.txt\"\r\n"
                 "Content-Disposition: attachment; filename=\"notes.txt\"\r\n"
                 "Content-Transfer-Encoding: base64\r\n"
                 "\r\n"
                 "SGVsbG8gV29ybGQ=\r\n"
                 "--B001\r\n"
                 "Content-Type: application/octet-stream; name=\"data.bin\"\r\n"
                 "Content-Disposition: attachment; filename=\"data.bin\"\r\n"
                 "Content-Transfer-Encoding: base64\r\n"
                 "\r\n"
                 "dGVzdCBkYXRh\r\n"
                 "--B001--\r\n",
                 g_subject) == -1)
        return NULL;

    if (is_header) {
        *section_out = "BODY[HEADER]";
        char *hdr = strdup(headers);
        free(full_msg);
        return hdr;
    }
    *section_out = "BODY[]";
    return full_msg;
}

static void handle_client(SSL *ssl) {
    char buffer[4096];
    char selected_folder[256] = "";   /* currently selected mailbox */

    /* Set a recv timeout so we don't block forever on half-closed connections */
    int fd = SSL_get_fd(ssl);
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 1. Greeting */
    const char *greeting = "* OK Mock IMAP server ready\r\n";
    SSL_write(ssl, greeting, (int)strlen(greeting));

    while (1) {
        /* Read one complete line (CRLF-terminated) at a time so fragmented
         * TCP writes from the client don't split a command across two recvs. */
        int bytes = readline_crlf_ssl(ssl, buffer, (int)sizeof(buffer));
        if (bytes < 0) break;
        printf("Client requested: %s\n", buffer);

        /* Extract Tag */
        char tag[16] = {0};
        if (sscanf(buffer, "%15s", tag) != 1) {
            strcpy(tag, "*");
        }

        /* Simple State Machine */
        if (strstr(buffer, "CAPABILITY")) {
            char cap_resp[128];
            if (g_cap_qresync)
                snprintf(cap_resp, sizeof(cap_resp),
                         "* CAPABILITY IMAP4rev1 CONDSTORE QRESYNC\r\n");
            else if (g_cap_condstore)
                snprintf(cap_resp, sizeof(cap_resp),
                         "* CAPABILITY IMAP4rev1 CONDSTORE\r\n");
            else
                snprintf(cap_resp, sizeof(cap_resp),
                         "* CAPABILITY IMAP4rev1\r\n");
            SSL_write(ssl, cap_resp, (int)strlen(cap_resp));
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK CAPABILITY completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "ENABLE")) {
            /* ENABLE QRESYNC / CONDSTORE */
            const char *enabled = "* ENABLED QRESYNC\r\n";
            SSL_write(ssl, enabled, (int)strlen(enabled));
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK ENABLE completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "LOGIN")) {
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK LOGIN completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "SELECT")) {
            /* Track selected folder — handle both quoted and unquoted names */
            char *sel = strstr(buffer, "SELECT ");
            if (sel) {
                sel += 7; /* skip "SELECT " */
                if (*sel == '"') {
                    /* quoted: SELECT "INBOX.Empty" */
                    sel++;
                    char *q2 = strchr(sel, '"');
                    size_t slen = q2 ? (size_t)(q2 - sel) : strlen(sel);
                    if (slen >= sizeof(selected_folder)) slen = sizeof(selected_folder) - 1;
                    strncpy(selected_folder, sel, slen);
                    selected_folder[slen] = '\0';
                } else {
                    /* unquoted: SELECT INBOX.Empty */
                    size_t slen = strcspn(sel, " \r\n");
                    if (slen >= sizeof(selected_folder)) slen = sizeof(selected_folder) - 1;
                    strncpy(selected_folder, sel, slen);
                    selected_folder[slen] = '\0';
                }
            }
            int is_empty = (strstr(selected_folder, "Empty") != NULL);
            int cnt = is_empty ? 0 : (g_count > 1 ? g_count : 1);
            int recent = is_empty ? 0 : (g_count > 1 ? 0 : 1);

            /* EXISTS / RECENT / UIDVALIDITY */
            char exists_resp[256];
            snprintf(exists_resp, sizeof(exists_resp),
                     "* %d EXISTS\r\n* %d RECENT\r\n"
                     "* OK [UIDVALIDITY %d] UIDs are valid\r\n",
                     cnt, recent, g_uidval);
            SSL_write(ssl, exists_resp, (int)strlen(exists_resp));

            /* HIGHESTMODSEQ (CONDSTORE/QRESYNC) */
            if (g_modseq > 0) {
                char modseq_resp[64];
                snprintf(modseq_resp, sizeof(modseq_resp),
                         "* OK [HIGHESTMODSEQ %lld]\r\n", g_modseq);
                SSL_write(ssl, modseq_resp, (int)strlen(modseq_resp));
            }

            /* QRESYNC: VANISHED (EARLIER) if the client sent a known modseq */
            int is_qresync_select = (strstr(buffer, "QRESYNC") != NULL);
            if (is_qresync_select && g_vanished && g_vanished[0]) {
                char van_resp[256];
                snprintf(van_resp, sizeof(van_resp),
                         "* VANISHED (EARLIER) %s\r\n", g_vanished);
                SSL_write(ssl, van_resp, (int)strlen(van_resp));
            }

            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK [READ-WRITE] SELECT completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "LIST")) {
            const char *list_resp =
                "* LIST (\\HasNoChildren) \".\" \"INBOX\"\r\n"
                "* LIST (\\HasNoChildren) \".\" \"INBOX.Sent\"\r\n"
                "* LIST (\\HasNoChildren) \".\" \"INBOX.Trash\"\r\n"
                "* LIST (\\HasNoChildren) \".\" \"INBOX.Empty\"\r\n"
                /* IMAP modified UTF-7: "INBOX.Träger" — exercises imap_utf7_decode */
                "* LIST (\\HasNoChildren) \".\" \"INBOX.Tr&AOQ-ger\"\r\n";
            SSL_write(ssl, list_resp, (int)strlen(list_resp));
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK LIST completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "SEARCH")) {
            int is_empty = (strstr(selected_folder, "Empty") != NULL);
            if (is_empty) {
                const char *sr = "* SEARCH\r\n";
                SSL_write(ssl, sr, (int)strlen(sr));
            } else if (g_count > 1) {
                /* Multi-message mode: build UID list */
                int is_unseen = (strstr(buffer, "UNSEEN") != NULL);
                int limit = is_unseen ? 20 : g_count;
                /* Allocate buffer: each UID up to 6 digits + space */
                char *sr_buf = malloc((size_t)limit * 8 + 16);
                if (!sr_buf) break;
                int off = sprintf(sr_buf, "* SEARCH");
                for (int i = 1; i <= limit; i++) {
                    off += sprintf(sr_buf + off, " %d", i);
                }
                off += sprintf(sr_buf + off, "\r\n");
                SSL_write(ssl, sr_buf, off);
                free(sr_buf);
            } else {
                const char *sr = "* SEARCH 1\r\n";
                SSL_write(ssl, sr, (int)strlen(sr));
            }
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK SEARCH completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "FETCH") && strstr(buffer, "CHANGEDSINCE")) {
            /* CONDSTORE: UID FETCH 1:* (UID FLAGS) (CHANGEDSINCE n) */
            /* Return messages 1..g_changed_count with their current flags */
            for (int ci = 1; ci <= g_changed_count; ci++) {
                const char *flags_str = (ci <= 20) ? "()" : "(\\Seen)";
                char resp[128];
                snprintf(resp, sizeof(resp),
                         "* %d FETCH (UID %d FLAGS %s MODSEQ (%lld))\r\n",
                         ci, ci, flags_str, g_modseq);
                SSL_write(ssl, resp, (int)strlen(resp));
            }
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK FETCH completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "FETCH") && strstr(buffer, "FLAGS")) {
            /* FLAGS-only fetch */
            if (g_count > 1) {
                /* Extract the UID/sequence number range or single number */
                int uid = extract_uid(buffer);
                /* UIDs 1..20 are unread; >20 are \Seen */
                const char *flags = (uid <= 20) ? "()" : "(\\Seen)";
                char resp[128];
                snprintf(resp, sizeof(resp), "* %d FETCH (FLAGS %s)\r\n", uid, flags);
                SSL_write(ssl, resp, (int)strlen(resp));
            } else {
                char resp[64];
                snprintf(resp, sizeof(resp), "* 1 FETCH (FLAGS ())\r\n");
                SSL_write(ssl, resp, (int)strlen(resp));
            }
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK FETCH completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "STORE")) {
            /* Flag update (e.g. restore \Seen) */
            const char *resp = "* 1 FETCH (FLAGS ())\r\n";
            SSL_write(ssl, resp, (int)strlen(resp));
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK STORE completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "CREATE")) {
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK CREATE completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "DELETE")) {
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK DELETE completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "COPY")) {
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK COPY completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "EXPUNGE")) {
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK EXPUNGE completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "FETCH")) {
            int is_header = (strstr(buffer, "HEADER") != NULL);
            const char *section = NULL;
            char *content = NULL;

            if (g_count > 1) {
                /* Multi-message mode: synthesize per-UID content */
                int uid = extract_uid(buffer);
                if (uid < 1) uid = 1;
                if (uid > g_count) uid = g_count;
                content = build_message_content(uid, is_header, 0, &section);
                /* Backward compat: UID 1 can use g_subject override */
                if (uid == 1 && g_subject && strcmp(g_subject, "Test Message") != 0) {
                    free(content);
                    content = build_legacy_content(is_header, &section);
                }
            } else {
                content = build_legacy_content(is_header, &section);
            }

            if (!content) {
                char bad[64];
                snprintf(bad, sizeof(bad), "%s BAD Internal error\r\n", tag);
                SSL_write(ssl, bad, (int)strlen(bad));
                break;
            }

            /* Determine which sequence number to report in the response.
             * For multi-message mode we report the UID as the seq number
             * (sufficient for these tests). */
            int seq = (g_count > 1) ? extract_uid(buffer) : 1;
            if (seq < 1) seq = 1;

            char head[256];
            snprintf(head, sizeof(head), "* %d FETCH (%s {%zu}\r\n",
                     seq, section, strlen(content));
            SSL_write(ssl, head, (int)strlen(head));
            SSL_write(ssl, content, (int)strlen(content));
            free(content);

            const char *tail = ")\r\n";
            SSL_write(ssl, tail, (int)strlen(tail));

            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK FETCH completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
            printf("Mock server sent FETCH response for %s\n", section);
        } else if (strstr(buffer, "APPEND")) {
            /* Handle APPEND with synchronising {N} or non-synchronising {N+} literal.
             * Both forms must be handled: the client may use either. */
            char *lbrace = strrchr(buffer, '{');
            long literal_size = 0;
            int  sync_literal = 1;  /* 1 = synchronising, 0 = LITERAL+ */
            if (lbrace) {
                char *end = NULL;
                literal_size = strtol(lbrace + 1, &end, 10);
                if (end && *end == '+') sync_literal = 0;
            }
            if (literal_size <= 0) {
                char bad[64];
                snprintf(bad, sizeof(bad), "%s BAD Missing literal size\r\n", tag);
                SSL_write(ssl, bad, (int)strlen(bad));
            } else {
                if (sync_literal) {
                    /* Synchronising literal: send continuation before reading data */
                    SSL_write(ssl, "+ OK\r\n", 6);
                }
                /* Read and discard the literal bytes */
                char *litbuf = malloc((size_t)literal_size + 1);
                if (!litbuf) break;
                long remaining = literal_size;
                while (remaining > 0) {
                    int n = SSL_read(ssl, litbuf, remaining > 4096 ? 4096 : (int)remaining);
                    if (n <= 0) break;
                    remaining -= n;
                }
                free(litbuf);
                printf("Mock server: APPEND received %ld-byte message\n", literal_size);
                /* RFC 3501: command = ... CRLF — the command-terminating \r\n
                 * comes AFTER the literal body and must not be skipped. */
                char trail[4] = {0};
                int tr = SSL_read(ssl, trail, 2);
                char ok[128];
                if (tr == 2 && trail[0] == '\r' && trail[1] == '\n') {
                    snprintf(ok, sizeof(ok), "%s OK [APPENDUID 1 42] APPEND completed\r\n", tag);
                } else {
                    snprintf(ok, sizeof(ok),
                             "%s BAD Missing command-terminating CRLF after literal\r\n", tag);
                }
                SSL_write(ssl, ok, (int)strlen(ok));
            }
        } else if (strstr(buffer, "LOGOUT")) {
            const char *bye = "* BYE Mock IMAP server logging out\r\n";
            SSL_write(ssl, bye, (int)strlen(bye));
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK LOGOUT completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
            break;
        } else {
            char bad[64];
            snprintf(bad, sizeof(bad), "%s BAD Unknown command\r\n", tag);
            SSL_write(ssl, bad, (int)strlen(bad));
        }
    }
}

int main(void) {
    /* Disable stdout buffering so log-watchers see lines immediately */
    setbuf(stdout, NULL);

    /* Configure port, subject, count, and message prefix from environment */
    const char *port_env = getenv("MOCK_IMAP_PORT");
    if (port_env && atoi(port_env) > 0) g_port = atoi(port_env);
    const char *subj_env = getenv("MOCK_IMAP_SUBJECT");
    if (subj_env && subj_env[0]) g_subject = subj_env;
    const char *count_env = getenv("MOCK_IMAP_COUNT");
    if (count_env && atoi(count_env) > 0) g_count = atoi(count_env);
    const char *prefix_env = getenv("MOCK_IMAP_MSG_PREFIX");
    if (prefix_env && prefix_env[0]) g_msg_prefix = prefix_env;
    const char *long_url_env = getenv("MOCK_IMAP_LONG_URL");
    if (long_url_env && long_url_env[0]) g_long_url = long_url_env;

    /* CONDSTORE / QRESYNC */
    const char *caps_env = getenv("MOCK_IMAP_CAPS");
    if (caps_env) {
        if (strstr(caps_env, "QRESYNC"))   { g_cap_qresync = 1; g_cap_condstore = 1; }
        else if (strstr(caps_env, "CONDSTORE")) g_cap_condstore = 1;
    }
    const char *modseq_env = getenv("MOCK_IMAP_MODSEQ");
    if (modseq_env && atoll(modseq_env) > 0) g_modseq = atoll(modseq_env);
    const char *uidval_env = getenv("MOCK_IMAP_UIDVAL");
    if (uidval_env && atoi(uidval_env) > 0) g_uidval = atoi(uidval_env);
    const char *vanished_env = getenv("MOCK_IMAP_VANISHED");
    if (vanished_env && vanished_env[0]) g_vanished = vanished_env;
    const char *changed_env = getenv("MOCK_IMAP_CHANGED_COUNT");
    if (changed_env && atoi(changed_env) >= 0) g_changed_count = atoi(changed_env);

    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    /* Load TLS certificate and key */
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fprintf(stderr, "SSL_CTX_new failed\n");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    if (SSL_CTX_use_certificate_file(ctx, "tests/certs/test.crt", SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "Failed to load certificate: tests/certs/test.crt\n");
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        exit(EXIT_FAILURE);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, "tests/certs/test.key", SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "Failed to load private key: tests/certs/test.key\n");
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        exit(EXIT_FAILURE);
    }

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        SSL_CTX_free(ctx);
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        close(server_fd);
        SSL_CTX_free(ctx);
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((uint16_t)g_port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        SSL_CTX_free(ctx);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen");
        close(server_fd);
        SSL_CTX_free(ctx);
        exit(EXIT_FAILURE);
    }

    printf("Mock IMAP Server (TLS) listening on port %d (subject: %s, count: %d)\n",
           g_port, g_subject, g_count);

    int client_sock;
    while ((client_sock = accept(server_fd, (struct sockaddr *)&address, &addrlen)) >= 0) {
        printf("Connection accepted from client\n");
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, client_sock);
        if (SSL_accept(ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            close(client_sock);
            continue;
        }
        handle_client(ssl);
        SSL_free(ssl);
        close(client_sock);
        printf("Connection closed\n");
    }

    close(server_fd);
    SSL_CTX_free(ctx);
    return 0;
}
