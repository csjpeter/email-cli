#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

/**
 * @file mock_imap_server.c
 * @brief Minimal IMAP server for integration testing.
 */

#define PORT 9993

void handle_client(int client_sock) {
    char buffer[4096];
    char selected_folder[256] = "";   /* currently selected mailbox */

    /* Set a recv timeout so we don't block forever on half-closed connections */
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 1. Greeting
    const char *greeting = "* OK Mock IMAP server ready\r\n";
    send(client_sock, greeting, strlen(greeting), 0);

    while (1) {
        int bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) break;
        buffer[bytes] = 0;
        printf("Client requested: %s", buffer);

        // Extract Tag
        char tag[16] = {0};
        if (sscanf(buffer, "%15s", tag) != 1) {
            strcpy(tag, "*");
        }

        // Simple State Machine
        if (strstr(buffer, "CAPABILITY")) {
            const char *resp = "* CAPABILITY IMAP4rev1\r\n";
            send(client_sock, resp, strlen(resp), 0);
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK CAPABILITY completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
        } else if (strstr(buffer, "LOGIN")) {
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK LOGIN completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
        } else if (strstr(buffer, "SELECT")) {
            /* Track selected folder — handle both quoted and unquoted names */
            char *sel = strstr(buffer, "SELECT ");
            if (sel) {
                sel += 7; /* skip "SELECT " */
                if (*sel == '"') {
                    /* quoted: SELECT "INBOX.Empty" */
                    sel++;
                    char *q2 = strchr(sel, '"');
                    size_t len = q2 ? (size_t)(q2 - sel) : strlen(sel);
                    if (len >= sizeof(selected_folder)) len = sizeof(selected_folder) - 1;
                    strncpy(selected_folder, sel, len);
                    selected_folder[len] = '\0';
                } else {
                    /* unquoted: SELECT INBOX.Empty */
                    size_t len = strcspn(sel, " \r\n");
                    if (len >= sizeof(selected_folder)) len = sizeof(selected_folder) - 1;
                    strncpy(selected_folder, sel, len);
                    selected_folder[len] = '\0';
                }
            }
            int is_empty = (strstr(selected_folder, "Empty") != NULL);
            const char *exists = is_empty
                ? "* 0 EXISTS\r\n* 0 RECENT\r\n* OK [UIDVALIDITY 1] UIDs are valid\r\n"
                : "* 1 EXISTS\r\n* 1 RECENT\r\n* OK [UIDVALIDITY 1] UIDs are valid\r\n";
            send(client_sock, exists, strlen(exists), 0);
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK [READ-WRITE] SELECT completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
        } else if (strstr(buffer, "LIST")) {
            const char *list_resp =
                "* LIST (\\HasNoChildren) \".\" \"INBOX\"\r\n"
                "* LIST (\\HasNoChildren) \".\" \"INBOX.Sent\"\r\n"
                "* LIST (\\HasNoChildren) \".\" \"INBOX.Trash\"\r\n"
                "* LIST (\\HasNoChildren) \".\" \"INBOX.Empty\"\r\n";
            send(client_sock, list_resp, strlen(list_resp), 0);
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK LIST completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
        } else if (strstr(buffer, "SEARCH")) {
            int is_empty = (strstr(selected_folder, "Empty") != NULL);
            const char *search_resp = is_empty ? "* SEARCH\r\n" : "* SEARCH 1\r\n";
            send(client_sock, search_resp, strlen(search_resp), 0);
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK SEARCH completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
        } else if (strstr(buffer, "FETCH") && strstr(buffer, "FLAGS")) {
            /* FLAGS-only fetch — no literal payload, inline response */
            char resp[64];
            snprintf(resp, sizeof(resp), "* 1 FETCH (FLAGS ())\r\n");
            send(client_sock, resp, strlen(resp), 0);
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK FETCH completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
        } else if (strstr(buffer, "STORE")) {
            /* Flag update (e.g. restore \Seen) */
            const char *resp = "* 1 FETCH (FLAGS ())\r\n";
            send(client_sock, resp, strlen(resp), 0);
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK STORE completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
        } else if (strstr(buffer, "FETCH")) {
            const char *headers =
                "From: Test User <test@example.com>\r\n"
                "Subject: Test Message\r\n"
                "Date: Thu, 26 Mar 2026 12:00:00 +0000\r\n"
                "\r\n";
            /* HTML-only message (no text/plain part) with an embedded
             * <style> block — CSS must be suppressed in rendered output. */
            const char *full_msg =
                "From: Test User <test@example.com>\r\n"
                "Subject: Test Message\r\n"
                "Date: Thu, 26 Mar 2026 12:00:00 +0000\r\n"
                "Content-Type: text/html; charset=UTF-8\r\n"
                "\r\n"
                "<html>"
                "<head><style>body { color: red; font-size: 14px; }</style></head>"
                "<body><b>Hello from Mock Server!</b></body>"
                "</html>";

            int is_header = strstr(buffer, "HEADER") != NULL;
            const char *content = is_header ? headers : full_msg;
            const char *section = is_header ? "BODY[HEADER]" : "BODY[]";

            char head[128];
            snprintf(head, sizeof(head), "* 1 FETCH (%s {%zu}\r\n", section, strlen(content));
            send(client_sock, head, strlen(head), 0);

            // Send content in chunks to be realistic
            send(client_sock, content, strlen(content), 0);

            const char *tail = ")\r\n";
            send(client_sock, tail, strlen(tail), 0);

            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK FETCH completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
            printf("Mock server sent FETCH response for %s\n", section);
        } else if (strstr(buffer, "LOGOUT")) {
            const char *bye = "* BYE Mock IMAP server logging out\r\n";
            send(client_sock, bye, strlen(bye), 0);
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK LOGOUT completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
            break;
        } else {
            char bad[64];
            snprintf(bad, sizeof(bad), "%s BAD Unknown command\r\n", tag);
            send(client_sock, bad, strlen(bad), 0);
        }
    }
    close(client_sock);
}

int main() {
    int server_fd, client_sock;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Mock IMAP Server listening on port %d\n", PORT);

    while ((client_sock = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) >= 0) {
        printf("Connection accepted from client\n");
        handle_client(client_sock);
        printf("Connection closed\n");
    }

    close(server_fd);
    return 0;
}
