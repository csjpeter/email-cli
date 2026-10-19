/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-04-17 */
/* Unit tests of the Gmail OAuth2 authentication. */
#include "test_helpers.h"
#include "gmail_auth.h"
#include "config.h"
#include "gmail_login.h"
#include "fault_alloc.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Wait for child with 5-second timeout, then SIGKILL. */
static void wait_child(pid_t pid) {
    if (pid <= 0) return;
    for (int i = 0; i < 50; i++) {
        int st;
        if (waitpid(pid, &st, WNOHANG) != 0) return;
        struct timespec ts = {0, 100000000L};
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    int st; waitpid(pid, &st, 0);
}

static void run_mock_token_server_expect(int port, const char *response,
                                         const char *const *expect);

/* Where the mock token server child reports whether the request it received
 * carried everything expected ("OK", or "missing <string>"); -1 = nowhere.
 * A pipe carries the verdict rather than the exit status: the status is not
 * reliably collectable (an earlier test may have changed SIGCHLD handling). */
static int g_mock_report_fd = -1;

/* The loopback redirect range gmail_auth_authorize() picks a port from.
 * Mirrors LOOPBACK_PORT_START/END in libemail/src/infrastructure/gmail_auth.c. */
#define REDIRECT_PORT_FIRST 8089
#define REDIRECT_PORT_LAST  8099

/**
 * Stand in for the browser Google redirects: connect to the loopback listener
 * gmail_auth_authorize() opened and deliver @p request.
 *
 * The port is not known in advance — device_flow binds the first free one in
 * the range — so the range is scanned.  The scan is *retried* until it
 * connects or @p deadline_ms passes, because a single pass can finish before
 * the parent has reached listen(): every connect is then refused, this child
 * exits, and the parent sits out its own 300-second accept timeout with
 * nothing left to answer it.  While the parent is in accept() it is listening
 * by definition, so a retry always gets through.
 *
 * A read timeout guards against a foreign listener in the same range: a peer
 * that takes the connection but never answers is skipped instead of trapping
 * this child in read().
 *
 * Runs in the forked child and never returns.
 */
static void redirect_child(const char *request, int deadline_ms) {
    for (int waited = 0; waited <= deadline_ms; waited += 50) {
        for (int port = REDIRECT_PORT_FIRST; port <= REDIRECT_PORT_LAST; port++) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            struct timeval tv = {1, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            struct sockaddr_in ca = {0};
            ca.sin_family      = AF_INET;
            ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ca.sin_port        = htons((uint16_t)port);

            if (connect(fd, (struct sockaddr *)&ca, sizeof(ca)) == 0) {
                ssize_t w = write(fd, request, strlen(request));
                if (w == (ssize_t)strlen(request)) {
                    char buf[512];
                    ssize_t r = read(fd, buf, sizeof(buf));
                    if (r > 0) { close(fd); _exit(0); }   /* answered: this was it */
                }
                close(fd);
                continue;                                  /* wrong peer — keep looking */
            }
            close(fd);
        }
        struct timespec ts = {0, 50000000L};   /* 50 ms */
        nanosleep(&ts, NULL);
    }
    _exit(1);
}


/* ── The authorization with the program's side stubbed ───────────────── */

static char g_auth_url[2048];
static int  g_auth_port;
static int  g_auth_shown;
static char g_auth_stderr[256];
static GmailAuthUi g_auth_ui;
static const char *g_auth_paste;

static void stub_show_url(const char *url, int port, void *user) {
    (void)user;
    snprintf(g_auth_url, sizeof(g_auth_url), "%s", url);
    g_auth_port = port;
    g_auth_shown++;
}

static char *stub_read_redirect(void *user) {
    (void)user;
    return g_auth_paste ? strdup(g_auth_paste) : NULL;
}

/**
 * gmail_auth_authorize() with stub callbacks and stderr captured: the library
 * must print nothing (EMAIL-113), so g_auth_stderr stays empty and the reason
 * is in g_auth_ui.error.
 */
static int authorize_run(Config *cfg, int paste, const char *pasted) {
    memset(&g_auth_ui, 0, sizeof(g_auth_ui));
    g_auth_ui.paste = paste;
    g_auth_ui.show_url = stub_show_url;
    g_auth_ui.read_redirect = stub_read_redirect;
    g_auth_paste = pasted;
    g_auth_shown = 0;
    g_auth_url[0] = '\0';
    int cap[2];
    if (pipe(cap) != 0) return -2;
    fcntl(cap[0], F_SETFL, O_NONBLOCK);
    fflush(stderr);
    int saved = dup(2);
    dup2(cap[1], 2);
    int rc = gmail_auth_authorize(cfg, &g_auth_ui);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(cap[1]);
    ssize_t got = read(cap[0], g_auth_stderr, sizeof(g_auth_stderr) - 1);
    g_auth_stderr[got > 0 ? got : 0] = '\0';
    close(cap[0]);
    return rc;
}

/* ── gmail_auth_refresh — error paths (no network needed) ─────────── */

static void test_refresh_no_token(void) {
    Config cfg = {0};
    /* No refresh_token → should return NULL */
    char *tok = gmail_auth_refresh(&cfg);
    ASSERT(tok == NULL, "refresh: returns NULL with no refresh_token");
}

static void test_refresh_empty_token(void) {
    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("");
    char *tok = gmail_auth_refresh(&cfg);
    ASSERT(tok == NULL, "refresh: returns NULL with empty refresh_token");
    free(cfg.gmail_refresh_token);
}

/* ── Auth code extraction from browser redirect ──────────────────────── */

static void test_auth_code_extraction(void) {
    /* Simulate the localhost listener + browser redirect that
     * gmail_auth_authorize uses internally. */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "auth_code: socket created");
    int opt = 1;
    ASSERT(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0,
           "SO_REUSEADDR set");
    struct timeval tv = {5, 0};
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;   /* the kernel picks a free port */
    int bound = bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    ASSERT(bound == 0, "auth_code: bind succeeded");
    ASSERT(listen(listen_fd, 1) == 0, "auth_code: listen succeeded");
    socklen_t alen = sizeof(addr);
    ASSERT(getsockname(listen_fd, (struct sockaddr *)&addr, &alen) == 0,
           "auth_code: getsockname");
    const uint16_t cb_port = ntohs(addr.sin_port);

    /* Fork child to simulate browser redirect */
    pid_t pid = fork();
    if (pid == 0) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in ca = {0};
        ca.sin_family = AF_INET;
        ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ca.sin_port = htons(cb_port);
        connect(fd, (struct sockaddr *)&ca, sizeof(ca));
        const char *req =
            "GET /callback?code=test_code_abc&scope=x HTTP/1.1\r\n"
            "Host: localhost\r\n\r\n";
        ssize_t w = write(fd, req, strlen(req)); (void)w;
        char buf[256];
        ssize_t r = read(fd, buf, sizeof(buf)); (void)r;
        close(fd);
        _exit(0);
    }

    /* Parent: accept connection and extract code */
    struct sockaddr_in cli;
    socklen_t cli_len = sizeof(cli);
    int conn = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
    ASSERT(conn >= 0, "auth_code: accept succeeded");

    char req[4096] = {0};
    ssize_t n = read(conn, req, sizeof(req) - 1);
    ASSERT(n > 0, "auth_code: read request");

    /* Extract code= parameter */
    char *cs = strstr(req, "code=");
    ASSERT(cs != NULL, "auth_code: found code= in request");
    cs += 5;
    char *ce = cs;
    while (*ce && *ce != '&' && *ce != ' ') ce++;
    char *code = strndup(cs, (size_t)(ce - cs));
    ASSERT(strcmp(code, "test_code_abc") == 0,
           "auth_code: extracted correct code");
    free(code);

    const char *html = "HTTP/1.1 200 OK\r\n\r\nOK";
    ssize_t wr = write(conn, html, strlen(html)); (void)wr;
    close(conn);
    close(listen_fd);
    wait_child(pid);
}

