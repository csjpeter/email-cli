#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/**
 * @file mock_imap_server.c
 * @brief Minimal IMAP server to satisfy libcurl for testing.
 */

#define PORT 9993 // We'll use plain IMAP for testing simplicity or a high port

void handle_client(int client_sock) {
    char buffer[1024];
    
    // 1. Greeting
    const char *greeting = "* OK Mock IMAP server ready\r\n";
    send(client_sock, greeting, strlen(greeting), 0);

    while (1) {
        int bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) break;
        buffer[bytes] = 0;
        printf("Client: %s", buffer);

        // Extract Tag
        char tag[16] = {0};
        sscanf(buffer, "%15s", tag);

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
            const char *exists = "* 1 EXISTS\r\n";
            send(client_sock, exists, strlen(exists), 0);
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK [READ-WRITE] SELECT completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
        } else if (strstr(buffer, "SEARCH")) {
            /* Respond to "UID SEARCH ALL" – report one message exists */
            const char *search_resp = "* SEARCH 1\r\n";
            send(client_sock, search_resp, strlen(search_resp), 0);
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK SEARCH completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
        } else if (strstr(buffer, "FETCH")) {
            const char *msg = "Subject: Test Message\r\n\r\nHello from Mock Server!";
            char data[1024];
            snprintf(data, sizeof(data), "* 1 FETCH (BODY[] {%zu}\r\n%s)\r\n", strlen(msg), msg);
            send(client_sock, data, strlen(data), 0);
            char ok[64];
            snprintf(ok, sizeof(ok), "%s OK FETCH completed\r\n", tag);
            send(client_sock, ok, strlen(ok), 0);
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
        printf("Connection accepted\n");
        handle_client(client_sock);
    }

    close(server_fd);
    return 0;
}
