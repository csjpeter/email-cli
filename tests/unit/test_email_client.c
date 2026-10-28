/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-15 */
/* Unit tests of libemail-client's public interface (EMAIL-101). */
#include "test_helpers.h"
#include "email-client.h"
#include "config_store.h"
#include "config.h"
#include <sys/stat.h>

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

/* Set before ec_start(): the IMAP mock logs in, then answers NO to every
 * mailbox command (EMAIL-117: the failure paths of the library). */
static int g_imap_no;

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
        } else if (g_imap_no && !strstr(buf, "LOGOUT")) {
            snprintf(resp, sizeof(resp), "%s NO mailbox command refused\r\n", tag);
        } else if (strstr(buf, "SELECT")) {
            snprintf(resp, sizeof(resp), "* 2 EXISTS\r\n%s OK [READ-WRITE] ok\r\n", tag);
        } else if (strstr(buf, "UID SEARCH")) {
            snprintf(resp, sizeof(resp), "* SEARCH %s\r\n%s OK done\r\n",
                     strstr(buf, "UNSEEN") ? "2" : "1 2", tag);
        } else if (strstr(buf, "BODY.PEEK[HEADER")) {
            static const char hdr[] = "From: a@example.org\r\nSubject: Hi\r\n\r\n";
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (UID 1 BODY[HEADER] {%d}\r\n%s)\r\n%s OK done\r\n",
                     (int)strlen(hdr), hdr, tag);
        } else if (strstr(buf, "UID FETCH") && strstr(buf, "FLAGS")) {
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (UID 1 FLAGS (\\Seen))\r\n%s OK done\r\n", tag);
        } else if (strstr(buf, "UID STORE")) {
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (FLAGS (\\Seen \\Flagged))\r\n%s OK done\r\n", tag);
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


/* EMAIL-236: a Gmail refusal reaches emc_last_error() with the server's words. */
static void test_ec_gmail_error_holds_servers_words(void)
{
    int lfd = 0, port = 0;
    lfd = ec_listener(&port);
    ASSERT(lfd >= 0, "gmail err: listener");
    pid_t pid = fork();
    if (pid == 0) {
        alarm(60);
        int cfd = accept(lfd, NULL, NULL);
        if (cfd >= 0) {
            char req[4096];
            ssize_t r = read(cfd, req, sizeof(req)); (void)r;
            const char *body = "{\"error\":{\"code\":403,\"message\":\"Daily Limit Exceeded\","
                               "\"errors\":[{\"reason\":\"dailyLimitExceeded\"}],"
                               "\"status\":\"PERMISSION_DENIED\"}}";
            char head[200];
            snprintf(head, sizeof(head), "HTTP/1.1 403 Forbidden\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n", strlen(body));
            ssize_t w = write(cfd, head, strlen(head)); (void)w;
            w = write(cfd, body, strlen(body)); (void)w;
            close(cfd);
        }
        close(lfd);
        GCOV_FLUSH();
        _exit(0);
    }
    close(lfd);
    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/gmail/v1/users/me", port);
    setenv("GMAIL_TEST_TOKEN", "unit_test_token", 1);
    setenv("GMAIL_API_BASE_URL", base, 1);
    emc_account a = {.gmail = 1, .user = "me@example.org", .gmail_refresh_token = "refresh"};
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "gmail err: open");
    char **names = NULL;
    int n = 0;
    ASSERT(emc_list_folders(c, &names, &n) != EMC_OK, "gmail err: refused");
    const char *e = emc_last_error(c);
    ASSERT(strstr(e, "list folders") && strstr(e, "GET /labels") && strstr(e, "HTTP 403") &&
           strstr(e, "PERMISSION_DENIED") && strstr(e, "Daily Limit Exceeded") &&
           strstr(e, "dailyLimitExceeded") && !strstr(e, "unit_test_token"), e);
    emc_close(c);
    unsetenv("GMAIL_TEST_TOKEN");
    unsetenv("GMAIL_API_BASE_URL");
    int st;
    waitpid(pid, &st, 0);
}


