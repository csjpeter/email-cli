/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-24 */
/* Unit-test helper: runs a function with stdin and stdout on a pseudo-terminal. */
#ifndef UNIT_PTY_STDIO_H
#define UNIT_PTY_STDIO_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/**
 * @file pty_stdio.h
 * @brief stdin and stdout of the process are put on the slave side of a
 *        pseudo-terminal of a given size while a unit under test runs.  A
 *        feeder process types @p input after a short delay (so that a
 *        function that switches the terminal mode and flushes pending input
 *        sees it), and what the unit wrote is returned by pty_stdio_end().
 */

typedef struct {
    int master, slave, saved_in, saved_out;
    pid_t feeder;
    pthread_t drainer;          /* reads what the unit writes, so the pty never fills up */
    volatile int stop;
    char *buf;
    size_t len, cap;
} PtyStdio;

static inline void *pty_stdio_drain(void *arg) {
    PtyStdio *p = arg;
    for (;;) {
        struct pollfd pf = { p->master, POLLIN, 0 };
        int pr = poll(&pf, 1, 20);
        if (pr > 0) {
            if (p->len + 4096 > p->cap) {
                size_t nc = p->cap ? p->cap * 2 : 65536;
                char *nb = realloc(p->buf, nc);
                if (!nb) break;
                p->buf = nb;
                p->cap = nc;
            }
            ssize_t r = read(p->master, p->buf + p->len, p->cap - p->len);
            if (r > 0) p->len += (size_t)r;
        } else if (pr == 0 && p->stop) {
            break;
        } else if (pr < 0) {
            break;
        }
    }
    return NULL;
}

/** Starts the redirection; returns 0, or -1 when no terminal could be made. */
static inline int pty_stdio_begin(PtyStdio *p, int rows, int cols, const char *input) {
    memset(p, 0, sizeof(*p));
    p->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (p->master < 0 || grantpt(p->master) != 0 || unlockpt(p->master) != 0) return -1;
    const char *name = ptsname(p->master);
    p->slave = name ? open(name, O_RDWR | O_NOCTTY) : -1;
    if (p->slave < 0) return -1;
    struct termios t;
    tcgetattr(p->slave, &t);
    cfmakeraw(&t);
    tcsetattr(p->slave, TCSANOW, &t);
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    ioctl(p->slave, TIOCSWINSZ, &ws);
    fflush(stdout);
    p->saved_in = dup(STDIN_FILENO);
    p->saved_out = dup(STDOUT_FILENO);
    dup2(p->slave, STDIN_FILENO);
    dup2(p->slave, STDOUT_FILENO);
    clearerr(stdin);
    alarm(30);   /* a unit that waits for a key that never comes ends the run, loudly */
    p->feeder = -1;
    if (input && *input) {
        p->feeder = fork();
        if (p->feeder == 0) {
            struct timespec ts = { 0, 150 * 1000000L };
            nanosleep(&ts, NULL);
            /* A '|' in the input is a pause, so that an Esc is not taken
             * for the start of an Alt+key. */
            for (const char *s = input; *s;) {
                if (*s == '|') {
                    struct timespec gap = { 0, 250 * 1000000L };
                    nanosleep(&gap, NULL);
                    s++;
                    continue;
                }
                size_t len = strcspn(s, "|");
                if (write(p->master, s, len) != (ssize_t)len) break;
                s += len;
            }
            /* A unit still waiting for a key after this is stuck: Ctrl-C frees it,
             * and its assertions then fail instead of the run hanging. */
            struct timespec stuck = { 5, 0 };
            nanosleep(&stuck, NULL);
            if (write(p->master, "\003", 1) < 0) { /* the unit is gone */ }
            _exit(0);
        }
    }
    if (pthread_create(&p->drainer, NULL, pty_stdio_drain, p) != 0) return -1;
    return 0;
}

/** Ends the redirection; what was written to the terminal is copied to @p out. */
static inline void pty_stdio_end(PtyStdio *p, char *out, size_t cap) {
    alarm(0);
    fflush(stdout);
    dup2(p->saved_in, STDIN_FILENO);
    dup2(p->saved_out, STDOUT_FILENO);
    close(p->saved_in);
    close(p->saved_out);
    clearerr(stdin);
    if (p->feeder > 0) {
        int st;
        kill(p->feeder, SIGKILL);
        waitpid(p->feeder, &st, 0);
    }
    p->stop = 1;
    pthread_join(p->drainer, NULL);
    if (out && cap) {
        size_t n = p->len < cap - 1 ? p->len : cap - 1;
        if (p->buf) memcpy(out, p->buf, n);
        out[n] = '\0';
    }
    free(p->buf);
    close(p->slave);
    close(p->master);
}

#endif /* UNIT_PTY_STDIO_H */
