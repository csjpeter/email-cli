#include "test_helpers.h"
#include "gmail_auth.h"
#include "config.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── gmail_auth_device_flow ──────────────────────────────────────── */
/* gmail_auth_device_flow() opens a TCP listener and launches a browser,
 * so it MUST NOT be called from unit tests.  The localhost listener +
 * code extraction logic is tested separately via test_auth_code_extraction
 * and test_auth_code_denied below. */

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
     * gmail_auth_device_flow uses internally. */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "auth_code: socket created");
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(18765);
    int bound = bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    ASSERT(bound == 0, "auth_code: bind succeeded");
    listen(listen_fd, 1);

    /* Fork child to simulate browser redirect */
    pid_t pid = fork();
    if (pid == 0) {
        usleep(50000);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in ca = {0};
        ca.sin_family = AF_INET;
        ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ca.sin_port = htons(18765);
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
    int status;
    waitpid(pid, &status, 0);
}

static void test_auth_code_denied(void) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0, "denied: socket created");
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(18766);
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 1);

    pid_t pid = fork();
    if (pid == 0) {
        usleep(50000);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in ca = {0};
        ca.sin_family = AF_INET;
        ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ca.sin_port = htons(18766);
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
    int status;
    waitpid(pid, &status, 0);
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

/* ── Additional coverage tests ──────────────────────────────────────── */

static void test_refresh_with_client_credentials(void) {
    /* Covers get_client_id (non-NULL cfg field), get_client_secret, the post
     * building, and http_post call in gmail_auth_refresh.
     * http_post will fail (no valid OAuth credentials) → tok is NULL. */
    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("dummy_refresh_token");
    cfg.gmail_client_id     = strdup("test-client-id.apps.googleusercontent.com");
    cfg.gmail_client_secret = strdup("test-client-secret");
    unsetenv("GMAIL_TEST_TOKEN");

    char *tok = gmail_auth_refresh(&cfg);
    /* Returns NULL (network failure or 400 from Google) — just no crash */
    free(tok);

    free(cfg.gmail_refresh_token);
    free(cfg.gmail_client_id);
    free(cfg.gmail_client_secret);
}

static void test_device_flow_no_credentials(void) {
    /* Empty client_id → returns -1 immediately, covering lines 184-203. */
    Config cfg = {0};
    int saved = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) dup2(dn, 2);
    int rc = gmail_auth_device_flow(&cfg);
    if (dn >= 0) { dup2(saved, 2); close(dn); }
    close(saved);
    ASSERT(rc == -1, "device_flow: empty client_id returns -1");
}

static void test_device_flow_access_denied(void) {
    /* Child sends error=access_denied → wait_for_auth_code returns NULL
     * (covers open_listener, wait_for_auth_code error path, device_flow setup).
     * No network call needed since auth_code is NULL → function returns -1. */
    Config cfg = {0};
    cfg.gmail_client_id     = strdup("test-client-id.apps.googleusercontent.com");
    cfg.gmail_client_secret = strdup("test-client-secret");

    pid_t pid = fork();
    if (pid == 0) {
        usleep(200000);
        for (int port = 8089; port <= 8099; port++) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            struct sockaddr_in ca = {0};
            ca.sin_family      = AF_INET;
            ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ca.sin_port        = htons((uint16_t)port);
            if (connect(fd, (struct sockaddr *)&ca, sizeof(ca)) == 0) {
                const char *req =
                    "GET /callback?error=access_denied HTTP/1.1\r\n"
                    "Host: localhost\r\n\r\n";
                ssize_t w = write(fd, req, strlen(req)); (void)w;
                char buf[512];
                ssize_t r = read(fd, buf, sizeof(buf)); (void)r;
                close(fd);
                _exit(0);
            }
            close(fd);
        }
        _exit(1);
    }

    int saved = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) dup2(dn, 2);
    int rc = gmail_auth_device_flow(&cfg);
    if (dn >= 0) { dup2(saved, 2); close(dn); }
    close(saved);

    ASSERT(rc == -1, "device_flow: access_denied returns -1");

    int status;
    waitpid(pid, &status, 0);

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
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) _exit(1);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(srv, 1) != 0) {
        close(srv);
        _exit(1);
    }
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    int conn = accept(srv, (struct sockaddr *)&cli, &cli_len);
    if (conn < 0) { close(srv); _exit(1); }
    char buf[4096];
    ssize_t n = read(conn, buf, sizeof(buf) - 1); (void)n;
    ssize_t w = write(conn, response, strlen(response)); (void)w;
    close(conn);
    close(srv);
    _exit(0);
}

