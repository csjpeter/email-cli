/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-15 */
/* Unit tests of libemail-client's public interface (EMAIL-101). */
#include "test_helpers.h"
#include "email-client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef ENABLE_GCOV
extern void __gcov_dump(void);
#  define GCOV_FLUSH() __gcov_dump()
#else
#  define GCOV_FLUSH() ((void)0)
#endif

/**
 * @file test_email_client.c
 * @brief The emc_* interface against in-process mock servers: SMTP and IMAP
 *        over TLS (self-signed test certificate) and the Gmail REST API.
 *
 * The library audits nothing, so the tests check what the caller needs for
 * its own record: the outcome and the message id.
 */

/* ── Mock server plumbing ──────────────────────────────────────────── */

static int ec_listener(int *port_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &len);
    *port_out = ntohs(a.sin_port);
    return fd;
}

static SSL_CTX *ec_server_ctx(void)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return NULL;
    if (SSL_CTX_use_certificate_file(ctx, TEST_CERT_PATH, SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, TEST_KEY_PATH, SSL_FILETYPE_PEM) <= 0) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

static int ec_readline(SSL *ssl, char *buf, int len)
{
    int n = 0;
    char c;
    while (n < len - 1) {
        if (SSL_read(ssl, &c, 1) <= 0) return -1;
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

static void ec_say(SSL *ssl, const char *s) { SSL_write(ssl, s, (int)strlen(s)); }

/* One TLS SMTP exchange.  The received message is written to report_fd. */
static void ec_smtp_child(int lfd, SSL_CTX *ctx, int report_fd)
{
    alarm(120);   /* watchdog: a child never outlives a stuck test */
    int cfd = accept(lfd, NULL, NULL);
    close(lfd);
    if (cfd < 0) { GCOV_FLUSH(); _exit(1); }
    struct timeval tv = {.tv_sec = 60, .tv_usec = 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, cfd);
    if (SSL_accept(ssl) <= 0) { GCOV_FLUSH(); _exit(1); }
    ec_say(ssl, "220 mock ready\r\n");
    char buf[4096];
    int in_data = 0;
    while (ec_readline(ssl, buf, sizeof(buf)) >= 0) {
        if (in_data) {
            if (strcmp(buf, ".") == 0) {
                in_data = 0;
                ec_say(ssl, "250 queued\r\n");
                continue;
            }
            ssize_t w = write(report_fd, buf, strlen(buf)); (void)w;
            w = write(report_fd, "\n", 1); (void)w;
        } else if (!strncasecmp(buf, "EHLO", 4) || !strncasecmp(buf, "HELO", 4)) {
            ec_say(ssl, "250-mock\r\n250-AUTH PLAIN LOGIN\r\n250 OK\r\n");
        } else if (!strncasecmp(buf, "AUTH PLAIN", 10)) {
            if (buf[10] == '\0') { ec_say(ssl, "334 \r\n"); ec_readline(ssl, buf, sizeof(buf)); }
            ec_say(ssl, "235 ok\r\n");
        } else if (!strncasecmp(buf, "AUTH LOGIN", 10)) {
            ec_say(ssl, "334 VXNlcm5hbWU6\r\n"); ec_readline(ssl, buf, sizeof(buf));
            ec_say(ssl, "334 UGFzc3dvcmQ6\r\n"); ec_readline(ssl, buf, sizeof(buf));
            ec_say(ssl, "235 ok\r\n");
        } else if (!strncasecmp(buf, "MAIL FROM", 9) || !strncasecmp(buf, "RCPT TO", 7)) {
            ec_say(ssl, "250 ok\r\n");
        } else if (!strncasecmp(buf, "DATA", 4)) {
            ec_say(ssl, "354 go\r\n");
            in_data = 1;
        } else if (!strncasecmp(buf, "QUIT", 4)) {
            ec_say(ssl, "221 bye\r\n");
            break;
        } else {
            ec_say(ssl, "250 ok\r\n");
        }
    }
    SSL_shutdown(ssl);
    GCOV_FLUSH();
    _exit(0);
}

/* How long the IMAP mock waits before its greeting (set before ec_start()). */
static int g_greeting_delay_ms;

/* One TLS IMAP session: LOGIN, LIST, SELECT, SEARCH, FETCH, APPEND, LOGOUT.
 * An APPEND is reported as "APPEND <folder>" on report_fd. */
static void ec_imap_child(int lfd, SSL_CTX *ctx, int report_fd)
{
    alarm(120);
    int cfd = accept(lfd, NULL, NULL);
    close(lfd);
    if (cfd < 0) { GCOV_FLUSH(); _exit(1); }
    struct timeval tv = {.tv_sec = 60, .tv_usec = 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, cfd);
    if (SSL_accept(ssl) <= 0) { GCOV_FLUSH(); _exit(1); }
    if (g_greeting_delay_ms > 0) usleep((useconds_t)g_greeting_delay_ms * 1000);
    ec_say(ssl, "* OK mock ready\r\n");
    static const char msg[] = "From: a@example.org\r\nSubject: Hi\r\n\r\nHello.\r\n";
    char buf[4096], resp[1024];
    for (;;) {
        int n = SSL_read(ssl, buf, (int)sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        char tag[32] = "*";
        sscanf(buf, "%31s", tag);
        if (strstr(buf, "LOGIN")) {
            snprintf(resp, sizeof(resp), "%s OK [CAPABILITY IMAP4rev1] ok\r\n", tag);
        } else if (strstr(buf, "SELECT")) {
            snprintf(resp, sizeof(resp), "* 2 EXISTS\r\n%s OK [READ-WRITE] ok\r\n", tag);
        } else if (strstr(buf, "UID SEARCH")) {
            snprintf(resp, sizeof(resp), "* SEARCH %s\r\n%s OK done\r\n",
                     strstr(buf, "UNSEEN") ? "2" : "1 2", tag);
        } else if (strstr(buf, "BODY.PEEK[]")) {
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (UID 1 BODY[] {%d}\r\n%s)\r\n%s OK done\r\n",
                     (int)strlen(msg), msg, tag);
        } else if (strstr(buf, "LIST")) {
            snprintf(resp, sizeof(resp),
                     "* LIST (\\HasNoChildren) \"/\" \"INBOX\"\r\n"
                     "* LIST (\\HasNoChildren) \"/\" \"Sent\"\r\n%s OK done\r\n", tag);
        } else if (strstr(buf, "APPEND")) {
            char *lb = strrchr(buf, '{');
            long size = lb ? strtol(lb + 1, NULL, 10) : 0;
            ec_say(ssl, "+ go\r\n");
            char tmp[512];
            while (size > 0) {
                int r = SSL_read(ssl, tmp, size > (long)sizeof(tmp) ? (int)sizeof(tmp) : (int)size);
                if (r <= 0) break;
                size -= r;
            }
            ssize_t w = write(report_fd, "APPEND\n", 7); (void)w;
            SSL_read(ssl, tmp, 2);   /* the closing CRLF of the command */
            snprintf(resp, sizeof(resp), "%s OK done\r\n", tag);
        } else if (strstr(buf, "LOGOUT")) {
            snprintf(resp, sizeof(resp), "* BYE\r\n%s OK done\r\n", tag);
            ec_say(ssl, resp);
            break;
        } else {
            snprintf(resp, sizeof(resp), "%s OK\r\n", tag);
        }
        ec_say(ssl, resp);
    }
    SSL_shutdown(ssl);
    GCOV_FLUSH();
    _exit(0);
}

/* The Gmail API as plain HTTP: every request answered 200; the request line
 * and body of a send are reported on report_fd. */
static void ec_gmail_child(int lfd, int report_fd)
{
    alarm(120);
    for (int i = 0; i < 4; i++) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) break;
        char req[16384];
        int total = 0;
        while (total < (int)sizeof(req) - 1) {
            ssize_t n = read(cfd, req + total, sizeof(req) - 1 - (size_t)total);
            if (n <= 0) break;
            total += (int)n;
            req[total] = '\0';
            char *hdr_end = strstr(req, "\r\n\r\n");
            if (hdr_end) {
                char *cl = strcasestr(req, "Content-Length:");
                long want = cl ? strtol(cl + 15, NULL, 10) : 0;
                if (total >= (int)(hdr_end - req) + 4 + want) break;
            }
        }
        req[total] = '\0';
        if (strstr(req, "/messages/send")) {
            ssize_t w = write(report_fd, req, (size_t)total); (void)w;
        }
        const char *body = "{\"id\":\"g1\"}";
        char head[256];
        snprintf(head, sizeof(head),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
        ssize_t w = write(cfd, head, strlen(head)); (void)w;
        w = write(cfd, body, strlen(body)); (void)w;
        close(cfd);
    }
    close(lfd);
    GCOV_FLUSH();
    _exit(0);
}

typedef enum { SRV_SMTP, SRV_IMAP, SRV_GMAIL } srv_kind;

/* Fork a server; *report_rd is the read end of its report pipe. */
static pid_t ec_start(srv_kind kind, int *port, int *report_rd)
{
    int lfd = ec_listener(port);
    if (lfd < 0) return -1;
    int p[2];
    if (pipe(p) != 0) { close(lfd); return -1; }
    SSL_CTX *ctx = kind == SRV_GMAIL ? NULL : ec_server_ctx();
    if (kind != SRV_GMAIL && !ctx) { close(lfd); close(p[0]); close(p[1]); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (kind == SRV_SMTP) ec_smtp_child(lfd, ctx, p[1]);
        else if (kind == SRV_IMAP) ec_imap_child(lfd, ctx, p[1]);
        else ec_gmail_child(lfd, p[1]);
        _exit(1);
    }
    close(lfd);
    close(p[1]);
    if (ctx) SSL_CTX_free(ctx);
    *report_rd = p[0];
    return pid;
}

/* Everything the server wrote to its report pipe, until it exits. */
static void ec_collect(pid_t pid, int rd, char *out, size_t cap)
{
    size_t used = 0;
    ssize_t n;
    while (used + 1 < cap && (n = read(rd, out + used, cap - 1 - used)) > 0)
        used += (size_t)n;
    out[used] = '\0';
    close(rd);
    int st;
    if (waitpid(pid, &st, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
    }
}

static void ec_account(emc_account *a, int imap_port, int smtp_port)
{
    static char host[64], smtp[64];
    memset(a, 0, sizeof(*a));
    snprintf(host, sizeof(host), "imaps://127.0.0.1:%d", imap_port);
    snprintf(smtp, sizeof(smtp), "smtps://127.0.0.1:%d", smtp_port);
    a->host = host;
    a->user = "user@example.org";
    a->pass = "secret";
    a->smtp_host = smtp;
    a->insecure_tls = 1;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static void test_ec_version_and_strerror(void)
{
    ASSERT(emc_version() && emc_version()[0], "version: non-empty");
    ASSERT(strcmp(emc_strerror(EMC_OK), "success") == 0, "strerror: OK");
    for (int s = EMC_OK; s <= EMC_ERR_MEMORY; s++)
        ASSERT(emc_strerror((emc_status)s)[0] != '\0', "strerror: every status has text");
    ASSERT(emc_strerror((emc_status)99)[0] != '\0', "strerror: unknown status has text");
}

static void test_ec_open_validation(void)
{
    emc_client *c = (emc_client *)1;
    emc_account a = {0};
    ASSERT(emc_open(NULL, &a) == EMC_ERR_ARGUMENT, "open: NULL out");
    ASSERT(emc_open(&c, NULL) == EMC_ERR_ARGUMENT && c == NULL, "open: NULL account");
    ASSERT(emc_open(&c, &a) == EMC_ERR_ARGUMENT, "open: empty IMAP account");
    a.host = "imaps://imap.example.org";
    ASSERT(emc_open(&c, &a) == EMC_ERR_ARGUMENT, "open: IMAP without user");
    a.user = "u@example.org";
    ASSERT(emc_open(&c, &a) == EMC_OK && c != NULL, "open: valid IMAP account");
    ASSERT(strcmp(emc_last_error(c), "") == 0, "open: no error yet");
    emc_close(c);
    emc_close(NULL);

    emc_account g = {.gmail = 1};
    ASSERT(emc_open(&c, &g) == EMC_ERR_ARGUMENT, "open: Gmail without refresh token");
    g.gmail_refresh_token = "tok";
    ASSERT(emc_open(&c, &g) == EMC_OK, "open: Gmail with refresh token");
    emc_close(c);
    ASSERT(strcmp(emc_last_error(NULL), "") == 0, "last_error: NULL handle");
}

static void test_ec_open_copies_account(void)
{
    char host[] = "imaps://127.0.0.1:1";
    char user[] = "u@example.org";
    emc_account a = {.host = host, .user = user};
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "copy: open");
    memset(host, 'x', sizeof(host) - 1);
    memset(user, 'x', sizeof(user) - 1);
    /* The copy is still the original: an unreachable server, not a garbage URL. */
    char **names = NULL;
    int n = -1;
    ASSERT(emc_list_folders(c, &names, &n) == EMC_ERR_CONNECT,
           "copy: operation uses the copied account");
    emc_close(c);
}

static void test_ec_send_validation(void)
{
    emc_account a = {.host = "imaps://127.0.0.1:1", .user = "u@example.org",
                     .insecure_tls = 1};
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "send validation: open");
    emc_send_result r;
    emc_message m = {.to = "x@example.org", .subject = "s", .body = "b"};

    ASSERT(emc_send(NULL, &m, &r) == EMC_ERR_ARGUMENT, "send: NULL client");
    ASSERT(emc_send(c, NULL, &r) == EMC_ERR_ARGUMENT, "send: NULL message");
    emc_message no_to = {.subject = "s", .body = "b"};
    ASSERT(emc_send(c, &no_to, &r) == EMC_ERR_ARGUMENT, "send: no recipient");
    ASSERT(r.status == EMC_ERR_ARGUMENT && r.message_id[0] == '\0',
           "send: result carries the failure and no id");
    emc_message no_subj = {.to = "x@example.org", .body = "b"};
    ASSERT(emc_send(c, &no_subj, NULL) == EMC_ERR_ARGUMENT, "send: no subject, NULL result");
    emc_message bad_att = {.to = "x@example.org", .subject = "s", .body = "b",
                           .attachment_count = 2};
    ASSERT(emc_send(c, &bad_att, &r) == EMC_ERR_ARGUMENT, "send: count without array");
    emc_close(c);
}

static void test_ec_send_smtp(void)
{
    int port = 0, rd = -1;
    pid_t pid = ec_start(SRV_SMTP, &port, &rd);
    ASSERT(pid > 0, "send smtp: start server");

    emc_account a;
    ec_account(&a, 1, port);
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "send smtp: open");
    emc_message m = {.to = "rcpt@example.org", .subject = "Greetings",
                     .body = "Line one\nLine two\n"};
    emc_send_result r;
    emc_status st = emc_send(c, &m, &r);
    emc_close(c);

    char got[8192];
    ec_collect(pid, rd, got, sizeof(got));
    ASSERT(st == EMC_OK && r.status == EMC_OK, "send smtp: succeeds");
    ASSERT(r.message_id[0] == '<', "send smtp: a Message-ID is returned");
    ASSERT(strstr(got, "Subject: Greetings") != NULL, "send smtp: subject on the wire");
    ASSERT(strstr(got, "To: rcpt@example.org") != NULL, "send smtp: recipient on the wire");
    ASSERT(strstr(got, "From: user@example.org") != NULL,
           "send smtp: sender defaults to the account's user");
    char want[320];
    snprintf(want, sizeof(want), "Message-ID: %s", r.message_id);
    ASSERT(strstr(got, want) != NULL, "send smtp: the returned id is the one sent");
    ASSERT(r.saved_copy == 0, "send smtp: no sent folder, no copy");
}