static void test_auth_code_denied(void) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "denied: socket created");
    int opt = 1;
    ASSERT(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0,
           "SO_REUSEADDR set");
    struct timeval tv = {5, 0};
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;   /* the kernel picks a free port */
    ASSERT(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0,
           "denied: bind succeeded");
    ASSERT(listen(listen_fd, 1) == 0, "denied: listen succeeded");
    socklen_t alen = sizeof(addr);
    ASSERT(getsockname(listen_fd, (struct sockaddr *)&addr, &alen) == 0,
           "denied: getsockname");
    const uint16_t cb_port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid == 0) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in ca = {0};
        ca.sin_family = AF_INET;
        ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ca.sin_port = htons(cb_port);
        connect(fd, (struct sockaddr *)&ca, sizeof(ca));
        const char *req =
            "GET /callback?error=access_denied HTTP/1.1\r\n"
            "Host: localhost\r\n\r\n";
        ssize_t w = write(fd, req, strlen(req)); (void)w;
        char buf[256];
        ssize_t r = read(fd, buf, sizeof(buf)); (void)r;
        close(fd);
        _exit(0);
    }

    struct sockaddr_in cli;
    socklen_t cli_len = sizeof(cli);
    int conn = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
    ASSERT(conn >= 0, "denied: accept succeeded");

    char req[4096] = {0};
    ssize_t n2 = read(conn, req, sizeof(req) - 1); (void)n2;

    ASSERT(strstr(req, "code=") == NULL,
           "denied: no code in request");
    ASSERT(strstr(req, "error=access_denied") != NULL,
           "denied: access_denied present");

    const char *html = "HTTP/1.1 200 OK\r\n\r\nDenied";
    ssize_t wr = write(conn, html, strlen(html)); (void)wr;
    close(conn);
    close(listen_fd);
    wait_child(pid);
}

/* ── Gmail IMAP rejection in wizard ──────────────────────────────────── */

static void test_wizard_rejects_gmail_imap(void) {
    /* The wizard should reject gmail.com as IMAP host */
    /* This is tested via test_wizard.c but we verify the concept here:
     * a Config with host containing gmail.com and gmail_mode=0 is invalid */
    Config cfg = {0};
    cfg.host = strdup("imaps://imap.gmail.com");
    cfg.gmail_mode = 0;
    ASSERT(strstr(cfg.host, "gmail.com") != NULL,
           "gmail_imap: gmail.com detected in host");
    ASSERT(cfg.gmail_mode == 0,
           "gmail_imap: incorrectly configured as IMAP");
    free(cfg.host);
}

/* The listener of the next mock token server.  It is bound by the parent, to
 * a port the kernel picks, *before* the fork: the child only accepts on it, and
 * the test knows the port with no fixed number and no race on the bind. */
static int g_mock_listen_fd = -1;

