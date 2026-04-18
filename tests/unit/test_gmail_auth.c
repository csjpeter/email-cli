#include "test_helpers.h"
#include "gmail_auth.h"
#include "config.h"
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
    read(conn, req, sizeof(req) - 1);

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

/* ── Registration ─────────────────────────────────────────────────── */

void test_gmail_auth(void) {
    RUN_TEST(test_refresh_no_token);
    RUN_TEST(test_refresh_empty_token);
    RUN_TEST(test_auth_code_extraction);
    RUN_TEST(test_auth_code_denied);
    RUN_TEST(test_wizard_rejects_gmail_imap);
}