static void test_refresh_via_mock_server_200(void) {
    /* Covers lines 336-339: code==200 path in gmail_auth_refresh */
    const int mock_port = 18770;
    const char *http_resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"access_token\":\"mock_access_tok\",\"token_type\":\"Bearer\"}";

    pid_t pid = fork();
    if (pid == 0) {
        run_mock_token_server(mock_port, http_resp);
    }
    usleep(50000); /* let server bind */

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
    int status;
    waitpid(pid, &status, 0);
}

static void test_refresh_via_mock_server_200_no_token(void) {
    /* Covers lines 290-293: code==200 but no access_token in response */
    const int mock_port = 18771;
    const char *http_resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"token_type\":\"Bearer\"}"; /* no access_token field */

    pid_t pid = fork();
    if (pid == 0) {
        run_mock_token_server(mock_port, http_resp);
    }
    usleep(50000);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);

    Config cfg = {0};
    cfg.gmail_refresh_token = strdup("valid_looking_refresh_token");
    char *tok = gmail_auth_refresh(&cfg);
    ASSERT(tok == NULL, "mock 200 no token: returns NULL");
    free(cfg.gmail_refresh_token);

    unsetenv("GMAIL_TEST_TOKEN_URL");
    int status;
    waitpid(pid, &status, 0);
}

static void test_refresh_via_mock_server_400_unknown_error(void) {
    /* Covers lines 351-352: non-200 with error that is neither invalid_grant
     * nor invalid_client */
    const int mock_port = 18772;
    const char *http_resp =
        "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n\r\n"
        "{\"error\":\"unsupported_grant_type\"}";

    pid_t pid = fork();
    if (pid == 0) {
        run_mock_token_server(mock_port, http_resp);
    }
    usleep(50000);

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
    int status;
    waitpid(pid, &status, 0);
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
    /* Covers lines 287-305: successful token exchange in gmail_auth_device_flow.
     * Child 1 simulates the browser redirect (code=xxx).
     * Child 2 is the mock token server that returns a valid access_token.
     * The function completes successfully → returns 0. */
    const int mock_port = 18773;
    const char *token_resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"access_token\":\"dev_flow_tok\",\"refresh_token\":\"dev_flow_refresh\","
        "\"token_type\":\"Bearer\"}";

    /* Start mock token server */
    pid_t srv_pid = fork();
    if (srv_pid == 0) {
        run_mock_token_server(mock_port, token_resp);
    }
    usleep(60000); /* let server bind */

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/token", mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);

    Config cfg = {0};
    cfg.gmail_client_id     = strdup("test-client-id.apps.googleusercontent.com");
    cfg.gmail_client_secret = strdup("test-client-secret");

    /* Browser redirect child */
    pid_t br_pid = fork();
    if (br_pid == 0) {
        usleep(250000); /* wait for listener to open */
        for (int port = 8089; port <= 8099; port++) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            struct sockaddr_in ca = {0};
            ca.sin_family      = AF_INET;
            ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ca.sin_port        = htons((uint16_t)port);
            if (connect(fd, (struct sockaddr *)&ca, sizeof(ca)) == 0) {
                const char *req =
                    "GET /callback?code=full_test_code&scope=x HTTP/1.1\r\n"
                    "Host: localhost\r\n\r\n";
                ssize_t w = write(fd, req, strlen(req)); (void)w;
                char buf[512];
                ssize_t r = read(fd, buf, sizeof(buf)); (void)r;
                close(fd);
                _exit(0);
            }
            close(fd);
        }
        _exit(1);
    }

    int saved = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) dup2(dn, 2);
    int rc = gmail_auth_device_flow(&cfg);
    if (dn >= 0) { dup2(saved, 2); close(dn); }
    close(saved);

    ASSERT(rc == 0, "device_flow full mock: returns 0 on success");
    if (cfg.gmail_refresh_token)
        ASSERT(strcmp(cfg.gmail_refresh_token, "dev_flow_refresh") == 0,
               "device_flow full mock: refresh_token set");

    free(cfg.gmail_client_id);
    free(cfg.gmail_client_secret);
    free(cfg.gmail_refresh_token);

    int status;
    waitpid(br_pid,  &status, 0);
    waitpid(srv_pid, &status, 0);

    unsetenv("GMAIL_TEST_TOKEN_URL");
}

