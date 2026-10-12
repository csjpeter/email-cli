/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-11 */
/* Port selection and publication shared by the mock servers. */
#ifndef MOCK_PORT_H
#define MOCK_PORT_H

#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @file mock_port.h
 * @brief A mock binds the port it is told to, or lets the kernel pick one.
 *
 * `MOCK_*_PORT` is 0 or unset: the kernel picks a free loopback port.  Either
 * way, after listen() the mock writes the port it really got to the file named
 * by `MOCK_*_PORT_FILE` (written whole and renamed, so a reader that sees the
 * file sees the number).  The test that started the mock waits for that file
 * and reads the port from it; there is no fixed port to collide on.
 */

/** @brief Port requested in @p env, 0 when unset, empty or not a port. */
static inline int mock_requested_port(const char *env)
{
    const char *v = getenv(env);
    if (!v || !v[0]) return 0;
    int p = atoi(v);
    return (p > 0 && p < 65536) ? p : 0;
}

/**
 * @brief Publish the port @p fd is bound to.
 * @param fd        A bound, listening socket.
 * @param file_env  Name of the environment variable holding the file path.
 * @return the port, or -1 on failure (message on stderr).
 */
static inline int mock_publish_port(int fd, const char *file_env)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &len) != 0) {
        perror("getsockname");
        return -1;
    }
    int port = ntohs(a.sin_port);
    const char *file = getenv(file_env);
    if (!file || !file[0]) return port;

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", file);
    FILE *f = fopen(tmp, "w");
    if (!f) { perror(tmp); return -1; }
    if (fprintf(f, "%d\n", port) < 0 || fclose(f) != 0) {
        perror(tmp);
        return -1;
    }
    if (rename(tmp, file) != 0) { perror(file); return -1; }
    return port;
}

#endif /* MOCK_PORT_H */
