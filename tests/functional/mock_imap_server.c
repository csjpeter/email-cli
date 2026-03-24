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

        // Simple State Machine
        if (strstr(buffer, "CAPABILITY")) {
            const char *resp = "* CAPABILITY IMAP4rev1 AUTH=PLAIN\r\nOK CAPABILITY completed\r\n";
            send(client_sock, resp, strlen(resp), 0);
        } else if (strstr(buffer, "LOGIN")) {
            const char *resp = "OK LOGIN completed\r\n";
            send(client_sock, resp, strlen(resp), 0);
        } else if (strstr(buffer, "SELECT")) {
            const char *resp = "* 1 EXISTS\r\nOK [READ-WRITE] SELECT completed\r\n";
            send(client_sock, resp, strlen(resp), 0);
        } else if (strstr(buffer, "FETCH")) {
            const char *msg = "Subject: Test Message\r\n\r\nHello from Mock Server!";
            char resp[1024];
            snprintf(resp, sizeof(resp), "* 1 FETCH (BODY[] {%zu}\r\n%s)\r\nOK FETCH completed\r\n", strlen(msg), msg);
            send(client_sock, resp, strlen(resp), 0);
        } else if (strstr(buffer, "LOGOUT")) {
            const char *resp = "* BYE Mock IMAP server logging out\r\nOK LOGOUT completed\r\n";
            send(client_sock, resp, strlen(resp), 0);
            break;
        } else {
            const char *resp = "BAD Unknown command\r\n";
            send(client_sock, resp, strlen(resp), 0);
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

    while ((client_sock = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen))) {
        printf("Connection accepted\n");
        handle_client(client_sock);
        // Only handle one session for test simplicity then exit
        break; 
    }

    close(server_fd);
    return 0;
}
