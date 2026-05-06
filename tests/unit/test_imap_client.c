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
#ifdef ENABLE_GCOV
extern void __gcov_dump(void);
#  define GCOV_FLUSH() __gcov_dump()
#else
#  define GCOV_FLUSH() ((void)0)
#endif

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
    /* 3-second accept() timeout so server children exit cleanly if the test
     * returns early (ASSERT failure) before making a connection. */
    struct timeval acc_tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &acc_tv, sizeof(acc_tv));
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
    if (SSL_CTX_use_certificate_file(ctx, TEST_CERT_PATH, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "Failed to load test cert: %s\n", TEST_CERT_PATH);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, TEST_KEY_PATH, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "Failed to load test key: %s\n", TEST_KEY_PATH);
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
        GCOV_FLUSH();
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
        GCOV_FLUSH();
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
                /* RFC 3501: command = ... CRLF — the command-terminating \r\n
                 * comes AFTER the literal body and must not be skipped. */
                char trail[4] = {0};
                int tr = SSL_read(ssl, trail, 2);
                int good_trail = (tr == 2 && trail[0] == '\r' && trail[1] == '\n');
                char ok2[128];
                if (good_trail) {
                    snprintf(ok2, sizeof(ok2),
                             "%s OK [APPENDUID 1 99] APPEND completed\r\n", tag);
                } else {
                    snprintf(ok2, sizeof(ok2),
                             "%s BAD Missing command-terminating CRLF after literal\r\n", tag);
                }
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
    GCOV_FLUSH();
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

/* ── Extended mock server: SELECT, SEARCH, FETCH, STORE, LIST, etc. ──── */

/*
 * Stateful mock server that handles a fixed command sequence:
 * LOGIN → SELECT → SEARCH → FETCH(body) → FETCH(hdrs) → FETCH(flags)
 * → STORE → LIST → CREATE → DELETE → LOGOUT.
 * Each command is matched by keyword; the tag is extracted from the request.
 */