static void test_ec_send_smtp_unreachable(void)
{
    int port = 0, rd = -1;
    pid_t pid = ec_start(SRV_SMTP, &port, &rd);   /* take a port, then free it */
    ASSERT(pid > 0, "send down: start server");
    kill(pid, SIGKILL);
    char dump[16];
    ec_collect(pid, rd, dump, sizeof(dump));

    emc_account a;
    ec_account(&a, 1, port);
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "send down: open");
    emc_message m = {.to = "rcpt@example.org", .subject = "s", .body = "b"};
    emc_send_result r;
    ASSERT(emc_send(c, &m, &r) == EMC_ERR_SEND, "send down: EMC_ERR_SEND");
    ASSERT(r.status == EMC_ERR_SEND && r.message_id[0] == '\0' && r.saved_copy == 0,
           "send down: no id, no copy");
    ASSERT(emc_last_error(c)[0] != '\0', "send down: last error is described");
    emc_close(c);
}

static void test_ec_imap_read(void)
{
    int port = 0, rd = -1;
    pid_t pid = ec_start(SRV_IMAP, &port, &rd);
    ASSERT(pid > 0, "imap: start server");
    emc_account a;
    ec_account(&a, port, 1);
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "imap: open");

    char **names = NULL;
    int n = 0;
    ASSERT(emc_list_folders(c, &names, &n) == EMC_OK, "imap: list folders");
    ASSERT(n == 2 && strcmp(names[0], "INBOX") == 0, "imap: two folders, INBOX first");
    emc_free_list(names, n);

    char **ids = NULL;
    ASSERT(emc_list_messages(c, NULL, 0, &ids, &n) == EMC_OK, "imap: list all");
    ASSERT(n == 2, "imap: two messages");
    emc_free_list(ids, n);
    ASSERT(emc_list_messages(c, "INBOX", 1, &ids, &n) == EMC_OK, "imap: list unseen");
    ASSERT(n == 1, "imap: one unseen message");
    char id[32];
    snprintf(id, sizeof(id), "%s", ids[0]);
    emc_free_list(ids, n);

    char *raw = NULL;
    ASSERT(emc_fetch_message(c, NULL, id, &raw) == EMC_OK && raw, "imap: fetch");
    ASSERT(strstr(raw, "Subject: Hi") != NULL, "imap: the message is complete");
    free(raw);
    ASSERT(emc_fetch_message(c, NULL, "", &raw) == EMC_ERR_ARGUMENT && raw == NULL,
           "imap: fetch without id");
    ASSERT(emc_list_folders(c, NULL, &n) == EMC_ERR_ARGUMENT, "imap: NULL out");
    emc_close(c);
    char dump[64];
    ec_collect(pid, rd, dump, sizeof(dump));
}

