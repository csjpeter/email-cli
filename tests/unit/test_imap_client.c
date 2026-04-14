#include "test_helpers.h"
#include "imap_client.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/*
 * Create a listening TCP socket bound to a random loopback port.
 * Returns the fd and fills *port_out with the actual port number.
 * Returns -1 on error.
 */
static int make_listener(int *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0; /* OS picks a free port */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    *port_out = ntohs(addr.sin_port);
    return fd;
}

/*
 * Create a server-side SSL_CTX loaded with the test self-signed certificate.
 * Returns NULL on failure.
 */
static SSL_CTX *create_server_ctx(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        return NULL;
    }
    if (SSL_CTX_use_certificate_file(ctx, "tests/certs/test.crt", SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "Failed to load test cert: tests/certs/test.crt\n");
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, "tests/certs/test.key", SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "Failed to load test key: tests/certs/test.key\n");
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

/*
 * Minimal IMAP server child process (TLS).
 *
 * Accepts ONE connection over TLS, sends `greeting`, then loops reading lines
 * and replies according to the command:
 *   LOGIN  → sends login_reply
 *   LOGOUT → sends BYE + OK, exits cleanly
 *   other  → sends "TAG BAD Unknown"
 *
 * After the first LOGOUT (or client disconnect) the child exits.
 */