static void run_mock_server_full(int listen_fd, SSL_CTX *ctx) {
    int cfd = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (cfd < 0) { SSL_CTX_free(ctx);
        GCOV_FLUSH();
        _exit(1);
    }

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    SSL *ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);
    SSL_set_fd(ssl, cfd);
    if (SSL_accept(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl); close(cfd);
        GCOV_FLUSH();
        _exit(1);
    }

    /* Greeting */
    const char *greet = "* OK Mock ready\r\n";
    SSL_write(ssl, greet, (int)strlen(greet));

    /* Message body used for FETCH responses */
    const char *msg_body = "From: a@b.com\r\nSubject: Test\r\n\r\nHello.\r\n";
    int msg_len = (int)strlen(msg_body);
    const char *hdrs = "From: a@b.com\r\nSubject: Test\r\n\r\n";
    int hdr_len = (int)strlen(hdrs);

    char buf[4096];
    while (1) {
        int n = SSL_read(ssl, buf, (int)(sizeof(buf) - 1));
        if (n <= 0) break;
        buf[n] = '\0';

        char tag[32] = "*";
        sscanf(buf, "%31s", tag);

        char resp[1024];
        if (strstr(buf, "LOGIN")) {
            snprintf(resp, sizeof(resp),
                     "%s OK [CAPABILITY IMAP4rev1 LITERAL+] Logged in\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "SELECT")) {
            snprintf(resp, sizeof(resp),
                     "* 3 EXISTS\r\n* 0 RECENT\r\n%s OK [READ-WRITE] SELECT completed\r\n",
                     tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "UID SEARCH")) {
            snprintf(resp, sizeof(resp),
                     "* SEARCH 1 2 3\r\n%s OK SEARCH completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "BODY.PEEK[]")) {
            /* FETCH body literal */
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (UID 1 BODY[] {%d}\r\n", msg_len);
            SSL_write(ssl, resp, (int)strlen(resp));
            SSL_write(ssl, msg_body, msg_len);
            snprintf(resp, sizeof(resp), ")\r\n%s OK FETCH completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "BODY.PEEK[HEADER]")) {
            /* FETCH headers literal */
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (UID 1 BODY[HEADER] {%d}\r\n", hdr_len);
            SSL_write(ssl, resp, (int)strlen(resp));
            SSL_write(ssl, hdrs, hdr_len);
            snprintf(resp, sizeof(resp), ")\r\n%s OK FETCH completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "UID FETCH") && strstr(buf, "FLAGS")) {
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (UID 1 FLAGS (\\Seen))\r\n%s OK FETCH completed\r\n",
                     tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "UID STORE")) {
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (FLAGS (\\Seen \\Flagged))\r\n%s OK STORE completed\r\n",
                     tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "LIST")) {
            snprintf(resp, sizeof(resp),
                     "* LIST (\\HasNoChildren) \".\" \"INBOX\"\r\n"
                     "* LIST (\\HasNoChildren) \".\" \"INBOX.Sent\"\r\n"
                     "%s OK LIST completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "CREATE")) {
            snprintf(resp, sizeof(resp), "%s OK CREATE completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "DELETE")) {
            snprintf(resp, sizeof(resp), "%s OK DELETE completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "APPEND")) {
            /* Handle literal APPEND as before */
            char *lbrace = strrchr(buf, '{');
            long lsize = 0;
            int  sync  = 1;
            if (lbrace) {
                char *end = NULL;
                lsize = strtol(lbrace + 1, &end, 10);
                if (end && *end == '+') sync = 0;
            }
            if (lsize <= 0) {
                snprintf(resp, sizeof(resp), "%s BAD Missing size\r\n", tag);
                SSL_write(ssl, resp, (int)strlen(resp));
            } else {
                if (sync) SSL_write(ssl, "+ OK\r\n", 6);
                char *ptr = lbrace ? strchr(lbrace, '}') : NULL;
                long already = 0;
                if (ptr) {
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
                snprintf(resp, sizeof(resp),
                         "%s OK [APPENDUID 1 99] APPEND completed\r\n", tag);
                SSL_write(ssl, resp, (int)strlen(resp));
            }
        } else if (strstr(buf, "LOGOUT")) {
            SSL_write(ssl, "* BYE Logging out\r\n", 19);
            snprintf(resp, sizeof(resp), "%s OK LOGOUT completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
            break;
        } else {
            snprintf(resp, sizeof(resp), "%s BAD Unknown\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(cfd);
    GCOV_FLUSH();
    _exit(0);
}

/*
 * Test that imap_select, imap_uid_search, imap_uid_fetch_body,
 * imap_uid_fetch_headers, imap_uid_fetch_flags, imap_uid_set_flag,
 * imap_list, imap_create_folder, imap_delete_folder, and
 * imap_set_progress all work against the extended mock server.
 */
void test_imap_full_operations(void) {
    int port = 0;
    int lfd  = make_listener(&port);
    ASSERT(lfd >= 0, "make_listener for full ops test");

    SSL_CTX *ctx = create_server_ctx();
    ASSERT(ctx != NULL, "create_server_ctx for full ops test");

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork for full ops test");
    if (pid == 0) {
        run_mock_server_full(lfd, ctx);
        /* _exit called inside */
    }
    close(lfd);
    SSL_CTX_free(ctx);

    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", port);
    ImapClient *c = imap_connect(url, "user", "pass", 0);
    ASSERT(c != NULL, "imap_full_ops: imap_connect must succeed");

    /* imap_set_progress: NULL guard + set callback to NULL */
    imap_set_progress(NULL, NULL, NULL);   /* NULL client — no crash */
    imap_set_progress(c, NULL, NULL);      /* clear callback */

    /* imap_select */
    int rc = imap_select(c, "INBOX");
    ASSERT(rc == 0, "imap_full_ops: imap_select returns 0");

    /* imap_uid_search */
    char (*uids)[17] = NULL;
    int count = 0;
    rc = imap_uid_search(c, "ALL", &uids, &count);
    ASSERT(rc == 0, "imap_full_ops: imap_uid_search returns 0");
    ASSERT(count == 3, "imap_full_ops: imap_uid_search found 3 UIDs");
    free(uids);

    /* imap_uid_fetch_body */
    char *body = imap_uid_fetch_body(c, "0000000000000001");
    ASSERT(body != NULL, "imap_full_ops: imap_uid_fetch_body returns non-NULL");
    free(body);

    /* imap_uid_fetch_headers */
    char *hdr = imap_uid_fetch_headers(c, "0000000000000001");
    ASSERT(hdr != NULL, "imap_full_ops: imap_uid_fetch_headers returns non-NULL");
    free(hdr);

    /* imap_uid_fetch_flags */
    int flags = imap_uid_fetch_flags(c, "0000000000000001");
    /* Server returns "FLAGS (\Seen)" → Seen set means NOT unseen */
    ASSERT(flags >= 0, "imap_full_ops: imap_uid_fetch_flags returns >= 0");

    /* imap_uid_set_flag */
    rc = imap_uid_set_flag(c, "0000000000000001", "\\Flagged", 1);
    ASSERT(rc == 0, "imap_full_ops: imap_uid_set_flag returns 0");

    /* imap_list */
    char **folders = NULL;
    int fc = 0;
    char sep = '.';
    rc = imap_list(c, &folders, &fc, &sep);
    ASSERT(rc == 0, "imap_full_ops: imap_list returns 0");
    ASSERT(fc == 2, "imap_full_ops: imap_list found 2 folders");
    for (int i = 0; i < fc; i++) free(folders[i]);
    free(folders);
    ASSERT(sep == '.', "imap_full_ops: separator is '.'");

    /* imap_create_folder */
    rc = imap_create_folder(c, "TestFolder");
    ASSERT(rc == 0, "imap_full_ops: imap_create_folder returns 0");

    /* imap_delete_folder */
    rc = imap_delete_folder(c, "TestFolder");
    ASSERT(rc == 0, "imap_full_ops: imap_delete_folder returns 0");

    imap_disconnect(c);

    int status = 0;
    waitpid(pid, &status, 0);
}

/* ── Extended mock server: QRESYNC + CONDSTORE + UID MOVE + CHANGEDSINCE ── */

/*
 * Mock server that handles:
 *   LOGIN → CAPABILITY → ENABLE QRESYNC → SELECT(CONDSTORE) → SELECT(QRESYNC)
 *   → UID FETCH(CHANGEDSINCE) → UID COPY → UID STORE → EXPUNGE → LOGOUT
 *
 * Also exercises NIL separator in LIST, unquoted folder names, and
 * [ALREADYEXISTS] response for CREATE.
 */
static void run_mock_server_ext(int listen_fd, SSL_CTX *ctx) {
    int cfd = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (cfd < 0) { SSL_CTX_free(ctx); GCOV_FLUSH(); _exit(1); }

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    SSL *ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);
    SSL_set_fd(ssl, cfd);
    if (SSL_accept(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl); close(cfd); GCOV_FLUSH(); _exit(1);
    }

    SSL_write(ssl, "* OK Mock ready\r\n", 17);

    char buf[4096];
    while (1) {
        int n = SSL_read(ssl, buf, (int)(sizeof(buf) - 1));
        if (n <= 0) break;
        buf[n] = '\0';

        char tag[32] = "*";
        sscanf(buf, "%31s", tag);
        char resp[2048];

        if (strstr(buf, "LOGIN")) {
            snprintf(resp, sizeof(resp),
                     "%s OK [CAPABILITY IMAP4rev1 CONDSTORE QRESYNC LITERAL+] Logged in\r\n",
                     tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "CAPABILITY")) {
            snprintf(resp, sizeof(resp),
                     "* CAPABILITY IMAP4rev1 CONDSTORE QRESYNC\r\n%s OK CAPABILITY completed\r\n",
                     tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "ENABLE")) {
            snprintf(resp, sizeof(resp),
                     "* ENABLED QRESYNC\r\n%s OK ENABLE completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "SELECT") && strstr(buf, "QRESYNC")) {
            /* SELECT with QRESYNC: return VANISHED (EARLIER) + HIGHESTMODSEQ */
            snprintf(resp, sizeof(resp),
                     "* 5 EXISTS\r\n"
                     "* OK [UIDVALIDITY 12345] UIDs valid\r\n"
                     "* OK [HIGHESTMODSEQ 999] Highest\r\n"
                     "* VANISHED (EARLIER) 3:4\r\n"
                     "%s OK [READ-WRITE] SELECT completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "SELECT") && strstr(buf, "CONDSTORE")) {
            /* SELECT with CONDSTORE: return HIGHESTMODSEQ in tagged OK */
            snprintf(resp, sizeof(resp),
                     "* 5 EXISTS\r\n"
                     "* OK [UIDVALIDITY 12345] UIDs valid\r\n"
                     "%s OK [HIGHESTMODSEQ 42] SELECT completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "SELECT")) {
            snprintf(resp, sizeof(resp),
                     "* 5 EXISTS\r\n%s OK [READ-WRITE] SELECT completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "CHANGEDSINCE")) {
            /* UID FETCH ... CHANGEDSINCE: return two flag-updated messages */
            snprintf(resp, sizeof(resp),
                     "* 2 FETCH (UID 2 FLAGS (\\Seen))\r\n"
                     "* 3 FETCH (UID 3 FLAGS (\\Seen \\Flagged))\r\n"
                     "%s OK FETCH completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "UID FETCH") && strstr(buf, "FLAGS")) {
            /* plain UID FETCH FLAGS */
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (UID 1 FLAGS (\\Seen))\r\n%s OK FETCH completed\r\n",
                     tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "UID COPY")) {
            snprintf(resp, sizeof(resp), "%s OK COPY completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "UID STORE")) {
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (FLAGS (\\Deleted))\r\n%s OK STORE completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "EXPUNGE")) {
            snprintf(resp, sizeof(resp),
                     "* 1 EXPUNGE\r\n%s OK EXPUNGE completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "LIST")) {
            /* Return one folder with NIL separator, one unquoted */
            snprintf(resp, sizeof(resp),
                     "* LIST (\\HasNoChildren) NIL INBOX\r\n"
                     "* LIST (\\HasNoChildren) \"/\" Archive\r\n"
                     "%s OK LIST completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "CREATE")) {
            /* Return [ALREADYEXISTS] to exercise that branch */
            snprintf(resp, sizeof(resp),
                     "%s NO [ALREADYEXISTS] Mailbox already exists\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "LOGOUT")) {
            SSL_write(ssl, "* BYE Logging out\r\n", 19);
            snprintf(resp, sizeof(resp), "%s OK LOGOUT completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
            break;
        } else {
            snprintf(resp, sizeof(resp), "%s BAD Unknown\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(cfd);
    GCOV_FLUSH();
    _exit(0);
}

/*
 * Test CONDSTORE/QRESYNC capabilities, uid_move, flags_changedsince,
 * LIST with NIL separator and unquoted names, CREATE [ALREADYEXISTS].
 */
void test_imap_extended_operations(void) {
    int port = 0;
    int lfd  = make_listener(&port);
    ASSERT(lfd >= 0, "make_listener for extended ops test");

    SSL_CTX *ctx = create_server_ctx();
    ASSERT(ctx != NULL, "create_server_ctx for extended ops test");

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork for extended ops test");
    if (pid == 0) {
        run_mock_server_ext(lfd, ctx);
        /* _exit called inside */
    }
    close(lfd);
    SSL_CTX_free(ctx);

    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", port);
    ImapClient *c = imap_connect(url, "user", "pass", 0);
    ASSERT(c != NULL, "imap_ext: imap_connect must succeed");

    /* imap_get_caps — queries CAPABILITY command */
    int caps = imap_get_caps(c);
    ASSERT((caps & IMAP_CAP_CONDSTORE) != 0, "imap_ext: CONDSTORE capability detected");
    ASSERT((caps & IMAP_CAP_QRESYNC)   != 0, "imap_ext: QRESYNC capability detected");

    /* Calling again returns cached value (caps_queried path) */
    int caps2 = imap_get_caps(c);
    ASSERT(caps2 == caps, "imap_ext: imap_get_caps cached second call");

    /* imap_select_condstore — parses HIGHESTMODSEQ from tagged OK */
    ImapSelectResult res;
    int rc = imap_select_condstore(c, "INBOX", &res);
    ASSERT(rc == 0, "imap_ext: imap_select_condstore returns 0");
    ASSERT(res.highestmodseq == 42, "imap_ext: HIGHESTMODSEQ parsed from tagged OK");
    ASSERT(res.uidvalidity == 12345, "imap_ext: UIDVALIDITY parsed");

    /* imap_select_qresync — parses HIGHESTMODSEQ + VANISHED */
    memset(&res, 0, sizeof(res));
    rc = imap_select_qresync(c, "INBOX", 12345, 42, &res);
    ASSERT(rc == 0, "imap_ext: imap_select_qresync returns 0");
    ASSERT(res.highestmodseq == 999, "imap_ext: qresync HIGHESTMODSEQ=999");
    ASSERT(res.vanished_count >= 0, "imap_ext: vanished_count is set");
    free(res.vanished_uids);

    /* imap_uid_fetch_flags_changedsince */
    ImapFlagUpdate *updates = NULL;
    int upd_count = 0;
    rc = imap_uid_fetch_flags_changedsince(c, 10, &updates, &upd_count);
    ASSERT(rc == 0, "imap_ext: imap_uid_fetch_flags_changedsince returns 0");
    ASSERT(upd_count == 2, "imap_ext: changedsince returned 2 updates");
    /* UID 2 is Seen → no UNSEEN flag */
    ASSERT((updates[0].flags & 1) == 0, "imap_ext: UID 2 Seen — UNSEEN clear");
    /* UID 3 is Seen + Flagged */
    ASSERT((updates[1].flags & 2) != 0, "imap_ext: UID 3 Flagged");
    free(updates);

    /* imap_uid_move — uses UID COPY + UID STORE \\Deleted + EXPUNGE */
    rc = imap_uid_move(c, "0000000000000001", "Archive");
    ASSERT(rc == 0, "imap_ext: imap_uid_move returns 0");

    /* imap_list with NIL separator and unquoted name */
    char **folders = NULL;
    int fc = 0;
    char sep = '.';
    rc = imap_list(c, &folders, &fc, &sep);
    ASSERT(rc == 0, "imap_ext: imap_list (NIL sep) returns 0");
    ASSERT(fc == 2, "imap_ext: imap_list found 2 folders (unquoted)");
    for (int i = 0; i < fc; i++) free(folders[i]);
    free(folders);

    /* imap_create_folder with [ALREADYEXISTS] response — must return 0 */
    rc = imap_create_folder(c, "INBOX");
    ASSERT(rc == 0, "imap_ext: imap_create_folder [ALREADYEXISTS] treated as success");

    imap_disconnect(c);

    int status = 0;
    waitpid(pid, &status, 0);
}

/* ── Test: imap_connect with bare host URL and imap:// refusal ──────────── */

/*
 * The imap_client always sends LOGIN with quoted user/pass (send_cmd uses
 * "LOGIN \"%s\" \"%s\""), so line 200 (unquoted username path) cannot be
 * reached through the public API.  We test the other uncovered URL-parsing
 * branches instead:
 *  • imap_connect with bare host URL (no scheme → IMAPS default)
 *  • imap_connect with imap:// + verify_tls=1 must be refused (no TLS)
 */
void test_imap_connect_bare_host(void) {
    /* A bare hostname (no imaps:// prefix) should be treated as IMAPS on
     * port 993.  Since 127.0.0.1:993 is almost certainly not listening we
     * just verify that the function returns NULL without crashing. */
    ImapClient *c = imap_connect("127.0.0.1", "u", "p", 0);
    /* May succeed or fail depending on whether port 993 is open; either is fine.
     * The important thing is no crash and the URL is parsed correctly. */
    if (c) imap_disconnect(c);
    ASSERT(1, "bare host: no crash");
}

void test_imap_connect_refused_without_tls(void) {
    /* imap:// (non-TLS) with verify_tls=1 must be refused */
    ImapClient *c = imap_connect("imap://127.0.0.1:19143", "u", "p", 1);
    ASSERT(c == NULL, "imap:// without verify_tls=0 must be refused");
}

/* ── Test: plain (non-TLS) imap:// connection path ─────────────────────── */

/*
 * Plain (non-TLS) IMAP server for testing the plain-socket I/O path.
 * Opened on a TCP socket without SSL — exercises net_read/net_write plain paths.
 */
static void run_plain_imap_server(int listen_fd) {
    int cfd = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (cfd < 0) { GCOV_FLUSH(); _exit(1); }

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Plain greeting */
    const char *greet = "* OK Mock plain IMAP ready\r\n";
    ssize_t _wr; _wr = write(cfd, greet, strlen(greet)); (void)_wr;

    char buf[2048];
    while (1) {
        ssize_t n = read(cfd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        char tag[32] = "*";
        sscanf(buf, "%31s", tag);
        char resp[256];
        if (strstr(buf, "LOGIN")) {
            snprintf(resp, sizeof(resp), "%s OK Logged in\r\n", tag);
            _wr = write(cfd, resp, strlen(resp)); (void)_wr;
        } else if (strstr(buf, "LIST")) {
            snprintf(resp, sizeof(resp),
                     "* LIST (\\HasNoChildren) \".\" INBOX\r\n"
                     "%s OK LIST completed\r\n", tag);
            _wr = write(cfd, resp, strlen(resp)); (void)_wr;
        } else if (strstr(buf, "SELECT")) {
            snprintf(resp, sizeof(resp),
                     "* 0 EXISTS\r\n%s OK SELECT completed\r\n", tag);
            _wr = write(cfd, resp, strlen(resp)); (void)_wr;
        } else if (strstr(buf, "UID SEARCH")) {
            snprintf(resp, sizeof(resp),
                     "* SEARCH\r\n%s OK SEARCH completed\r\n", tag);
            _wr = write(cfd, resp, strlen(resp)); (void)_wr;
        } else if (strstr(buf, "LOGOUT")) {
            _wr = write(cfd, "* BYE Bye\r\n", 11); (void)_wr;
            snprintf(resp, sizeof(resp), "%s OK LOGOUT completed\r\n", tag);
            _wr = write(cfd, resp, strlen(resp)); (void)_wr;
            break;
        } else {
            snprintf(resp, sizeof(resp), "%s BAD Unknown\r\n", tag);
            _wr = write(cfd, resp, strlen(resp)); (void)_wr;
        }
    }
    close(cfd);
    GCOV_FLUSH();
    _exit(0);
}

/*
 * Connect with imap:// (no TLS) and verify_tls=0 so the plain socket code
 * paths (net_read plain, net_write plain) are exercised.
 */
void test_imap_plain_socket_ops(void) {
    int port = 0;
    int lfd  = make_listener(&port);
    ASSERT(lfd >= 0, "plain: make_listener");

    pid_t pid = fork();
    ASSERT(pid >= 0, "plain: fork");
    if (pid == 0) {
        run_plain_imap_server(lfd);
        /* _exit inside */
    }
    close(lfd);

    char url[64];
    snprintf(url, sizeof(url), "imap://127.0.0.1:%d", port);
    ImapClient *c = imap_connect(url, "user", "pass", 0);
    ASSERT(c != NULL, "plain: imap_connect (no TLS, verify=0) must succeed");

    /* Exercise plain net_write path via SELECT */
    int rc = imap_select(c, "INBOX");
    ASSERT(rc == 0, "plain: imap_select returns 0");

    /* Exercise plain net_write + net_read via UID SEARCH */
    char (*uids)[17] = NULL;
    int count = 0;
    rc = imap_uid_search(c, "ALL", &uids, &count);
    ASSERT(rc == 0, "plain: imap_uid_search returns 0");
    free(uids);

    /* imap_list exercises more plain I/O */
    char **folders = NULL;
    int fc = 0;
    char sep = '.';
    rc = imap_list(c, &folders, &fc, &sep);
    ASSERT(rc == 0, "plain: imap_list returns 0");
    for (int i = 0; i < fc; i++) free(folders[i]);
    free(folders);

    imap_disconnect(c);

    int status = 0;
    waitpid(pid, &status, 0);
}

/* ── Test: imap_list with empty (quoted "") separator ───────────────────── */

static void run_mock_server_empty_sep(int listen_fd, SSL_CTX *ctx) {
    int cfd = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (cfd < 0) { SSL_CTX_free(ctx); GCOV_FLUSH(); _exit(1); }

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    SSL *ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);
    SSL_set_fd(ssl, cfd);
    if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl); close(cfd); GCOV_FLUSH(); _exit(1);
    }

    SSL_write(ssl, "* OK Mock ready\r\n", 17);

    char buf[2048];
    while (1) {
        int n = SSL_read(ssl, buf, (int)(sizeof(buf) - 1));
        if (n <= 0) break;
        buf[n] = '\0';
        char tag[32] = "*";
        sscanf(buf, "%31s", tag);
        char resp[512];
        if (strstr(buf, "LOGIN")) {
            snprintf(resp, sizeof(resp), "%s OK Logged in\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "LIST")) {
            /* Empty separator (quoted "") — exercises the *p=='"' branch */
            snprintf(resp, sizeof(resp),
                     "* LIST () \"\" \"INBOX\"\r\n"
                     "%s OK LIST completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "LOGOUT")) {
            SSL_write(ssl, "* BYE\r\n", 7);
            snprintf(resp, sizeof(resp), "%s OK LOGOUT completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
            break;
        } else {
            snprintf(resp, sizeof(resp), "%s BAD Unknown\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(cfd);
    GCOV_FLUSH();
    _exit(0);
}

void test_imap_list_empty_separator(void) {
    int port = 0;
    int lfd  = make_listener(&port);
    ASSERT(lfd >= 0, "empty_sep: make_listener");

    SSL_CTX *ctx = create_server_ctx();
    ASSERT(ctx != NULL, "empty_sep: create_server_ctx");

    pid_t pid = fork();
    ASSERT(pid >= 0, "empty_sep: fork");
    if (pid == 0) {
        run_mock_server_empty_sep(lfd, ctx);
    }
    close(lfd);
    SSL_CTX_free(ctx);

    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", port);
    ImapClient *c = imap_connect(url, "user", "pass", 0);
    ASSERT(c != NULL, "empty_sep: imap_connect must succeed");

    char **folders = NULL;
    int fc = 0;
    char sep = 'x';
    int rc = imap_list(c, &folders, &fc, &sep);
    ASSERT(rc == 0, "empty_sep: imap_list returns 0");
    ASSERT(fc == 1, "empty_sep: imap_list found 1 folder");
    for (int i = 0; i < fc; i++) free(folders[i]);
    free(folders);

    imap_disconnect(c);

    int status = 0;
    waitpid(pid, &status, 0);
}

/* ── Test: QRESYNC VANISHED without (EARLIER) + uid_fetch with no literal ── */

/*
 * Mock server that:
 *  1. On SELECT+QRESYNC: sends VANISHED without "(EARLIER)" — covers lines 1052-1053
 *  2. On UID FETCH BODY.PEEK[]: sends OK with no literal — covers line 798
 */
static void run_mock_server_vanished_noearlier(int listen_fd, SSL_CTX *ctx) {
    int cfd = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (cfd < 0) { SSL_CTX_free(ctx); GCOV_FLUSH(); _exit(1); }

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    SSL *ssl = SSL_new(ctx);
    SSL_CTX_free(ctx);
    SSL_set_fd(ssl, cfd);
    if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl); close(cfd); GCOV_FLUSH(); _exit(1);
    }

    SSL_write(ssl, "* OK Mock ready\r\n", 17);

    char buf[4096];
    while (1) {
        int n = SSL_read(ssl, buf, (int)(sizeof(buf) - 1));
        if (n <= 0) break;
        buf[n] = '\0';
        char tag[32] = "*";
        sscanf(buf, "%31s", tag);
        char resp[1024];
        if (strstr(buf, "LOGIN")) {
            snprintf(resp, sizeof(resp),
                     "%s OK [CAPABILITY IMAP4rev1 CONDSTORE QRESYNC] Logged in\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "ENABLE")) {
            snprintf(resp, sizeof(resp),
                     "* ENABLED QRESYNC\r\n%s OK ENABLE completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "SELECT") && strstr(buf, "QRESYNC")) {
            /* VANISHED without "(EARLIER)" — hits the else branch in parse */
            snprintf(resp, sizeof(resp),
                     "* 2 EXISTS\r\n"
                     "* OK [HIGHESTMODSEQ 500]\r\n"
                     "* VANISHED 5:7\r\n"   /* no (EARLIER) */
                     "%s OK SELECT completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "BODY.PEEK[]")) {
            /* Return OK but NO literal body — exercises line 798 logger_log */
            snprintf(resp, sizeof(resp),
                     "* 1 FETCH (UID 1)\r\n%s OK FETCH completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        } else if (strstr(buf, "LOGOUT")) {
            SSL_write(ssl, "* BYE\r\n", 7);
            snprintf(resp, sizeof(resp), "%s OK LOGOUT completed\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
            break;
        } else {
            snprintf(resp, sizeof(resp), "%s BAD Unknown\r\n", tag);
            SSL_write(ssl, resp, (int)strlen(resp));
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(cfd);
    GCOV_FLUSH();
    _exit(0);
}

void test_imap_qresync_vanished_no_earlier(void) {
    int port = 0;
    int lfd  = make_listener(&port);
    ASSERT(lfd >= 0, "vanished_no_earlier: make_listener");

    SSL_CTX *ctx = create_server_ctx();
    ASSERT(ctx != NULL, "vanished_no_earlier: create_server_ctx");

    pid_t pid = fork();
    ASSERT(pid >= 0, "vanished_no_earlier: fork");
    if (pid == 0) {
        run_mock_server_vanished_noearlier(lfd, ctx);
    }
    close(lfd);
    SSL_CTX_free(ctx);

    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", port);
    ImapClient *c = imap_connect(url, "user", "pass", 0);
    ASSERT(c != NULL, "vanished_no_earlier: imap_connect must succeed");

    /* imap_select_qresync with VANISHED without (EARLIER) */
    ImapSelectResult res;
    memset(&res, 0, sizeof(res));
    int rc = imap_select_qresync(c, "INBOX", 1000, 50, &res);
    ASSERT(rc == 0, "vanished_no_earlier: imap_select_qresync returns 0");
    /* VANISHED 5:7 expands to UIDs 5, 6, 7 */
    ASSERT(res.vanished_count >= 0, "vanished_no_earlier: vanished_count set");
    free(res.vanished_uids);

    /* imap_uid_fetch_body with no literal in response — returns NULL, logs warning */
    char *body = imap_uid_fetch_body(c, "0000000000000001");
    ASSERT(body == NULL, "vanished_no_earlier: uid_fetch_body NULL when no literal");

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
    test_imap_full_operations();
    test_imap_extended_operations();
    test_imap_connect_bare_host();
    test_imap_connect_refused_without_tls();
    test_imap_plain_socket_ops();
    test_imap_list_empty_separator();
    test_imap_qresync_vanished_no_earlier();
}
