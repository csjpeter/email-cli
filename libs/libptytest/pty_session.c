/**
 * @file pty_session.c
 * @brief PTY session lifecycle, input, and screen inspection.
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "pty_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

/* ── Lifecycle ───────────────────────────────────────────────────────── */

PtySession *pty_open(int cols, int rows) {
    PtySession *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->master_fd = -1;
    s->child_pid = -1;
    s->cols = cols;
    s->rows = rows;
    s->screen = pty_screen_new(cols, rows);
    if (!s->screen) { free(s); return NULL; }
    return s;
}

int pty_run(PtySession *s, const char *argv[]) {
    struct winsize ws = {
        .ws_row = (unsigned short)s->rows,
        .ws_col = (unsigned short)s->cols
    };

    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child process */
        setenv("TERM", "xterm-256color", 1);
        setenv("LC_ALL", "en_US.UTF-8", 1);
        /* Remove COLUMNS/LINES so the program queries the PTY */
        unsetenv("COLUMNS");
        unsetenv("LINES");
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent */
    s->master_fd = master_fd;
    s->child_pid = pid;

    /* Set master fd to non-blocking for poll-based reads */
    int flags = fcntl(master_fd, F_GETFL);
    if (flags >= 0) fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    return 0;
}

void pty_close(PtySession *s) {
    if (!s) return;

    if (s->child_pid > 0) {
        kill(s->child_pid, SIGTERM);
        /* Give the child 100ms to exit */
        usleep(100000);
        int status;
        if (waitpid(s->child_pid, &status, WNOHANG) == 0) {
            kill(s->child_pid, SIGKILL);
            waitpid(s->child_pid, &status, 0);
        }
    }

    if (s->master_fd >= 0) close(s->master_fd);
    pty_screen_free(s->screen);
    free(s);
}

/* ── Input ───────────────────────────────────────────────────────────── */

void pty_send(PtySession *s, const char *bytes, size_t len) {
    if (!s || s->master_fd < 0) return;
    ssize_t n = write(s->master_fd, bytes, len);
    (void)n;
}

void pty_send_key(PtySession *s, PtyKey key) {
    switch (key) {
    case PTY_KEY_UP:     pty_send(s, "\033[A", 3); break;
    case PTY_KEY_DOWN:   pty_send(s, "\033[B", 3); break;
    case PTY_KEY_RIGHT:  pty_send(s, "\033[C", 3); break;
    case PTY_KEY_LEFT:   pty_send(s, "\033[D", 3); break;
    case PTY_KEY_PGUP:   pty_send(s, "\033[5~", 4); break;
    case PTY_KEY_PGDN:   pty_send(s, "\033[6~", 4); break;
    case PTY_KEY_HOME:   pty_send(s, "\033[H", 3); break;
    case PTY_KEY_END:    pty_send(s, "\033[F", 3); break;
    default: {
        char c = (char)key;
        pty_send(s, &c, 1);
    }
    }
}

void pty_send_str(PtySession *s, const char *str) {
    if (str) pty_send(s, str, strlen(str));
}

/* ── Screen inspection ───────────────────────────────────────────────── */

const char *pty_cell_text(PtySession *s, int row, int col) {
    if (!s || !s->screen) return "";
    if (row < 0 || row >= s->rows || col < 0 || col >= s->cols) return "";
    return s->screen->cells[row * s->cols + col].ch;
}

int pty_cell_attr(PtySession *s, int row, int col) {
    if (!s || !s->screen) return 0;
    if (row < 0 || row >= s->rows || col < 0 || col >= s->cols) return 0;
    return s->screen->cells[row * s->cols + col].attr;
}

char *pty_row_text(PtySession *s, int row, char *buf, size_t size) {
    if (!s || !s->screen || !buf || size == 0) { if (buf) buf[0] = '\0'; return buf; }
    if (row < 0 || row >= s->rows) { buf[0] = '\0'; return buf; }

    size_t pos = 0;
    for (int c = 0; c < s->cols && pos + 4 < size; c++) {
        const char *ch = s->screen->cells[row * s->cols + c].ch;
        size_t len = strlen(ch);
        if (pos + len < size) {
            memcpy(buf + pos, ch, len);
            pos += len;
        }
    }
    buf[pos] = '\0';

    /* Trim trailing spaces */
    while (pos > 0 && buf[pos - 1] == ' ') buf[--pos] = '\0';

    return buf;
}

int pty_row_contains(PtySession *s, int row, const char *text) {
    char buf[4096];
    pty_row_text(s, row, buf, sizeof(buf));
    return strstr(buf, text) != NULL;
}

int pty_screen_contains(PtySession *s, const char *text) {
    if (!s || !s->screen) return 0;
    for (int r = 0; r < s->rows; r++) {
        if (pty_row_contains(s, r, text)) return 1;
    }
    return 0;
}

void pty_get_size(PtySession *s, int *cols, int *rows) {
    if (cols) *cols = s ? s->cols : 0;
    if (rows) *rows = s ? s->rows : 0;
}