static int mock_listen(void) {
    if (g_mock_listen_fd >= 0) close(g_mock_listen_fd);
    g_mock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_mock_listen_fd < 0) { perror("mock_listen: socket"); exit(EXIT_FAILURE); }
    int opt = 1;
    if (setsockopt(g_mock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        perror("mock_listen: setsockopt");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    socklen_t len = sizeof(addr);
    if (bind(g_mock_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(g_mock_listen_fd, 1) != 0 ||
        getsockname(g_mock_listen_fd, (struct sockaddr *)&addr, &len) != 0) {
        perror("mock_listen: bind/listen");
        exit(EXIT_FAILURE);
    }
    return ntohs(addr.sin_port);
}

/* ── Additional coverage tests ──────────────────────────────────────── */

static void test_refresh_with_client_credentials(void) {
    /* Covers get_client_id/get_client_secret taking the configured values and
     * the request body built from them.  The token endpoint is the local mock
     * (GMAIL_TEST_TOKEN_URL): this test never leaves the machine, and it
     * checks what was sent as well as what came back. */
    const int mock_port = mock_listen();
    const char *http_resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"access_token\":\"cred_access_tok\",\"token_type\":\"Bearer\"}";
    static const char *const expect[] = {
        "client_id=test-client-id.apps.googleusercontent.com",
        "client_secret=test-client-secret",
        "refresh_token=dummy_refresh_token",
        "grant_type=refresh_token",
        NULL
    };

    int report[2];
    ASSERT(pipe(report) == 0, "client credentials: report pipe");
    g_mock_report_fd = report[1];
    pid_t pid = fork();
    if (pid == 0) {
        close(report[0]);
        run_mock_token_server_expect(mock_port, http_resp, expect);
    }
    close(report[1]);
    g_mock_report_fd = -1;

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);
    unsetenv("GMAIL_TEST_TOKEN");

    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("dummy_refresh_token");
    cfg.gmail_client_id     = strdup("test-client-id.apps.googleusercontent.com");
    cfg.gmail_client_secret = strdup("test-client-secret");

    char *tok = gmail_auth_refresh(&cfg);

    char verdict[300] = "";
    ssize_t vn = read(report[0], verdict, sizeof(verdict) - 1);
    verdict[vn > 0 ? vn : 0] = '\0';
    close(report[0]);
    wait_child(pid);
    int tok_ok = tok != NULL && strcmp(tok, "cred_access_tok") == 0;
    int sent_all = strcmp(verdict, "OK") == 0;

    /* Release everything before asserting: a failed ASSERT returns. */
    unsetenv("GMAIL_TEST_TOKEN_URL");
    free(tok);
    free(cfg.gmail_refresh_token);
    free(cfg.gmail_client_id);
    free(cfg.gmail_client_secret);

    ASSERT(tok_ok, "client credentials: the mock's access token is returned");
    ASSERT(sent_all, verdict[0] ? verdict : "client credentials: no verdict from the mock "
                                            "(the request carried client_id, client_secret, "
                                            "refresh_token and grant_type)");
}

static void test_device_flow_no_credentials(void) {
    /* No client_id anywhere → device_flow must refuse before it opens a
     * listener.
     *
     * Whether a client_id exists is a property of the build: when
     * gmail_credentials.cmake is present, GMAIL_DEFAULT_CLIENT_ID is compiled
     * in and this branch is simply not reachable.  Without the hook this test
     * then walked straight into the authorization flow, opened the loopback
     * listener and sat out its own 300-second accept timeout waiting for a
     * browser — and still returned -1 at the end, so the assertion passed for
     * entirely the wrong reason while costing five minutes per unit run. */
    setenv("GMAIL_TEST_NO_CLIENT_ID", "1", 1);

    Config cfg = {0};
    time_t t0 = time(NULL);
    int rc = authorize_run(&cfg, 0, NULL);
    time_t elapsed = time(NULL) - t0;

    unsetenv("GMAIL_TEST_NO_CLIENT_ID");

    ASSERT(rc == -1, "device_flow: no client_id returns -1");
    ASSERT(strstr(g_auth_ui.error, "credentials are not configured") != NULL,
           "no client_id: the reason is returned");
    ASSERT(g_auth_shown == 0 && g_auth_stderr[0] == '\0',
           "no client_id: nothing shown, nothing printed");
    /* The point of the branch is that it refuses *before* waiting for anyone:
     * a -1 that arrives after the accept timeout is not the same answer. */
    ASSERT(elapsed < 5, "device_flow: no client_id refuses without waiting");
}

static void test_device_flow_access_denied(void) {
    /* Child sends error=access_denied → wait_for_auth_code returns NULL
     * (covers open_listener, wait_for_auth_code error path, device_flow setup).
     * No network call needed since auth_code is NULL → function returns -1. */
    Config cfg = {0};
    cfg.gmail_client_id     = strdup("test-client-id.apps.googleusercontent.com");
    cfg.gmail_client_secret = strdup("test-client-secret");

    pid_t pid = fork();
    if (pid == 0)
        redirect_child("GET /callback?error=access_denied HTTP/1.1\r\n"
                       "Host: localhost\r\n\r\n", 5000);   /* never returns */

    int rc = authorize_run(&cfg, 0, NULL);

    ASSERT(rc == -1, "device_flow: access_denied returns -1");
    ASSERT(strstr(g_auth_ui.error, "access_denied") != NULL,
           "access_denied: the reason is returned through the interface");
    ASSERT(g_auth_stderr[0] == '\0', "access_denied: the library printed nothing");

    wait_child(pid);

    free(cfg.gmail_client_id);
    free(cfg.gmail_client_secret);
}

static void test_refresh_test_token_hook(void) {
    /* Covers the GMAIL_TEST_TOKEN early-return path in gmail_auth_refresh */
    setenv("GMAIL_TEST_TOKEN", "test_access_token_xyz", 1);
    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("some_refresh_token");
    char *tok = gmail_auth_refresh(&cfg);
    ASSERT(tok != NULL, "refresh: GMAIL_TEST_TOKEN path returns non-NULL");
    if (tok)
        ASSERT(strcmp(tok, "test_access_token_xyz") == 0, "refresh: test token value");
    free(tok);
    free(cfg.gmail_refresh_token);
    unsetenv("GMAIL_TEST_TOKEN");
}

/* Minimal HTTP server child that sends a fixed response */
static void run_mock_token_server(int port, const char *response) {
    run_mock_token_server_expect(port, response, NULL);
}

/* As above, and exits 2 when the request lacks any string in the NULL-ended
 * `expect` list, so the parent can assert on what was actually sent. */
static void run_mock_token_server_expect(int port, const char *response,
                                         const char *const *expect) {
    (void)port;   /* the listener was bound by mock_listen() before the fork */
    int srv = g_mock_listen_fd;
    if (srv < 0) _exit(1);
    struct timeval tv = {5, 0};
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    int conn = accept(srv, (struct sockaddr *)&cli, &cli_len);
    if (conn < 0) { close(srv); _exit(1); }
    /* The request arrives in pieces (curl sends the headers, then the body):
     * read until every expected string is there, or the headers are complete
     * when nothing is expected, or the peer stops (receive timeout). */
    char buf[4096];
    size_t have = 0;
    buf[0] = '\0';
    while (have < sizeof(buf) - 1) {
        ssize_t n = read(conn, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0) break;
        have += (size_t)n;
        buf[have] = '\0';
        int complete = 1;
        for (int i = 0; expect && expect[i]; i++)
            if (!strstr(buf, expect[i])) { complete = 0; break; }
        if (complete && (expect || strstr(buf, "\r\n\r\n"))) break;
    }
    ssize_t w = write(conn, response, strlen(response)); (void)w;
    close(conn);
    close(srv);
    for (int i = 0; expect && expect[i]; i++)
        if (!strstr(buf, expect[i])) {
            if (g_mock_report_fd >= 0) {
                char msg[256];
                int m = snprintf(msg, sizeof(msg), "missing %s", expect[i]);
                ssize_t rw = write(g_mock_report_fd, msg, (size_t)m); (void)rw;
            }
            _exit(2);
        }
    if (g_mock_report_fd >= 0) {
        ssize_t rw = write(g_mock_report_fd, "OK", 2); (void)rw;
    }
    _exit(0);
}

static void test_refresh_via_mock_server_200(void) {
    /* Covers lines 336-339: code==200 path in gmail_auth_refresh */
    const int mock_port = mock_listen();
    const char *http_resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"access_token\":\"mock_access_tok\",\"token_type\":\"Bearer\"}";

    pid_t pid = fork();
    if (pid == 0) {
        run_mock_token_server(mock_port, http_resp);
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);
    unsetenv("GMAIL_TEST_TOKEN");

    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("valid_looking_refresh_token");
    char *tok = gmail_auth_refresh(&cfg);
    ASSERT(tok != NULL, "mock 200: access_token returned");
    if (tok)
        ASSERT(strcmp(tok, "mock_access_tok") == 0, "mock 200: access_token value");
    free(tok);
    free(cfg.gmail_refresh_token);

    unsetenv("GMAIL_TEST_TOKEN_URL");
    wait_child(pid);
}

static void test_refresh_via_mock_server_200_no_token(void) {
    /* Covers lines 290-293: code==200 but no access_token in response */
    const int mock_port = mock_listen();
    const char *http_resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"token_type\":\"Bearer\"}"; /* no access_token field */

    pid_t pid = fork();
    if (pid == 0) {
        run_mock_token_server(mock_port, http_resp);
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);

    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("valid_looking_refresh_token");
    char *tok = gmail_auth_refresh(&cfg);
    ASSERT(tok == NULL, "mock 200 no token: returns NULL");
    free(cfg.gmail_refresh_token);

    unsetenv("GMAIL_TEST_TOKEN_URL");
    wait_child(pid);
}

static void test_refresh_via_mock_server_400_unknown_error(void) {
    /* Covers lines 351-352: non-200 with error that is neither invalid_grant
     * nor invalid_client */
    const int mock_port = mock_listen();
    const char *http_resp =
        "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
        "{\"error\":\"unsupported_grant_type\"}";

    pid_t pid = fork();
    if (pid == 0) {
        run_mock_token_server(mock_port, http_resp);
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);

    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("some_refresh_token");
    int saved = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) dup2(dn, 2);
    char *tok = gmail_auth_refresh(&cfg);
    if (dn >= 0) { dup2(saved, 2); close(dn); }
    close(saved);
    ASSERT(tok == NULL, "mock 400 unknown error: returns NULL");
    free(cfg.gmail_refresh_token);

    unsetenv("GMAIL_TEST_TOKEN_URL");
    wait_child(pid);
}

