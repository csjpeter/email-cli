/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* The event wait that also reports a terminal resize. */
#include "key_events.h"

#include <tuikit/tty.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/**
 * @file key_events.c
 * @brief See key_events.h.  SIGWINCH sets a flag and interrupts the wait; the
 *        signal is blocked outside ppoll() so that a resize cannot slip
 *        between the check of the flag and the wait.
 */

static volatile sig_atomic_t g_resized;

static void on_winch(int sig) {
    (void)sig;
    g_resized = 1;
}

void key_events_begin(KeyEvents *ke) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    g_resized = 0;
    sigaction(SIGWINCH, &sa, &ke->old);
}

void key_events_end(KeyEvents *ke) {
    sigaction(SIGWINCH, &ke->old, NULL);
}

int key_events_next(int fd, tui_decoder *d, tui_event *ev, int idle_ms) {
    sigset_t block, orig;
    sigemptyset(&block);
    sigaddset(&block, SIGWINCH);
    for (;;) {
        if (tui_decoder_next(d, ev)) return 1;
        int pending = tui_decoder_pending(d);
        sigprocmask(SIG_BLOCK, &block, &orig);
        if (g_resized) {
            g_resized = 0;
            sigprocmask(SIG_SETMASK, &orig, NULL);
            memset(ev, 0, sizeof(*ev));
            ev->type = TUI_EV_RESIZE;
            return 1;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        struct timespec ts, *tsp = NULL;
        int wait = pending ? TUI_ESC_DELAY_DEFAULT : idle_ms;
        if (wait >= 0) {
            ts.tv_sec  = wait / 1000;
            ts.tv_nsec = (long)(wait % 1000) * 1000000L;
            tsp = &ts;
        }
        int pr = ppoll(&pfd, 1, tsp, &orig);
        int err = errno;
        sigprocmask(SIG_SETMASK, &orig, NULL);
        if (pr < 0 && err == EINTR) continue;      /* the flag says why */
        if (pr < 0) return -1;
        if (pr == 0) {
            if (pending) {
                if (tui_decoder_timeout(d, ev)) return 1;
                continue;
            }
            return 0;
        }
        unsigned char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (tui_decoder_timeout(d, ev)) return 1;
            return -1;
        }
        tui_decoder_feed(d, (const char *)&c, 1);
    }
}