static void test_ec_new_functions_validate_arguments(void)
{
    emc_account a = {.host = "imaps://127.0.0.1:1", .user = "u@example.org"};
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "args: open");
    char **names = NULL, **ids = NULL;
    int n = 0;
    char sep = 0;
    char (*uids)[17] = NULL;
    emc_select_result sr;
    emc_flag_update *fu = NULL;
    char *out = NULL;

    ASSERT(emc_connect(NULL) == EMC_ERR_ARGUMENT, "args: connect NULL");
    ASSERT(emc_list_names(c, NULL, &n, &sep) == EMC_ERR_ARGUMENT, "args: list names");
    ASSERT(emc_list_with_ids(c, &names, NULL, &n) == EMC_ERR_ARGUMENT, "args: list ids");
    ASSERT(emc_select(NULL, "INBOX") == EMC_ERR_ARGUMENT, "args: select NULL client");
    ASSERT(emc_select_ext(c, NULL, 0, 0, &sr) == EMC_ERR_ARGUMENT, "args: select_ext folder");
    ASSERT(emc_select_ext(c, "INBOX", 0, 0, NULL) == EMC_ERR_ARGUMENT, "args: select_ext res");
    ASSERT(emc_search(c, EMC_SEARCH_ALL, NULL, &n) == EMC_ERR_ARGUMENT, "args: search");
    ASSERT(emc_fetch_headers(c, "") == NULL, "args: headers without uid");
    ASSERT(emc_fetch_body(NULL, "1") == NULL, "args: body NULL client");
    ASSERT(emc_fetch_flags(c, NULL) == -1, "args: flags without uid");
    ASSERT(emc_fetch_flags_changedsince(c, 1, NULL, &n) == EMC_ERR_ARGUMENT, "args: changedsince");
    ASSERT(emc_set_flag(c, "", "\\Seen", 1) == EMC_ERR_ARGUMENT, "args: set_flag uid");
    ASSERT(emc_set_flag(c, "1", "", 1) == EMC_ERR_ARGUMENT, "args: set_flag flag");
    ASSERT(emc_mark_junk(c, "") == EMC_ERR_ARGUMENT, "args: junk");
    ASSERT(emc_mark_notjunk(c, NULL) == EMC_ERR_ARGUMENT, "args: not junk");
    ASSERT(emc_trash(c, "") == EMC_ERR_ARGUMENT, "args: trash");
    ASSERT(emc_delete(c, "") == EMC_ERR_ARGUMENT, "args: delete");
    ASSERT(emc_move_to_folder(c, "1", "") == EMC_ERR_ARGUMENT, "args: move");
    ASSERT(emc_modify_label(c, "1", "", 1) == EMC_ERR_ARGUMENT, "args: modify label");
    ASSERT(emc_create_label(c, "", &out) == EMC_ERR_ARGUMENT && out == NULL, "args: create label");
    ASSERT(emc_delete_label(c, "") == EMC_ERR_ARGUMENT, "args: delete label");
    ASSERT(emc_create_folder(c, "") == EMC_ERR_ARGUMENT, "args: create folder");
    ASSERT(emc_delete_folder(c, "") == EMC_ERR_ARGUMENT, "args: delete folder");
    ASSERT(emc_append(c, "Sent", NULL, 0) == EMC_ERR_ARGUMENT, "args: append");
    ASSERT(emc_rebuild_indexes(NULL) == EMC_ERR_ARGUMENT, "args: rebuild");
    ASSERT(emc_gmail_authorize(NULL, &out) == EMC_ERR_ARGUMENT, "args: authorize");
    {
        emc_account ga = {.user = "u@gmail.com", .gmail = 1};
        emc_gmail_auth au = {0};
        ASSERT(emc_gmail_authorize_with(NULL, &au, &out) == EMC_ERR_ARGUMENT, "args: authorize_with account");
        ASSERT(emc_gmail_authorize_with(&ga, NULL, &out) == EMC_ERR_ARGUMENT, "args: authorize_with auth");
        ASSERT(emc_gmail_authorize_with(&ga, &au, &out) == EMC_ERR_ARGUMENT && out == NULL &&
               au.error[0] != '\0', "authorize_with: no show_url is refused, with a reason");
        au.mode = EMC_GMAIL_AUTH_PASTE;
        au.show_url = (void (*)(const char *, int, void *))abort;
        ASSERT(emc_gmail_authorize_with(&ga, &au, &out) == EMC_ERR_ARGUMENT &&
               strstr(au.error, "read_redirect") != NULL,
               "authorize_with: paste mode without read_redirect is refused");
        ASSERT(emc_gmail_authorize(&ga, &out) == EMC_ERR_ARGUMENT && out == NULL,
               "the old call without callbacks only refuses");
    }
    ASSERT(emc_compose(NULL, &out, (size_t *)&n) == EMC_ERR_ARGUMENT, "args: compose");
    ASSERT(emc_send_wire(c, "", "x@example.org", "m", 1, NULL) == EMC_ERR_ARGUMENT,
           "args: send_wire from");
    ASSERT(emc_reply_meta(NULL, &out, &out, &out) == EMC_ERR_ARGUMENT, "args: reply_meta");
    (void)names; (void)ids; (void)uids; (void)fu;
    emc_close(c);
}