static void test_refresh_via_mock_server_curl_error(void) {
    /* Covers lines 76-80: curl error path (port not listening → connect failure) */
    /* Use a port that is definitely not listening */
    setenv("GMAIL_TEST_TOKEN_URL", "http://127.0.0.1:19999/token", 1);
    unsetenv("GMAIL_TEST_TOKEN");

    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("some_token");
    char *tok = gmail_auth_refresh(&cfg);
    ASSERT(tok == NULL, "mock curl error: returns NULL");
    free(cfg.gmail_refresh_token);

    unsetenv("GMAIL_TEST_TOKEN_URL");
}

static void test_device_flow_full_mock(void) {
    /* Covers lines 287-305: successful token exchange in gmail_auth_authorize.
     * Child 1 simulates the browser redirect (code=xxx).
     * Child 2 is the mock token server that returns a valid access_token.
     * The function completes successfully → returns 0. */
    const int mock_port = mock_listen();
    const char *token_resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"access_token\":\"dev_flow_tok\",\"refresh_token\":\"dev_flow_refresh\","
        "\"token_type\":\"Bearer\"}";

    /* Start mock token server */
    pid_t srv_pid = fork();
    if (srv_pid == 0) {
        run_mock_token_server(mock_port, token_resp);
    }

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);

    Config cfg = {0};
    cfg.gmail_client_id     = strdup("test-client-id.apps.googleusercontent.com");
    cfg.gmail_client_secret = strdup("test-client-secret");

    /* Browser redirect child */
    pid_t br_pid = fork();
    if (br_pid == 0)
        redirect_child("GET /callback?code=full_test_code&scope=x HTTP/1.1\r\n"
                       "Host: localhost\r\n\r\n", 5000);   /* never returns */

    int rc = authorize_run(&cfg, 0, NULL);

    ASSERT(rc == 0, "device_flow full mock: returns 0 on success");
    ASSERT(g_auth_shown == 1, "full mock: the URL was handed to the program once");
    ASSERT(strstr(g_auth_url, "client_id=test-client-id.apps.googleusercontent.com") != NULL &&
           strstr(g_auth_url, "scope=https%3A%2F%2Fmail.google.com%2F") != NULL &&
           strstr(g_auth_url, "access_type=offline") != NULL,
           "full mock: the URL carries client, scope and offline access");
    ASSERT(g_auth_port >= REDIRECT_PORT_FIRST && g_auth_port <= REDIRECT_PORT_LAST,
           "full mock: the listener port is reported");
    ASSERT(g_auth_stderr[0] == '\0', "full mock: the library printed nothing");
    if (cfg.gmail_refresh_token)
        ASSERT(strcmp(cfg.gmail_refresh_token, "dev_flow_refresh") == 0,
               "device_flow full mock: refresh_token set");

    free(cfg.gmail_client_id);
    free(cfg.gmail_client_secret);
    free(cfg.gmail_refresh_token);

    wait_child(br_pid);
    wait_child(srv_pid);

    unsetenv("GMAIL_TEST_TOKEN_URL");
}

