/**
 * @file pty_sync.c
 * @brief PTY output reading and synchronisation (poll + timeout).
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "pty_internal.h"
#include <poll.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

/* ── Time helpers ────────────────────────────────────────────────────── */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ── Read and feed ───────────────────────────────────────────────────── */

int g_trace_active = 0;
void pty_trace_enable(int on) { g_trace_active = on; }

/** @brief Reads available data from master fd and feeds to screen. */
static int read_and_feed(PtySession *s) {
    char buf[8192];
    ssize_t n = read(s->master_fd, buf, sizeof(buf));
    if (n > 0) {
        if (g_trace_active) {
            printf("  [TRACE %zd bytes]:", n);
            for (ssize_t i = 0; i < n && i < 120; i++) {
                unsigned char c = (unsigned char)buf[i];
                if (c == 0x1B) printf(" ESC");
                else if (c < 0x20 || c > 0x7E) printf(" %02X", c);
                else printf(" %c", c);
            }
            printf("\n"); fflush(stdout);
        }
        pty_screen_feed(s->screen, buf, (size_t)n);
        return (int)n;
    }
    return 0;
}

int pty_drain(PtySession *s) {
    if (!s || s->master_fd < 0) return 0;
    int total = 0;
    for (;;) {
        int n = read_and_feed(s);
        if (n <= 0) break;
        total += n;
    }
    return total;
}

/* ── Synchronisation ─────────────────────────────────────────────────── */

int pty_wait_for(PtySession *s, const char *text, int timeout_ms) {
    if (!s || s->master_fd < 0) return -1;

    long deadline = now_ms() + timeout_ms;
    struct pollfd pfd = { .fd = s->master_fd, .events = POLLIN };

    for (;;) {
        /* Check if text is already on screen */
        if (pty_screen_contains(s, text)) return 0;

        long remaining = deadline - now_ms();
        if (remaining <= 0) return -1; /* Timeout */

        int ret = poll(&pfd, 1, (int)remaining);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            read_and_feed(s);
        } else if (ret == 0) {
            return -1; /* Timeout */
        } else {
            /* POLLERR/POLLHUP — child may have exited */
            pty_drain(s);
            return pty_screen_contains(s, text) ? 0 : -1;
        }
    }
}

int pty_settle(PtySession *s, int quiet_ms) {
    if (!s || s->master_fd < 0) return -1;

    struct pollfd pfd = { .fd = s->master_fd, .events = POLLIN };

    for (;;) {
        int ret = poll(&pfd, 1, quiet_ms);
        if (ret == 0) return 0; /* Quiet period elapsed — settled */
        if (ret > 0 && (pfd.revents & POLLIN)) {
            if (read_and_feed(s) <= 0) return -1; /* EOF — child exited */
        } else {
            return -1; /* Error */
        }
    }
}