static void ec_on_alarm(int sig) { (void)sig; }

/* A signal during a blocking read must not fail the operation: email-tui
 * installs a SIGCHLD handler without SA_RESTART while it syncs, and a
 * program using the library may do the same (EINTR is not an error). */
static void test_ec_imap_survives_signals(void)
{
    int port = 0, rd = -1;
    g_greeting_delay_ms = 400;
    pid_t pid = ec_start(SRV_IMAP, &port, &rd);
    g_greeting_delay_ms = 0;
    ASSERT(pid > 0, "signal: start server");

    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ec_on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* no SA_RESTART: the read is interrupted */
    sigaction(SIGALRM, &sa, &old);
    struct itimerval it = {.it_interval = {0, 50000}, .it_value = {0, 50000}};
    struct itimerval none = {0};
    setitimer(ITIMER_REAL, &it, NULL);

    emc_account a;
    ec_account(&a, port, 1);
    emc_client *c = NULL;
    emc_status st = emc_open(&c, &a);
    char **names = NULL;
    int n = 0;
    if (st == EMC_OK) st = emc_list_folders(c, &names, &n);

    setitimer(ITIMER_REAL, &none, NULL);
    sigaction(SIGALRM, &old, NULL);
    emc_free_list(names, n);
    emc_close(c);
    char dump[64];
    ec_collect(pid, rd, dump, sizeof(dump));
    ASSERT(st == EMC_OK && n == 2, "signal: the greeting wait survives interrupting signals");
}