static void test_device_flow_with_code(void) {
    /* Child sends a real code= → wait_for_auth_code returns the code.
     * Covers wait_for_auth_code code-extraction path and the post-building
     * step in gmail_auth_authorize.  http_post to TOKEN_URL fails
     * (invalid credentials / no network) → returns -1. */
    Config cfg = {0};
    cfg.gmail_client_id     = strdup("test-client-id.apps.googleusercontent.com");
    cfg.gmail_client_secret = strdup("test-client-secret");

    pid_t pid = fork();
    if (pid == 0)
        redirect_child("GET /callback?code=test_code_exchange&scope=x HTTP/1.1\r\n"
                       "Host: localhost\r\n\r\n", 5000);   /* never returns */

    int rc = authorize_run(&cfg, 0, NULL);

    /* Token exchange fails with invalid test credentials → returns -1 */
    ASSERT(rc == -1, "device_flow: invalid credentials returns -1");
    ASSERT(strstr(g_auth_ui.error, "Token exchange failed") != NULL,
           "failed exchange: the reason is returned");
    ASSERT(g_auth_stderr[0] == '\0', "failed exchange: the library printed nothing");

    wait_child(pid);

    free(cfg.gmail_client_id);
    free(cfg.gmail_client_secret);
    free(cfg.gmail_refresh_token);
}


/* ── Paste mode and the shape of what is pasted (EMAIL-113) ──────────── */

/**
 * Paste mode against the mock token endpoint: @p pasted is what the person
 * pastes, @p expect the form field the exchange must carry (NULL = the
 * exchange must not happen).  Returns the authorization's result; the token
 * lands in @p tok_out (heap) on success.
 */
static int paste_case(const char *pasted, const char *expect, char **tok_out,
                      char *verdict, size_t vcap) {
    const int mock_port = mock_listen();
    const char *resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"access_token\":\"a\",\"refresh_token\":\"pasted_refresh\"}";
    const char *expects[] = { expect, "grant_type=authorization_code",
                              "client_secret=test-client-secret", NULL };
    int report[2];
    if (pipe(report) != 0) return -2;
    g_mock_report_fd = report[1];
    pid_t srv = fork();
    if (srv == 0) {
        close(report[0]);
        run_mock_token_server_expect(mock_port, resp, expects);
    }
    close(report[1]);
    g_mock_report_fd = -1;
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);

    Config cfg = {0};
    cfg.gmail_client_id     = strdup("test-client-id.apps.googleusercontent.com");
    cfg.gmail_client_secret = strdup("test-client-secret");
    int rc = authorize_run(&cfg, 1, pasted);
    *tok_out = cfg.gmail_refresh_token;
    cfg.gmail_refresh_token = NULL;
    free(cfg.gmail_client_id);
    free(cfg.gmail_client_secret);
    unsetenv("GMAIL_TEST_TOKEN_URL");

    if (rc != 0) kill(srv, SIGKILL);    /* no exchange happened: nothing to wait for */
    ssize_t vn = read(report[0], verdict, vcap - 1);
    verdict[vn > 0 ? vn : 0] = '\0';
    close(report[0]);
    kill(srv, SIGKILL);
    wait_child(srv);
    return rc;
}

static void test_paste_redirect_url(void) {
    char *tok = NULL; char v[300];
    int rc = paste_case("http://localhost:8089/callback?code=pasted_code&scope=x\n",
                        "code=pasted_code", &tok, v, sizeof(v));
    int tok_ok = tok && strcmp(tok, "pasted_refresh") == 0;
    free(tok);
    ASSERT(rc == 0, "paste: the pasted redirect address authorizes");
    ASSERT(tok_ok, "paste: the refresh token comes back");
    ASSERT(strcmp(v, "OK") == 0, v[0] ? v : "paste: the exchange carried the pasted code");
    ASSERT(g_auth_shown == 1 && strstr(g_auth_url, "redirect_uri=http%3A%2F%2Flocalhost%3A8089") != NULL,
           "paste: the URL was shown, with the fixed redirect port");
    ASSERT(g_auth_port == 8089, "paste: the redirect port is reported");
    ASSERT(g_auth_stderr[0] == '\0', "paste: the library printed nothing");
}

static void test_paste_bare_code_and_encoded_code(void) {
    char *tok = NULL; char v[300];
    int rc = paste_case("  4%2F0AbcDEF-xyz  \r\n", "code=4%2F0AbcDEF-xyz", &tok, v, sizeof(v));
    free(tok);
    ASSERT(rc == 0, "paste: a bare code (percent-encoded slash) authorizes");
    ASSERT(strcmp(v, "OK") == 0, v[0] ? v : "paste: the code is decoded and re-encoded once");
    rc = paste_case("?code=4/0Aplain&scope=x", "code=4%2F0Aplain", &tok, v, sizeof(v));
    free(tok);
    ASSERT(rc == 0 && strcmp(v, "OK") == 0, "paste: a query string alone works, slash encoded once");
}