static void test_ec_compose_and_reply_meta(void)
{
    emc_message m = {.from = "me@example.org", .to = "you@example.org",
                     .cc = "cc@example.org", .subject = "Plan", .body = "Text\n",
                     .in_reply_to = "<orig@example.org>"};
    char *wire = NULL;
    size_t len = 0;
    emc_status st = emc_compose(&m, &wire, &len);
    ASSERT(st == EMC_OK && wire != NULL && len > 0, "compose: builds the message");
    int has = strstr(wire, "Subject: Plan") && strstr(wire, "To: you@example.org") &&
              strstr(wire, "Cc: cc@example.org") && strstr(wire, "Message-ID: <") &&
              strstr(wire, "In-Reply-To: <orig@example.org>");
    char *reply_to = NULL, *subject = NULL, *id = NULL;
    emc_status rs = emc_reply_meta(wire, &reply_to, &subject, &id);
    int meta_ok = rs == EMC_OK && reply_to && strstr(reply_to, "me@example.org") &&
                  subject && strcmp(subject, "Re: Plan") == 0 && id && id[0] == '<';
    free(reply_to); free(subject); free(id); free(wire);
    ASSERT(has, "compose: the headers are on the wire");
    ASSERT(meta_ok, "reply_meta: address, Re: subject and id of the composed message");
    emc_message no_from = {.to = "you@example.org", .subject = "s", .body = "b"};
    ASSERT(emc_compose(&no_from, &wire, &len) == EMC_ERR_ARGUMENT && wire == NULL,
           "compose: a sender is required");
    char *a = NULL, *b = NULL, *c = NULL;
    ASSERT(emc_reply_meta("", &a, &b, &c) != EMC_OK || (free(a), free(b), free(c), 1),
           "reply_meta: an empty message is handled");
}

static void test_ec_send_wire_smtp(void)
{
    int port = 0, rd = -1;
    pid_t pid = ec_start(SRV_SMTP, &port, &rd);
    ASSERT(pid > 0, "wire: start server");
    emc_account a;
    ec_account(&a, 1, port);
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "wire: open");
    static const char wire[] = "From: user@example.org\r\nTo: r@example.org\r\n"
                               "Subject: Raw\r\nMessage-ID: <raw-1@example.org>\r\n\r\nBody\r\n";
    emc_send_result r;
    emc_status st = emc_send_wire(c, "user@example.org", "r@example.org", wire,
                                  strlen(wire), &r);
    emc_close(c);
    char got[4096];
    ec_collect(pid, rd, got, sizeof(got));
    ASSERT(st == EMC_OK && r.status == EMC_OK, "wire: sent");
    ASSERT(strcmp(r.message_id, "<raw-1@example.org>") == 0, "wire: the id of the message");
    ASSERT(strstr(got, "Subject: Raw") != NULL, "wire: the server got the message");
}

static void test_ec_imap_operations(void)
{
    int port = 0, rd = -1;
    pid_t pid = ec_start(SRV_IMAP, &port, &rd);
    ASSERT(pid > 0, "ops: start server");
    emc_account a;
    ec_account(&a, port, 1);
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "ops: open");
    int failed = 0;