static void test_ec_imap_unreachable(void)
{
    int port = 0, rd = -1;
    pid_t pid = ec_start(SRV_IMAP, &port, &rd);
    ASSERT(pid > 0, "imap down: start server");
    kill(pid, SIGKILL);
    char dump[16];
    ec_collect(pid, rd, dump, sizeof(dump));

    emc_account a;
    ec_account(&a, port, 1);
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "imap down: open (no network yet)");
    char **ids = (char **)1;
    int n = 7;
    ASSERT(emc_list_messages(c, NULL, 0, &ids, &n) == EMC_ERR_CONNECT,
           "imap down: EMC_ERR_CONNECT");
    ASSERT(ids == NULL && n == 0, "imap down: outputs are cleared");
    ASSERT(strstr(emc_last_error(c), "connect") != NULL, "imap down: error names the step");
    emc_close(c);
}

static void test_ec_send_saves_copy(void)
{
    int smtp_port = 0, smtp_rd = -1, imap_port = 0, imap_rd = -1;
    pid_t smtp = ec_start(SRV_SMTP, &smtp_port, &smtp_rd);
    pid_t imap = ec_start(SRV_IMAP, &imap_port, &imap_rd);
    ASSERT(smtp > 0 && imap > 0, "copy: start servers");

    emc_account a;
    ec_account(&a, imap_port, smtp_port);
    a.sent_folder = "Sent";
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "copy: open");
    emc_message m = {.to = "rcpt@example.org", .subject = "Kept", .body = "b"};
    emc_send_result r;
    ASSERT(emc_send(c, &m, &r) == EMC_OK, "copy: send succeeds");
    ASSERT(r.saved_copy == 1, "copy: a sent copy was appended");
    emc_close(c);

    char got[8192], appended[256];
    ec_collect(smtp, smtp_rd, got, sizeof(got));
    ec_collect(imap, imap_rd, appended, sizeof(appended));
    ASSERT(strstr(appended, "APPEND") != NULL, "copy: the IMAP server saw the APPEND");
}