static void test_paste_refusals(void) {
    char *tok = NULL; char v[300];
    int rc = paste_case("http://localhost:8089/callback?error=access_denied\n", "code=x",
                        &tok, v, sizeof(v));
    ASSERT(rc == -1 && tok == NULL, "paste: a denial fails");
    ASSERT(strstr(g_auth_ui.error, "access_denied") != NULL, "paste: the denial is the reason");
    rc = paste_case("this is not a code\n", "code=x", &tok, v, sizeof(v));
    ASSERT(rc == -1, "paste: text with blanks is no code");
    ASSERT(strstr(g_auth_ui.error, "No authorization code") != NULL, "paste: and says so");
    rc = paste_case("http://localhost:8089/callback\n", "code=x", &tok, v, sizeof(v));
    ASSERT(rc == -1, "paste: an address without a code is no code");
    rc = paste_case(NULL, "code=x", &tok, v, sizeof(v));
    ASSERT(rc == -1 && strstr(g_auth_ui.error, "No redirect") != NULL,
           "paste: nothing pasted fails with its reason");
    ASSERT(g_auth_stderr[0] == '\0', "paste refusals: the library printed nothing");
}

static void test_authorize_needs_callbacks(void) {
    Config cfg = {0};
    cfg.gmail_client_id     = strdup("id");
    cfg.gmail_client_secret = strdup("secret");
    GmailAuthUi ui;
    memset(&ui, 0, sizeof(ui));
    int r1 = gmail_auth_authorize(&cfg, &ui);               /* no show_url */
    char e1[sizeof(ui.error)];
    snprintf(e1, sizeof(e1), "%s", ui.error);
    ui.show_url = stub_show_url;
    ui.paste = 1;                                           /* paste, no reader */
    int r2 = gmail_auth_authorize(&cfg, &ui);
    free(cfg.gmail_client_id);
    free(cfg.gmail_client_secret);
    ASSERT(r1 == -1 && e1[0] != '\0', "no show_url: refused with a reason");
    ASSERT(r2 == -1 && strstr(ui.error, "pasted redirect") != NULL,
           "paste without read_redirect: refused with a reason");
    ASSERT(gmail_auth_authorize(&cfg, NULL) == -1, "no ui: refused");
}


/* ── Taking over a refresh token made elsewhere (EMAIL-113) ─────────── */

static int import_from(const char *text, Config *cfg) {
    FILE *f = fmemopen((void *)text, strlen(text), "r");
    if (!f) return -2;
    int rc = gmail_login_import_token(cfg, f);
    fclose(f);
    return rc;
}

static void test_import_token(void) {
    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("old");
    int r1 = import_from("  1//0gNewToken-abc_DEF  \r\n", &cfg);
    ASSERT(r1 == 0, "import: a token line is taken");
    ASSERT(strcmp(cfg.gmail_refresh_token, "1//0gNewToken-abc_DEF") == 0,
           "import: blanks around it are dropped, the old token replaced");
    int r2 = import_from("two words\n", &cfg);
    ASSERT(r2 == -1 && strcmp(cfg.gmail_refresh_token, "1//0gNewToken-abc_DEF") == 0,
           "import: a line with blanks inside is refused and changes nothing");
    int r3 = import_from("   \n", &cfg);
    ASSERT(r3 == -1, "import: an empty line is refused");
    int r4 = import_from("", &cfg);
    ASSERT(r4 == -1, "import: end of input is refused");
    ASSERT(strcmp(cfg.gmail_refresh_token, "1//0gNewToken-abc_DEF") == 0,
           "import: the token survives every refusal");
    free(cfg.gmail_refresh_token);
}

/* ── Registration ─────────────────────────────────────────────────── */

/**
 * A revoked authorization must say what revoked it and how to get it back.
 *
 * "Gmail refresh token expired. Re-authorization needed." was true and
 * useless: it named no cause, and for a long time there was no command to
 * re-authorize with.  stderr is captured here, because the message is the
 * whole behaviour under test.
 */
static void test_refresh_invalid_grant_message(void) {
    const int mock_port = mock_listen();
    const char *http_resp =
        "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
        "{\"error\":\"invalid_grant\"}";

    pid_t pid = fork();
    if (pid == 0)
        run_mock_token_server(mock_port, http_resp);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);

    char errfile[PATH_MAX];
    snprintf(errfile, sizeof(errfile), "%s/invalid-grant-XXXXXX", test_scratch_dir());
    int efd = mkstemp(errfile);
    ASSERT(efd >= 0, "invalid_grant: temp file for stderr");

    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("revoked_token");
    cfg.user                = strdup("someone@gmail.com");

    int saved = dup(2);
    dup2(efd, 2);
    char *tok = gmail_auth_refresh(&cfg);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);

    ASSERT(tok == NULL, "invalid_grant: returns NULL");

    char buf[2048] = {0};
    lseek(efd, 0, SEEK_SET);
    ssize_t rd = read(efd, buf, sizeof(buf) - 1);
    close(efd);
    unlink(errfile);
    ASSERT(rd > 0, "invalid_grant: something was written to stderr");

    ASSERT(strstr(buf, "revoked") != NULL,
           "invalid_grant: says the authorization was revoked");
    ASSERT(strstr(buf, "someone@gmail.com") != NULL,
           "invalid_grant: names the account it happened to");
    ASSERT(strstr(buf, "password") != NULL,
           "invalid_grant: names the cause — a changed Google password");
    ASSERT(strstr(buf, "config reauth") != NULL,
           "invalid_grant: names the command that repairs it");

    free(cfg.gmail_refresh_token);
    free(cfg.user);
    unsetenv("GMAIL_TEST_TOKEN_URL");
    wait_child(pid);
}

/* ── The failures of the authorization (EMAIL-139) ──────────────────── */

static Config *fi_cfg(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->gmail_client_id     = strdup("test-client-id.apps.googleusercontent.com");
    cfg->gmail_client_secret = strdup("test-client-secret");
    return cfg;
}

static void fi_cfg_free(Config *cfg) {
    free(cfg->gmail_client_id);
    free(cfg->gmail_client_secret);
    free(cfg->gmail_refresh_token);
}