#define CHECK(cond) do { if (!(cond)) failed = __LINE__; } while (0)
    CHECK(emc_uses_labels(c) == 0);
    CHECK(emc_connect(c) == EMC_OK);
    CHECK(emc_uses_labels(c) == 0);
    emc_set_progress(c, NULL, NULL);

    char **names = NULL, **ids = NULL;
    int n = 0;
    char sep = 0;
    CHECK(emc_list_names(c, &names, &n, &sep) == EMC_OK && n == 2 && sep == '/');
    emc_free_list(names, n);
    CHECK(emc_list_with_ids(c, &names, &ids, &n) == EMC_OK && n == 2 &&
          strcmp(names[0], ids[0]) == 0);
    emc_free_list(names, n);
    emc_free_list(ids, n);

    CHECK(emc_select(c, "INBOX") == EMC_OK);
    emc_select_result sr;
    CHECK(emc_select_ext(c, "INBOX", 0, 0, &sr) == EMC_OK);
    free(sr.vanished_uids);
    char (*uids)[17] = NULL;
    CHECK(emc_search(c, EMC_SEARCH_ALL, &uids, &n) == EMC_OK && n == 2);
    free(uids);
    CHECK(emc_search(c, EMC_SEARCH_UNREAD, &uids, &n) == EMC_OK && n == 1);
    free(uids);

    char *hdr = emc_fetch_headers(c, "0000000000000001");
    CHECK(hdr && strstr(hdr, "Subject: Hi"));
    free(hdr);
    char *body = emc_fetch_body(c, "0000000000000001");
    CHECK(body && strstr(body, "Hello."));
    free(body);
    CHECK(emc_fetch_flags(c, "0000000000000001") >= 0);

    CHECK(emc_set_flag(c, "0000000000000001", "\\Flagged", 1) == EMC_OK);
    CHECK(emc_mark_junk(c, "0000000000000001") == EMC_OK);
    CHECK(emc_mark_notjunk(c, "0000000000000001") == EMC_OK);
    CHECK(emc_trash(c, "0000000000000001") == EMC_OK);
    CHECK(emc_delete(c, "0000000000000001") == EMC_OK);
    CHECK(emc_move_to_folder(c, "0000000000000001", "Archive") == EMC_OK);
    CHECK(emc_modify_label(c, "0000000000000001", "INBOX", 1) == EMC_OK);  /* IMAP: no-op */
    CHECK(emc_create_folder(c, "Projects") == EMC_OK);
    CHECK(emc_delete_folder(c, "Projects") == EMC_OK);
    CHECK(emc_append(c, "Drafts", "From: a@example.org\r\n\r\nx\r\n", 25) == EMC_OK);
    CHECK(emc_sync(c, 0) == EMC_OK);              /* IMAP: the caller syncs */
    CHECK(emc_create_label(c, "Work", NULL) != EMC_OK);   /* labels are Gmail's */
#undef CHECK
    emc_close(c);
    char dump[64];
    ec_collect(pid, rd, dump, sizeof(dump));
    ASSERT(failed == 0, "ops: every operation of the mail access set works (line in failed)");
    (void)failed;
}


/* EMAIL-117: a server that refuses the mailbox commands is a protocol error
 * of the operation, never a crash or a half-filled result. */
static void test_ec_imap_refusals(void)
{
    g_imap_no = 1;
    int port = 0, rd = -1;
    pid_t pid = ec_start(SRV_IMAP, &port, &rd);
    g_imap_no = 0;
    ASSERT(pid > 0, "refusals: start server");
    emc_account a;
    ec_account(&a, port, 1);
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "refusals: open");
    int failed = 0;
