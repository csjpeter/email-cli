#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/**
 * @file mock_imap_server.c
 * @brief Minimal TLS IMAP server for integration testing.
 */

#define PORT 9993

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
            const char *resp = "* CAPABILITY IMAP4rev1\r\n";
            SSL_write(ssl, resp, (int)strlen(resp));
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK CAPABILITY completed\r\n", tag);
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
            const char *exists = is_empty
                ? "* 0 EXISTS\r\n* 0 RECENT\r\n* OK [UIDVALIDITY 1] UIDs are valid\r\n"
                : "* 1 EXISTS\r\n* 1 RECENT\r\n* OK [UIDVALIDITY 1] UIDs are valid\r\n";
            SSL_write(ssl, exists, (int)strlen(exists));
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK [READ-WRITE] SELECT completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "LIST")) {
            const char *list_resp =
                "* LIST (\\HasNoChildren) \".\" \"INBOX\"\r\n"
                "* LIST (\\HasNoChildren) \".\" \"INBOX.Sent\"\r\n"
                "* LIST (\\HasNoChildren) \".\" \"INBOX.Trash\"\r\n"
                "* LIST (\\HasNoChildren) \".\" \"INBOX.Empty\"\r\n";
            SSL_write(ssl, list_resp, (int)strlen(list_resp));
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK LIST completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "SEARCH")) {
            int is_empty = (strstr(selected_folder, "Empty") != NULL);
            const char *search_resp = is_empty ? "* SEARCH\r\n" : "* SEARCH 1\r\n";
            SSL_write(ssl, search_resp, (int)strlen(search_resp));
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK SEARCH completed\r\n", tag);
            SSL_write(ssl, ok, (int)strlen(ok));
        } else if (strstr(buffer, "FETCH") && strstr(buffer, "FLAGS")) {
            /* FLAGS-only fetch — no literal payload, inline response */
            char resp[64];
            snprintf(resp, sizeof(resp), "* 1 FETCH (FLAGS ())\r\n");
            SSL_write(ssl, resp, (int)strlen(resp));
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
        } else if (strstr(buffer, "FETCH")) {
            const char *headers =
                "From: Test User <test@example.com>\r\n"
                "Subject: Test Message\r\n"
                "Date: Thu, 26 Mar 2026 12:00:00 +0000\r\n"
                "\r\n";
            /* multipart/mixed message: HTML body + two attachments.
             * notes.txt  = base64("Hello World")
             * data.bin   = base64("test data")
             * The HTML part is intentionally identical to the old plain
             * HTML message so that existing show-view tests still pass. */
            const char *full_msg =
                "From: Test User <test@example.com>\r\n"
                "Subject: Test Message\r\n"
                "Date: Thu, 26 Mar 2026 12:00:00 +0000\r\n"
                "MIME-Version: 1.0\r\n"
                "Content-Type: multipart/mixed; boundary=\"B001\"\r\n"
                "\r\n"
                "--B001\r\n"
                "Content-Type: text/html; charset=UTF-8\r\n"
                "\r\n"
                "<html>"
                "<head><style>body { color: red; font-size: 14px; }</style></head>"
                "<body><b>Hello from Mock Server!</b><br>"
                "Line 2<br>Line 3<br>Line 4<br>Line 5<br>"
                "Line 6<br>Line 7<br>Line 8<br>Line 9</body>"
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
                "--B001--\r\n";

            int is_header = strstr(buffer, "HEADER") != NULL;
            const char *content = is_header ? headers : full_msg;
            const char *section = is_header ? "BODY[HEADER]" : "BODY[]";

            char head[128];
            snprintf(head, sizeof(head), "* 1 FETCH (%s {%zu}\r\n", section, strlen(content));
            SSL_write(ssl, head, (int)strlen(head));

            /* Send content in chunks to be realistic */
            SSL_write(ssl, content, (int)strlen(content));

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
                char ok[64];
                snprintf(ok, sizeof(ok), "%s OK [APPENDUID 1 42] APPEND completed\r\n", tag);
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
    address.sin_port = htons(PORT);

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

    printf("Mock IMAP Server (TLS) listening on port %d\n", PORT);

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
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(client_sock);
        printf("Connection closed\n");
    }

    close(server_fd);
    SSL_CTX_free(ctx);
    return 0;
}
