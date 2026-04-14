#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <signal.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/**
 * @file mock_smtp_server.c
 * @brief Minimal TLS SMTP server for compose/send integration testing.
 *
 * Listens on port 9025 (configurable via MOCK_SMTP_PORT env var).
 * Accepts one SMTP exchange, prints "RECEIVED" + message headers to stdout,
 * then accepts further connections until killed.
 */

#define DEFAULT_PORT 9025

static int readline_crlf_ssl(SSL *ssl, char *buf, int len) {
    int n = 0;
    char c;
    while (n < len - 1) {
        int r = SSL_read(ssl, &c, 1);
        if (r <= 0) return -1;
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

static void ssl_sendf(SSL *ssl, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SSL_write(ssl, buf, (int)strlen(buf));
}

static void handle_client(SSL *ssl) {
    int fd = SSL_get_fd(ssl);
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[4096];

    /* Greeting */
    ssl_sendf(ssl, "220 Mock SMTP Server ready\r\n");

    int in_data = 0;
    char msg_body[65536] = "";
    size_t msg_len = 0;

    while (1) {
        if (readline_crlf_ssl(ssl, buf, (int)sizeof(buf)) < 0) break;
        printf("C: %s\n", buf);

        if (in_data) {
            /* DATA mode: accumulate until \r\n.\r\n */
            if (strcmp(buf, ".") == 0) {
                in_data = 0;
                ssl_sendf(ssl, "250 2.0.0 Message queued\r\n");
                printf("RECEIVED\n");
                printf("--- message begin ---\n%s--- message end ---\n", msg_body);
                fflush(stdout);
            } else {
                /* Dot-stuffing: leading dot (not a lone dot) → strip one dot */
                const char *line = (buf[0] == '.' && buf[1]) ? buf + 1 : buf;
                size_t llen = strlen(line);
                if (msg_len + llen + 2 < sizeof(msg_body)) {
                    memcpy(msg_body + msg_len, line, llen);
                    msg_len += llen;
                    msg_body[msg_len++] = '\n';
                    msg_body[msg_len]   = '\0';
                }
            }
            continue;
        }

        if (strncasecmp(buf, "EHLO", 4) == 0 || strncasecmp(buf, "HELO", 4) == 0) {
            ssl_sendf(ssl, "250-mock.smtp\r\n"
                           "250-AUTH LOGIN PLAIN\r\n"
                           "250-SIZE 10485760\r\n"
                           "250 OK\r\n");
        } else if (strncasecmp(buf, "AUTH LOGIN", 10) == 0) {
            ssl_sendf(ssl, "334 VXNlcm5hbWU6\r\n");             /* Username: */
            readline_crlf_ssl(ssl, buf, sizeof(buf));            /* base64 username */
            ssl_sendf(ssl, "334 UGFzc3dvcmQ6\r\n");             /* Password: */
            readline_crlf_ssl(ssl, buf, sizeof(buf));            /* base64 password */
            ssl_sendf(ssl, "235 2.7.0 Authentication successful\r\n");
        } else if (strncasecmp(buf, "AUTH PLAIN", 10) == 0) {
            /* AUTH PLAIN <base64> inline → immediate accept */
            /* AUTH PLAIN (no credentials) → challenge then accept */
            if (buf[10] == '\0' || buf[10] == '\r') {
                ssl_sendf(ssl, "334 \r\n");
                readline_crlf_ssl(ssl, buf, sizeof(buf)); /* base64 credentials */
            }
            ssl_sendf(ssl, "235 2.7.0 Authentication successful\r\n");
        } else if (strncasecmp(buf, "MAIL FROM", 9) == 0) {
            ssl_sendf(ssl, "250 2.1.0 OK\r\n");
        } else if (strncasecmp(buf, "RCPT TO", 7) == 0) {
            ssl_sendf(ssl, "250 2.1.5 OK\r\n");
        } else if (strncasecmp(buf, "DATA", 4) == 0) {
            ssl_sendf(ssl, "354 Start input; end with <CRLF>.<CRLF>\r\n");
            in_data = 1;
            msg_body[0] = '\0';
            msg_len = 0;
        } else if (strncasecmp(buf, "QUIT", 4) == 0) {
            ssl_sendf(ssl, "221 2.0.0 Bye\r\n");
            break;
        } else if (strncasecmp(buf, "NOOP", 4) == 0) {
            ssl_sendf(ssl, "250 OK\r\n");
        } else if (strncasecmp(buf, "RSET", 4) == 0) {
            ssl_sendf(ssl, "250 OK\r\n");
        } else {
            ssl_sendf(ssl, "502 5.5.2 Command not recognized\r\n");
        }
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    const char *port_env = getenv("MOCK_SMTP_PORT");
    int port = port_env ? atoi(port_env) : DEFAULT_PORT;

    /* Load TLS certificate and key */
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fprintf(stderr, "SSL_CTX_new failed\n");
        ERR_print_errors_fp(stderr);
        return 1;
    }
    if (SSL_CTX_use_certificate_file(ctx, "tests/certs/test.crt", SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "Failed to load certificate: tests/certs/test.crt\n");
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return 1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, "tests/certs/test.key", SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "Failed to load private key: tests/certs/test.key\n");
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return 1;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); SSL_CTX_free(ctx); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); SSL_CTX_free(ctx); return 1;
    }
    if (listen(srv, 5) < 0) {
        perror("listen"); close(srv); SSL_CTX_free(ctx); return 1;
    }

    printf("Mock SMTP server (TLS) listening on port %d\n", port);
    fflush(stdout);

    socklen_t addrlen = sizeof(addr);
    int fd;
    while ((fd = accept(srv, (struct sockaddr *)&addr, &addrlen)) >= 0) {
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            close(fd);
            continue;
        }
        handle_client(ssl);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
    }

    close(srv);
    SSL_CTX_free(ctx);
    return 0;
}