#define CHECK(cond) do { if (!(cond) && !failed) failed = __LINE__; } while (0)
    char **names = NULL, **ids = NULL;
    int n = -1;
    char sep = 0;
    CHECK(emc_list_folders(c, &names, &n) == EMC_ERR_PROTOCOL && names == NULL && n == 0);
    CHECK(emc_list_messages(c, "INBOX", 0, &ids, &n) == EMC_ERR_PROTOCOL && ids == NULL);
    CHECK(emc_list_names(c, &names, &n, &sep) == EMC_ERR_PROTOCOL && names == NULL && n == 0);
    CHECK(emc_list_with_ids(c, &names, &ids, &n) == EMC_ERR_PROTOCOL &&
          names == NULL && ids == NULL && n == 0);
    CHECK(emc_select(c, "INBOX") == EMC_ERR_PROTOCOL);
    emc_select_result sr;
    CHECK(emc_select_ext(c, "INBOX", 0, 0, &sr) == EMC_ERR_PROTOCOL);
    char (*uids)[17] = NULL;
    CHECK(emc_search(c, EMC_SEARCH_ALL, &uids, &n) == EMC_ERR_PROTOCOL && uids == NULL && n == 0);
    emc_flag_update *fu = NULL;
    CHECK(emc_fetch_flags_changedsince(c, 5, &fu, &n) == EMC_ERR_PROTOCOL);
    CHECK(emc_last_error(c)[0] != '\0');
#undef CHECK
    emc_close(c);
    char dump[64];
    ec_collect(pid, rd, dump, sizeof(dump));
    ASSERT(failed == 0, "refusals: every refused operation reports a protocol error (line in failed)");
}

/* EMAIL-117: what emc_send refuses before anything is sent. */
static void test_ec_send_refusals(void)
{
    emc_account a = {.gmail = 1, .gmail_refresh_token = "refresh"};   /* a Gmail account without an address */
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "send refusals: open without an address");
    emc_message m = {.to = "x@example.org", .subject = "s", .body = "b"};
    emc_send_result r;
    ASSERT(emc_send(c, &m, &r) == EMC_ERR_CONFIG && r.status == EMC_ERR_CONFIG,
           "send: no sender at all is a configuration error");
    ASSERT(strstr(emc_last_error(c), "no sender") != NULL, "send: and says so");
    emc_close(c);

    emc_account b = {.host = "imaps://127.0.0.1:1", .user = "u@example.org", .insecure_tls = 1};
    ASSERT(emc_open(&c, &b) == EMC_OK, "send refusals: open");
    const char *const missing[] = { "/no/such/attachment.bin" };
    emc_message att = {.to = "x@example.org", .subject = "s", .body = "b",
                       .attachments = missing, .attachment_count = 1};
    ASSERT(emc_send(c, &att, &r) == EMC_ERR_ARGUMENT, "send: a missing attachment is refused");
    ASSERT(strstr(emc_last_error(c), "cannot build") != NULL, "send: and says why");
    emc_close(c);
}

/* EMAIL-117: a wire message through the Gmail API. */
static void test_ec_gmail_send_wire(void)
{
    int port = 0, rd = -1;
    pid_t pid = ec_start(SRV_GMAIL, &port, &rd);
    ASSERT(pid > 0, "gmail wire: start server");
    char base[128];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/gmail/v1/users/me", port);
    setenv("GMAIL_TEST_TOKEN", "unit_test_token", 1);
    setenv("GMAIL_API_BASE_URL", base, 1);
    emc_account a = {.gmail = 1, .user = "me@example.org", .gmail_refresh_token = "refresh"};
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "gmail wire: open");
    static const char wire[] = "From: me@example.org\r\nTo: r@example.org\r\n"
                               "Subject: Raw\r\nMessage-ID: <raw-g1@example.org>\r\n\r\nBody\r\n";
    emc_send_result r;
    emc_status st = emc_send_wire(c, "me@example.org", "r@example.org", wire, strlen(wire), &r);
    emc_close(c);
    unsetenv("GMAIL_TEST_TOKEN");
    unsetenv("GMAIL_API_BASE_URL");
    char got[16384];
    ec_collect(pid, rd, got, sizeof(got));
    ASSERT(st == EMC_OK && r.status == EMC_OK, "gmail wire: sent");
    ASSERT(strstr(got, "/messages/send") != NULL, "gmail wire: through the API");
}