static void run_mock_server(int listen_fd,
                             const char *greeting,
                             const char *login_reply,
                             SSL_CTX *ctx) {
    int cfd = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (cfd < 0) {
        SSL_CTX_free(ctx);
        _exit(1);
    }

    /* timeout so the child never hangs in CI */
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    SSL *ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);   /* child no longer needs ctx after SSL_new */
    SSL_set_fd(ssl, cfd);
    if (SSL_accept(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(cfd);
        _exit(1);
    }

    SSL_write(ssl, greeting, (int)strlen(greeting));

    char buf[1024];
    while (1) {
        int n = SSL_read(ssl, buf, (int)(sizeof(buf) - 1));
        if (n <= 0) break;
        buf[n] = '\0';

        /* Extract tag (first token) */
        char tag[32] = "*";
        sscanf(buf, "%31s", tag);

        if (strstr(buf, "LOGIN")) {
            SSL_write(ssl, login_reply, (int)strlen(login_reply));
        } else if (strstr(buf, "APPEND")) {
            /* Handle LITERAL+ {N+} and synchronising {N} literals */
            char *lbrace = strrchr(buf, '{');
            long lsize = 0;
            int  sync  = 1;
            if (lbrace) {
                char *end = NULL;
                lsize = strtol(lbrace + 1, &end, 10);
                if (end && *end == '+') sync = 0;
            }
            if (lsize <= 0) {
                char bad2[64];
                snprintf(bad2, sizeof(bad2), "%s BAD Missing size\r\n", tag);
                SSL_write(ssl, bad2, (int)strlen(bad2));
            } else {
                if (sync) SSL_write(ssl, "+ OK\r\n", 6);
                /* Drain the literal bytes already in buf + remaining reads */
                char *ptr = lbrace ? strchr(lbrace, '}') : NULL;
                long already = 0;
                if (ptr) {
                    /* skip past '}' and optional '+' and '\r\n' */
                    ptr++;
                    if (*ptr == '+') ptr++;
                    if (*ptr == '\r') ptr++;
                    if (*ptr == '\n') ptr++;
                    already = (long)(buf + n - ptr);
                    if (already > lsize) already = lsize;
                }
                long remaining = lsize - already;
                char tmp[512];
                while (remaining > 0) {
                    int r2 = SSL_read(ssl, tmp,
                                     remaining > (long)sizeof(tmp) ?
                                     (int)sizeof(tmp) : (int)remaining);
                    if (r2 <= 0) break;
                    remaining -= r2;
                }
                char ok2[80];
                snprintf(ok2, sizeof(ok2),
                         "%s OK [APPENDUID 1 99] APPEND completed\r\n", tag);
                SSL_write(ssl, ok2, (int)strlen(ok2));
            }
        } else if (strstr(buf, "LOGOUT")) {
            const char *bye = "* BYE Logging out\r\n";
            SSL_write(ssl, bye, (int)strlen(bye));
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK LOGOUT completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
            break;
        } else {
            char bad[64];
            snprintf(bad, sizeof(bad), "%s BAD Unknown\r\n", tag);
            SSL_write(ssl, bad, (int)strlen(bad));
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(cfd);
    _exit(0);
}

/* ── Test: imap_connect / read_response ─────────────────────────────────── */

/*
 * Regression test for the use-after-free bug in read_response().
 *
 * Previously, linebuf_free() was called before strncasecmp(status, "OK"),
 * so if the allocator reused the memory the check would fail, returning -1
 * even though the server sent "TAG OK Logged in".  This caused spurious
 * "LOGIN failed" errors in production.
 *
 * The test verifies that imap_connect() returns non-NULL (login accepted)
 * when the server genuinely responds with OK — including the long
 * [CAPABILITY ...] inline text that Dovecot sends, which increases the
 * chance of the allocator reusing freed memory.
 */
void test_imap_connect_login_ok(void) {
    int port = 0;
    int lfd  = make_listener(&port);
    ASSERT(lfd >= 0, "make_listener: could not bind");

    SSL_CTX *ctx = create_server_ctx();
    ASSERT(ctx != NULL, "create_server_ctx failed");

    /* Simulate a real Dovecot-style OK with a long CAPABILITY string so the
     * allocator is more likely to reuse memory if a bug exists. */
    const char *greeting =
        "* OK [CAPABILITY IMAP4rev1 SASL-IR LOGIN-REFERRALS ID ENABLE IDLE"
        " LITERAL+ AUTH=PLAIN AUTH=LOGIN] Dovecot ready.\r\n";
    char login_reply[512];
    snprintf(login_reply, sizeof(login_reply),
             "A0001 OK [CAPABILITY IMAP4rev1 SASL-IR LOGIN-REFERRALS ID ENABLE"
             " IDLE SORT SORT=DISPLAY THREAD=REFERENCES THREAD=REFS"
             " THREAD=ORDEREDSUBJECT MULTIAPPEND URL-PARTIAL CATENATE UNSELECT"
             " CHILDREN NAMESPACE UIDPLUS LIST-EXTENDED I18NLEVEL=1 CONDSTORE"
             " QRESYNC ESEARCH ESORT SEARCHRES WITHIN CONTEXT=SEARCH"
             " LIST-STATUS BINARY MOVE SNIPPET=FUZZY PREVIEW=FUZZY PREVIEW"
             " STATUS=SIZE SAVEDATE LITERAL+ NOTIFY SPECIAL-USE QUOTA]"
             " Logged in\r\n");

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork failed");

    if (pid == 0) {
        run_mock_server(lfd, greeting, login_reply, ctx);
        /* _exit is called inside run_mock_server */
    }
    close(lfd);
    SSL_CTX_free(ctx);

    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", port);
    ImapClient *c = imap_connect(url, "user", "pass", 0);

    ASSERT(c != NULL,
           "imap_connect must return non-NULL when server responds OK "
           "(regression: use-after-free in read_response caused OK check to fail)");

    imap_disconnect(c);

    int status = 0;
    waitpid(pid, &status, 0);
}

/*
 * Verify that imap_connect() returns NULL when the server explicitly
 * rejects the login (NO / BAD response).
 */
void test_imap_connect_login_rejected(void) {
    int port = 0;
    int lfd  = make_listener(&port);
    ASSERT(lfd >= 0, "make_listener: could not bind");

    SSL_CTX *ctx = create_server_ctx();
    ASSERT(ctx != NULL, "create_server_ctx failed");

    const char *greeting     = "* OK Mock ready\r\n";
    const char *login_reply  = "A0001 NO [AUTHENTICATIONFAILED] Invalid credentials\r\n";

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork failed");

    if (pid == 0) {
        run_mock_server(lfd, greeting, login_reply, ctx);
    }
    close(lfd);
    SSL_CTX_free(ctx);

    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", port);
    ImapClient *c = imap_connect(url, "bad_user", "bad_pass", 0);

    ASSERT(c == NULL, "imap_connect must return NULL when server says NO");

    int status = 0;
    waitpid(pid, &status, 0);
}

/*
 * Verify that imap_append() correctly uses LITERAL+ (non-synchronising
 * literal, "{N+}") and the server returns OK after receiving the message.
 */
void test_imap_append_literal_plus(void) {
    int port = 0;
    int lfd  = make_listener(&port);
    ASSERT(lfd >= 0, "make_listener: could not bind");

    SSL_CTX *ctx = create_server_ctx();
    ASSERT(ctx != NULL, "create_server_ctx failed");

    const char *greeting    = "* OK Mock ready\r\n";
    const char *login_reply =
        "A0001 OK [CAPABILITY IMAP4rev1 LITERAL+] Logged in\r\n";

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork failed");

    if (pid == 0) {
        run_mock_server(lfd, greeting, login_reply, ctx);
    }
    close(lfd);
    SSL_CTX_free(ctx);

    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", port);
    ImapClient *c = imap_connect(url, "user", "pass", 0);
    ASSERT(c != NULL, "imap_append test: imap_connect must succeed");

    const char *msg = "From: a@b.com\r\nSubject: Test\r\n\r\nHello.\r\n";
    int rc = imap_append(c, "Sent", msg, strlen(msg));
    ASSERT(rc == 0, "imap_append must return 0 (OK) with LITERAL+");

    imap_disconnect(c);

    int status = 0;
    waitpid(pid, &status, 0);
}

/* ── Test suite entry point ──────────────────────────────────────────────── */

void test_imap_client(void) {
    /* Verify that a NULL pointer is handled gracefully */
    imap_disconnect(NULL);

    /* imap_connect with a bad host must return NULL, not crash */
    ImapClient *c = imap_connect("imaps://invalid.host.example.invalid",
                                  "user", "pass", 1);
    ASSERT(c == NULL, "imap_connect to invalid host should return NULL");

    test_imap_connect_login_ok();
    test_imap_connect_login_rejected();
    test_imap_append_literal_plus();
}