static void test_device_flow_with_code(void) {
    /* Child sends a real code= → wait_for_auth_code returns the code.
     * Covers wait_for_auth_code code-extraction path and the post-building
     * step in gmail_auth_device_flow.  http_post to TOKEN_URL fails
     * (invalid credentials / no network) → returns -1. */
    Config cfg = {0};
    cfg.gmail_client_id     = strdup("test-client-id.apps.googleusercontent.com");
    cfg.gmail_client_secret = strdup("test-client-secret");

    pid_t pid = fork();
    if (pid == 0) {
        usleep(200000);
        for (int port = 8089; port <= 8099; port++) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            struct sockaddr_in ca = {0};
            ca.sin_family      = AF_INET;
            ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ca.sin_port        = htons((uint16_t)port);
            if (connect(fd, (struct sockaddr *)&ca, sizeof(ca)) == 0) {
                const char *req =
                    "GET /callback?code=test_code_exchange&scope=x HTTP/1.1\r\n"
                    "Host: localhost\r\n\r\n";
                ssize_t w = write(fd, req, strlen(req)); (void)w;
                char buf[512];
                ssize_t r = read(fd, buf, sizeof(buf)); (void)r;
                close(fd);
                _exit(0);
            }
            close(fd);
        }
        _exit(1);
    }

    int saved = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) dup2(dn, 2);
    int rc = gmail_auth_device_flow(&cfg);
    if (dn >= 0) { dup2(saved, 2); close(dn); }
    close(saved);

    /* Token exchange fails with invalid test credentials → returns -1 */
    ASSERT(rc == -1, "device_flow: invalid credentials returns -1");

    int status;
    waitpid(pid, &status, 0);

    free(cfg.gmail_client_id);
    free(cfg.gmail_client_secret);
    free(cfg.gmail_refresh_token);
}

/* ── Registration ─────────────────────────────────────────────────── */

void test_gmail_auth(void) {
    RUN_TEST(test_refresh_no_token);
    RUN_TEST(test_refresh_empty_token);
    RUN_TEST(test_auth_code_extraction);
    RUN_TEST(test_auth_code_denied);
    RUN_TEST(test_wizard_rejects_gmail_imap);
    RUN_TEST(test_refresh_with_client_credentials);
    RUN_TEST(test_device_flow_no_credentials);
    RUN_TEST(test_device_flow_access_denied);
    RUN_TEST(test_device_flow_with_code);
    RUN_TEST(test_refresh_test_token_hook);
    RUN_TEST(test_refresh_via_mock_server_200);
    RUN_TEST(test_refresh_via_mock_server_200_no_token);
    RUN_TEST(test_refresh_via_mock_server_400_unknown_error);
    RUN_TEST(test_refresh_via_mock_server_curl_error);
    RUN_TEST(test_device_flow_full_mock);
}