static void test_ec_log_open_writes_the_file(void)
{
    const char *path = TP("emc-library.log");
    emc_log_open(path, 1);
    emc_account a = {.host = "imaps://127.0.0.1:1", .user = "u@example.org",
                     .insecure_tls = 1};
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "log: open");
    emc_status st = emc_connect(c);   /* refused: logs an error */
    emc_close(c);
    emc_log_close();
    emc_log_open("", 0);              /* blank path: nothing happens */
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL, "log: the file exists");
    char line[512];
    int saw_error = 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "connect to 127.0.0.1:1 failed")) saw_error = 1;
    fclose(f);
    ASSERT(st == EMC_ERR_CONNECT, "log: the connect failed");
    ASSERT(saw_error, "log: the library's failure is in the file");
}

/* ── emc_account_load (EMAIL-231) ──────────────────────────────────── */

static void ec_write_account(const char *name, const char *body)
{
    char path[512];
    snprintf(path, sizeof(path), "%s", TP("email-cli-test-home-load/.config"));
    mkdir(TP("email-cli-test-home-load"), 0700);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/email-cli", TP("email-cli-test-home-load/.config"));
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/email-cli/accounts", TP("email-cli-test-home-load/.config"));
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/email-cli/accounts/%s", TP("email-cli-test-home-load/.config"), name);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/email-cli/accounts/%s/config.ini", TP("email-cli-test-home-load/.config"), name);
    FILE *f = fopen(path, "w");
    ASSERT(f != NULL, "account file written");
    fputs(body, f);
    fclose(f);
}