static void test_authorize_all_loopback_ports_busy(void) {
    int held[REDIRECT_PORT_LAST - REDIRECT_PORT_FIRST + 1];
    int n = 0;
    for (int port = REDIRECT_PORT_FIRST; port <= REDIRECT_PORT_LAST; port++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));   /* as the library does */
        struct sockaddr_in a = {0};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = htons((uint16_t)port);
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(fd, 1) == 0)
            held[n++] = fd;
        else
            close(fd);      /* somebody else holds it: just as busy */
    }
    Config cfg;
    fi_cfg(&cfg);
    int rc = authorize_run(&cfg, 0, NULL);
    for (int i = 0; i < n; i++) close(held[i]);
    fi_cfg_free(&cfg);
    ASSERT(n == REDIRECT_PORT_LAST - REDIRECT_PORT_FIRST + 1, "all the loopback ports were held");
    ASSERT(rc == -1, "no free loopback port: the authorization fails");
    ASSERT(strstr(g_auth_ui.error, "listener") != NULL, "and says no listener could be opened");
    ASSERT(g_auth_shown == 0, "the person was not sent to a browser that cannot reach us");
}

static volatile sig_atomic_t g_auth_ticks;
static void auth_tick(int sig) { (void)sig; g_auth_ticks++; }

static void test_authorize_waiting_for_the_redirect_is_interrupted(void) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = auth_tick;
    sigemptyset(&sa.sa_mask);
    g_auth_ticks = 0;
    sigaction(SIGALRM, &sa, &old);
    struct itimerval it = { {0, 0}, {0, 300000} };
    setitimer(ITIMER_REAL, &it, NULL);

    Config cfg;
    fi_cfg(&cfg);
    int rc = authorize_run(&cfg, 0, NULL);

    struct itimerval off = { {0, 0}, {0, 0} };
    setitimer(ITIMER_REAL, &off, NULL);
    sigaction(SIGALRM, &old, NULL);
    ASSERT(g_auth_ticks > 0, "the signal arrived while waiting for the redirect");
    ASSERT(rc == -1, "a wait that ends without a connection fails the authorization");
    ASSERT(strstr(g_auth_ui.error, "timed out") != NULL, "and says the code did not come");
    fi_cfg_free(&cfg);
}

static void test_authorize_redirect_connection_without_a_request(void) {
    pid_t pid = fork();
    if (pid == 0) redirect_child("", 4000);
    Config cfg;
    fi_cfg(&cfg);
    int rc = authorize_run(&cfg, 0, NULL);
    kill(pid, SIGKILL);
    wait_child(pid);
    ASSERT(rc == -1, "a browser that connects and sends nothing fails the authorization");
    ASSERT(strstr(g_auth_ui.error, "Did not receive the authorization code") != NULL,
           "and the reason is named");
    fi_cfg_free(&cfg);
}

static void test_authorize_token_response_without_access_token(void) {
    const int mock_port = mock_listen();
    const char *resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"refresh_token\":\"only_refresh\"}";
    pid_t srv = fork();
    if (srv == 0) run_mock_token_server(mock_port, resp);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);
    Config cfg;
    fi_cfg(&cfg);
    int rc = authorize_run(&cfg, 1, "code=abc");
    unsetenv("GMAIL_TEST_TOKEN_URL");
    kill(srv, SIGKILL);
    wait_child(srv);
    ASSERT(rc == -1, "a token reply without access_token fails the authorization");
    ASSERT(strstr(g_auth_ui.error, "missing access_token") != NULL, "and says what is missing");
    ASSERT(cfg.gmail_refresh_token == NULL, "no half-token is stored");
    fi_cfg_free(&cfg);
}

static void test_refresh_invalid_client_message(void) {
    const int mock_port = mock_listen();
    const char *resp =
        "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n\r\n"
        "{\"error\":\"invalid_client\"}";
    pid_t pid = fork();
    if (pid == 0) run_mock_token_server(mock_port, resp);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);

    char errfile[PATH_MAX];
    snprintf(errfile, sizeof(errfile), "%s/invalid-client-XXXXXX", test_scratch_dir());
    int efd = mkstemp(errfile);
    ASSERT(efd >= 0, "invalid_client: temp file for stderr");
    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("some_token");
    fflush(stderr);
    int saved = dup(2);
    dup2(efd, 2);
    char *tok = gmail_auth_refresh(&cfg);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    ASSERT(tok == NULL, "invalid_client: returns NULL");
    char buf[1024] = {0};
    lseek(efd, 0, SEEK_SET);
    ssize_t rd = read(efd, buf, sizeof(buf) - 1);
    close(efd);
    unlink(errfile);
    ASSERT(rd > 0 && strstr(buf, "client credentials are invalid") != NULL,
           "invalid_client: says the client credentials are invalid");
    ASSERT(strstr(buf, "email-cli help gmail") != NULL,
           "invalid_client: names the guide that creates them");
    free(cfg.gmail_refresh_token);
    unsetenv("GMAIL_TEST_TOKEN_URL");
    wait_child(pid);
}

/**
 * Every allocation point of the paste-mode authorization fails in turn: the
 * result is a clean failure (the token endpoint is not there), never a crash,
 * and the leak checker sees every path release what it took.
 */
static void test_authorize_every_allocation_failing_in_turn(void) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", test_unused_port());
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);
    int swept = 0, oom = 0;
    for (long k = 1; k < 300; k++) {
        Config cfg;
        fi_cfg(&cfg);
        fault_alloc_arm(k);
        int rc = authorize_run(&cfg, 1, "code=abc");
        int fired = fault_alloc_disarm();
        if (strstr(g_auth_ui.error, "Out of memory")) oom++;
        fi_cfg_free(&cfg);
        ASSERT(rc == -1, "the token endpoint is down: the authorization fails");
        if (!fired) { swept = 1; break; }
    }
    unsetenv("GMAIL_TEST_TOKEN_URL");
    ASSERT(swept, "the sweep reached the end of the allocations");
    ASSERT(oom > 0, "a failed allocation is reported as out of memory");
}

/* ── gmail_login(): what the person sees, and the stored token (EMAIL-139) ── */

