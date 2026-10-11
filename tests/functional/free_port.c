/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-11 */
/* Prints a loopback TCP port that is free at this moment. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @file free_port.c
 * @brief Ask the kernel for a free loopback port and print it.
 *
 * The port is returned to the kernel before this program exits, so the caller
 * binds it a moment later; run_functional.sh never hands the same number out
 * twice in one run.  Exit status is non-zero, with a message on stderr, when
 * no port could be had.
 */
int main(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return EXIT_FAILURE; }
    struct sockaddr_in a = {0};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    socklen_t len = sizeof(a);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &len) != 0) {
        perror("bind");
        close(fd);
        return EXIT_FAILURE;
    }
    printf("%d\n", ntohs(a.sin_port));
    close(fd);
    return EXIT_SUCCESS;
}