static void test_ec_account_load(void)
{
    char *old_home = getenv("HOME");
    char *old_xdg = getenv("XDG_CONFIG_HOME");
    char *saved_home = old_home ? strdup(old_home) : NULL;
    char *saved_xdg = old_xdg ? strdup(old_xdg) : NULL;
    setenv("HOME", TP("email-cli-test-home-load"), 1);
    unsetenv("XDG_CONFIG_HOME");
    char err[256];
    emc_account a;

    /* argument errors */
    ASSERT(emc_account_load(NULL, &a, err, sizeof(err)) == EMC_ERR_ARGUMENT, "NULL name");
    ASSERT(emc_account_load("x", NULL, err, sizeof(err)) == EMC_ERR_ARGUMENT, "NULL out");
    ASSERT(emc_account_load("", &a, err, sizeof(err)) == EMC_ERR_ARGUMENT, "empty name");

    /* missing account */
    ASSERT(emc_account_load("nobody@example.org", &a, err, sizeof(err)) == EMC_ERR_NO_ACCOUNT,
           "missing account gives EMC_ERR_NO_ACCOUNT");
    ASSERT(strstr(err, "no such account") != NULL, "missing account text");
    ASSERT(a.host == NULL && a.pass == NULL, "out zeroed on failure");
    ASSERT(emc_account_load("../x", &a, err, sizeof(err)) == EMC_ERR_NO_ACCOUNT,
           "a path is not an account name");
    ASSERT(emc_account_load("nobody@example.org", &a, NULL, 0) == EMC_ERR_NO_ACCOUNT,
           "no error buffer is allowed");

    /* plaintext credentials */
    ec_write_account("plain@example.org",
        "EMAIL_HOST=imaps://imap.example.org\nEMAIL_USER=plain@example.org\n"
        "EMAIL_PASS=secret1\nEMAIL_SENT_FOLDER=Sent\nSMTP_HOST=smtps://smtp.example.org\n"
        "SMTP_PORT=465\nSMTP_USER=smtpuser\nSMTP_PASS=secret2\n");
    ASSERT(emc_account_load("plain@example.org", &a, err, sizeof(err)) == EMC_OK, "plain loads");
    ASSERT(strcmp(a.host, "imaps://imap.example.org") == 0, "host");
    ASSERT(strcmp(a.user, "plain@example.org") == 0, "user");
    ASSERT(strcmp(a.pass, "secret1") == 0, "pass");
    ASSERT(strcmp(a.folder, "INBOX") == 0, "folder default");
    ASSERT(strcmp(a.sent_folder, "Sent") == 0, "sent folder");
    ASSERT(strcmp(a.smtp_host, "smtps://smtp.example.org") == 0, "smtp host");
    ASSERT(a.smtp_port == 465, "smtp port");
    ASSERT(strcmp(a.smtp_user, "smtpuser") == 0, "smtp user");
    ASSERT(strcmp(a.smtp_pass, "secret2") == 0, "smtp pass");
    emc_client *c = NULL;
    ASSERT(emc_open(&c, &a) == EMC_OK, "the loaded account opens");
    emc_close(c);
    emc_account_free(&a);
    ASSERT(a.host == NULL && a.pass == NULL, "free zeroes");
    emc_account_free(NULL);

    /* the file is left as it was (no re-encryption) */
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/email-cli/accounts/plain@example.org/config.ini",
                 TP("email-cli-test-home-load/.config"));
        FILE *f = fopen(path, "r");
        char line[256];
        int plain_left = 0;
        while (f && fgets(line, sizeof(line), f))
            if (strncmp(line, "EMAIL_PASS=secret1", 18) == 0) plain_left = 1;
        if (f) fclose(f);
        ASSERT(plain_left, "load does not rewrite the file");
    }

    /* enc: credentials, written by email-cli's own store */
    {
        Config cfg = {0};
        cfg.host = strdup("imaps://imap.example.org");
        cfg.user = strdup("enc@example.org");
        cfg.pass = strdup("topsecret");
        cfg.folder = strdup("INBOX");
        ASSERT(config_save_to_store(&cfg) == 0, "store save");
        free(cfg.host); free(cfg.user); free(cfg.pass); free(cfg.folder);
    }
    ASSERT(emc_account_load("enc@example.org", &a, err, sizeof(err)) == EMC_OK, "enc loads");
    ASSERT(strcmp(a.pass, "topsecret") == 0, "enc: password decrypted");
    emc_account_free(&a);

    /* undecryptable */
    ec_write_account("bad@example.org",
        "EMAIL_HOST=imaps://imap.example.org\nEMAIL_USER=bad@example.org\n"
        "EMAIL_PASS=enc:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n");
    ASSERT(emc_account_load("bad@example.org", &a, err, sizeof(err)) == EMC_ERR_CREDENTIAL,
           "undecryptable gives EMC_ERR_CREDENTIAL");
    ASSERT(strstr(err, "cannot be decrypted") != NULL, "undecryptable text");
    ASSERT(a.pass == NULL, "no partial account");

    /* unreadable: incomplete, and a non-TLS server */
    ec_write_account("half@example.org", "EMAIL_USER=half@example.org\n");
    ASSERT(emc_account_load("half@example.org", &a, err, sizeof(err)) == EMC_ERR_CONFIG,
           "incomplete gives EMC_ERR_CONFIG");
    ASSERT(strstr(err, "cannot be read") != NULL, "unreadable text");
    ec_write_account("plainnet@example.org",
        "EMAIL_HOST=imap://imap.example.org\nEMAIL_USER=plainnet@example.org\nEMAIL_PASS=x\n");
    ASSERT(emc_account_load("plainnet@example.org", &a, err, sizeof(err)) == EMC_ERR_CONFIG,
           "a non-TLS server is refused");

    /* by address, when the directory has another name */
    ec_write_account("work",
        "EMAIL_HOST=imaps://imap.example.org\nEMAIL_USER=byaddr@example.org\nEMAIL_PASS=p\n");
    ASSERT(emc_account_load("byaddr@example.org", &a, err, sizeof(err)) == EMC_OK,
           "found by address");
    emc_account_free(&a);

    if (saved_home) setenv("HOME", saved_home, 1); else unsetenv("HOME");
    if (saved_xdg) setenv("XDG_CONFIG_HOME", saved_xdg, 1);
    free(saved_home); free(saved_xdg);
}

void test_email_client(void)
{
    signal(SIGPIPE, SIG_IGN);
    RUN_TEST(test_ec_account_load);
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
    RUN_TEST(test_ec_gmail_error_holds_servers_words);
    RUN_TEST(test_ec_new_functions_validate_arguments);
    RUN_TEST(test_ec_compose_and_reply_meta);
    RUN_TEST(test_ec_send_wire_smtp);
    RUN_TEST(test_ec_imap_operations);
    RUN_TEST(test_ec_imap_refusals);
    RUN_TEST(test_ec_send_refusals);
    RUN_TEST(test_ec_gmail_send_wire);
    RUN_TEST(test_ec_log_open_writes_the_file);
}