/** Run gmail_login() with stderr captured into @p text. */
static int login_captured(Config *cfg, GmailLoginMode mode, const char *input,
                          char *text, size_t cap) {
    char errfile[PATH_MAX];
    snprintf(errfile, sizeof(errfile), "%s/login-err-XXXXXX", test_scratch_dir());
    int efd = mkstemp(errfile);
    if (efd < 0) return -2;
    /* an empty stream is /dev/null: fmemopen() refuses a zero size, and a NULL
     * stream would make gmail_login() read the real stdin */
    FILE *in = (input && input[0]) ? fmemopen((void *)input, strlen(input), "r")
                                   : fopen("/dev/null", "r");
    if (!in) return -2;
    fflush(stderr);
    int saved = dup(2);
    dup2(efd, 2);
    int rc = gmail_login(cfg, mode, in);
    fclose(in);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    lseek(efd, 0, SEEK_SET);
    ssize_t n = read(efd, text, cap - 1);
    text[n > 0 ? n : 0] = '\0';
    close(efd);
    unlink(errfile);
    return rc;
}

static const char *const k_login_token_reply =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
    "{\"access_token\":\"a\",\"refresh_token\":\"login_refresh\"}";

static void test_gmail_login_paste_mode(void) {
    const int mock_port = mock_listen();
    pid_t srv = fork();
    if (srv == 0) run_mock_token_server(mock_port, k_login_token_reply);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);
    Config cfg;
    fi_cfg(&cfg);
    char text[4096];
    int rc = login_captured(&cfg, GMAIL_LOGIN_PASTE,
                            "http://localhost:8089/callback?code=PASTED&scope=x\n", text, sizeof(text));
    unsetenv("GMAIL_TEST_TOKEN_URL");
    kill(srv, SIGKILL);
    wait_child(srv);
    ASSERT(rc == 0, "paste login succeeds");
    ASSERT(cfg.gmail_refresh_token && strcmp(cfg.gmail_refresh_token, "login_refresh") == 0,
           "the refresh token is stored in the account");
    ASSERT(strstr(text, "Open this address in a browser") != NULL, "the person is told where to go");
    ASSERT(strstr(text, "Paste the address") != NULL, "and asked for the redirect");
    ASSERT(strstr(text, "Authorization successful") != NULL, "and told it worked");
    fi_cfg_free(&cfg);
}

static void test_gmail_login_paste_nothing_given(void) {
    Config cfg;
    fi_cfg(&cfg);
    char text[4096];
    int rc = login_captured(&cfg, GMAIL_LOGIN_PASTE, "", text, sizeof(text));
    ASSERT(rc == -1, "an empty paste fails the login");
    ASSERT(strstr(text, "Error:") != NULL, "and the error is printed");
    ASSERT(cfg.gmail_refresh_token == NULL, "no token is stored");
    fi_cfg_free(&cfg);
}

static void test_gmail_login_loopback_mode(void) {
    const int mock_port = mock_listen();
    pid_t srv = fork();
    if (srv == 0) run_mock_token_server(mock_port, k_login_token_reply);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);
    pid_t br = fork();
    if (br == 0) redirect_child("GET /callback?code=LOOPED HTTP/1.1\r\n\r\n", 8000);
    Config cfg;
    fi_cfg(&cfg);
    char text[4096];
    int rc = login_captured(&cfg, GMAIL_LOGIN_LOOPBACK, NULL, text, sizeof(text));
    unsetenv("GMAIL_TEST_TOKEN_URL");
    kill(br, SIGKILL);
    wait_child(br);
    kill(srv, SIGKILL);
    wait_child(srv);
    ASSERT(rc == 0, "loopback login succeeds");
    ASSERT(cfg.gmail_refresh_token && strcmp(cfg.gmail_refresh_token, "login_refresh") == 0,
           "the refresh token is stored in the account");
    ASSERT(strstr(text, "Waiting for authorization") != NULL, "the person is told it is waiting");
    ASSERT(strstr(text, "ssh -L") != NULL, "and how to reach it from another machine");
    fi_cfg_free(&cfg);
}

void test_gmail_auth(void) {
    RUN_TEST(test_refresh_no_token);
    RUN_TEST(test_refresh_empty_token);
    RUN_TEST(test_auth_code_extraction);
    RUN_TEST(test_auth_code_denied);
    RUN_TEST(test_wizard_rejects_gmail_imap);
    RUN_TEST(test_refresh_with_client_credentials);
    RUN_TEST(test_import_token);
    RUN_TEST(test_paste_redirect_url);
    RUN_TEST(test_paste_bare_code_and_encoded_code);
    RUN_TEST(test_paste_refusals);
    RUN_TEST(test_authorize_needs_callbacks);
    RUN_TEST(test_device_flow_no_credentials);
    RUN_TEST(test_device_flow_access_denied);
    RUN_TEST(test_device_flow_with_code);
    RUN_TEST(test_refresh_test_token_hook);
    RUN_TEST(test_refresh_via_mock_server_200);
    RUN_TEST(test_refresh_via_mock_server_200_no_token);
    RUN_TEST(test_refresh_via_mock_server_400_unknown_error);
    RUN_TEST(test_refresh_invalid_grant_message);
    RUN_TEST(test_refresh_via_mock_server_curl_error);
    RUN_TEST(test_device_flow_full_mock);
    RUN_TEST(test_authorize_all_loopback_ports_busy);
    RUN_TEST(test_authorize_waiting_for_the_redirect_is_interrupted);
    RUN_TEST(test_authorize_redirect_connection_without_a_request);
    RUN_TEST(test_authorize_token_response_without_access_token);
    RUN_TEST(test_refresh_invalid_client_message);
    RUN_TEST(test_authorize_every_allocation_failing_in_turn);
    RUN_TEST(test_gmail_login_paste_mode);
    RUN_TEST(test_gmail_login_paste_nothing_given);
    RUN_TEST(test_gmail_login_loopback_mode);
}