static void test_ec_send_copy_failure_is_not_a_send_failure(void)
{
    int smtp_port = 0, smtp_rd = -1, imap_port = 0, imap_rd = -1;
    pid_t smtp = ec_start(SRV_SMTP, &smtp_port, &smtp_rd);
    pid_t imap = ec_start(SRV_IMAP, &imap_port, &imap_rd);
    ASSERT(smtp > 0 && imap > 0, "copy fail: start servers");
    kill(imap, SIGKILL);
    char dump[16];
    ec_collect(imap, imap_rd, dump, sizeof(dump));

    emc_account a;
    ec_account(&a, imap_port, smtp_port);
    a.sent_folder = "Sent";
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "copy fail: open");
    emc_message m = {.to = "rcpt@example.org", .subject = "Sent anyway", .body = "b"};
    emc_send_result r;
    ASSERT(emc_send(c, &m, &r) == EMC_OK, "copy fail: the send still succeeds");
    ASSERT(r.saved_copy == 0 && r.message_id[0] == '<',
           "copy fail: no copy, the id is returned");
    emc_close(c);
    char got[8192];
    ec_collect(smtp, smtp_rd, got, sizeof(got));
}

static void test_ec_gmail_send(void)
{
    int port = 0, rd = -1;
    pid_t pid = ec_start(SRV_GMAIL, &port, &rd);
    ASSERT(pid > 0, "gmail: start server");
    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/gmail/v1/users/me", port);
    setenv("GMAIL_TEST_TOKEN", "unit_test_token", 1);
    setenv("GMAIL_API_BASE_URL", base, 1);

    emc_account a = {.gmail = 1, .user = "me@example.org",
                     .gmail_refresh_token = "refresh"};
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "gmail: open");
    emc_message m = {.to = "rcpt@example.org", .subject = "Via API", .body = "b"};
    emc_send_result r;
    emc_status st = emc_send(c, &m, &r);
    emc_close(c);
    unsetenv("GMAIL_TEST_TOKEN");
    unsetenv("GMAIL_API_BASE_URL");

    char got[16384];
    ec_collect(pid, rd, got, sizeof(got));
    ASSERT(st == EMC_OK, "gmail: send succeeds");
    ASSERT(strstr(got, "POST") != NULL && strstr(got, "/messages/send") != NULL,
           "gmail: the API was called");
    ASSERT(r.message_id[0] == '<' && r.saved_copy == 0,
           "gmail: the id is returned, no IMAP copy");
}

void test_email_client(void)
{
    signal(SIGPIPE, SIG_IGN);
    RUN_TEST(test_ec_version_and_strerror);
    RUN_TEST(test_ec_open_validation);
    RUN_TEST(test_ec_open_copies_account);
    RUN_TEST(test_ec_send_validation);
    RUN_TEST(test_ec_send_smtp);
    RUN_TEST(test_ec_send_smtp_unreachable);
    RUN_TEST(test_ec_imap_read);
    RUN_TEST(test_ec_imap_survives_signals);
    RUN_TEST(test_ec_imap_unreachable);
    RUN_TEST(test_ec_send_saves_copy);
    RUN_TEST(test_ec_send_copy_failure_is_not_a_send_failure);
    RUN_TEST(test_ec_gmail_send);
}
