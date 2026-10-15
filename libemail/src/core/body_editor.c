/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* The full-screen message body editor: libtuikit's tui_textedit with autosave. */
#include "body_editor.h"

#include <tuikit/buf.h>
#include <tuikit/canvas.h>
#include <tuikit/event.h>
#include <tuikit/screen.h>
#include <tuikit/textedit.h>
#include <tuikit/tty.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BODY_MAX (1024 * 1024)

/* Below this the editor shows only a notice (tui-interface-model). */
#define MIN_COLS 80
#define MIN_ROWS 24

static void draw(tui_textedit *te, const char *header, const char *note,
                 int rows, int cols) {
    tui_screen *scr = tui_screen_new(rows, cols);
    if (!scr) return;
    tui_canvas c = tui_canvas_of(scr);
    if (rows < MIN_ROWS || cols < MIN_COLS) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Terminal too small (%dx%d).", cols, rows);
        tui_canvas_put(c, 0, 0, msg, TUI_STYLE_DEFAULT);
        snprintf(msg, sizeof(msg), "Needs %dx%d. Resize; Esc saves and leaves.",
                 MIN_COLS, MIN_ROWS);
        if (rows > 1) tui_canvas_put(c, 1, 0, msg, TUI_STYLE_DEFAULT);
        tui_buf out;
        tui_buf_init(&out);
        if (tui_screen_render(scr, &out) >= 0) {
            fflush(stdout);
            tui_buf_write_fd(&out, STDOUT_FILENO);
        }
        tui_buf_free(&out);
        tui_screen_free(scr);
        return;
    }
    tui_canvas_fill(c, tui_rect_make(0, 0, 1, cols), TUI_STYLE_HEADER);
    tui_canvas_put(c, 0, 0, header ? header : "  Message body", TUI_STYLE_HEADER);
    if (rows > 3) {
        tui_canvas ec = tui_canvas_sub(c, tui_rect_make(1, 0, rows - 2, cols));
        tui_textedit_render(te, ec, TUI_STYLE_DEFAULT);
    }
    char status[160];
    snprintf(status, sizeof(status), "  Esc=save and continue  Ctrl-S=save   %s", note);
    tui_canvas_put(c, rows - 1, 0, status, tui_style_attrs(TUI_ATTR_DIM));

    tui_buf out;
    tui_buf_init(&out);
    if (tui_screen_render(scr, &out) >= 0) {
        fflush(stdout);
        tui_buf_write_fd(&out, STDOUT_FILENO);
    }
    tui_buf_free(&out);
    tui_screen_free(scr);
}

/* A resize while the editor is open (EMAIL-97): SIGWINCH sets the flag and
 * interrupts the wait; the editor redraws at the new size at once.  The
 * handler of the program is put back when the editor ends. */
static volatile sig_atomic_t g_resized;

static void on_winch(int sig) {
    (void)sig;
    g_resized = 1;
}

/* One event from @p fd through the persistent decoder @p d, so that a
 * bracketed paste arrives whole (tui_tty_read_key() is one-shot and drops
 * the text of a paste).  @p idle_ms < 0 blocks; otherwise 0 is returned when
 * that long passes with no input.  Returns 1 event, 0 idle, -1 hang-up. */
static int next_event(int fd, tui_decoder *d, tui_event *ev, int idle_ms) {
    sigset_t block, orig;
    sigemptyset(&block);
    sigaddset(&block, SIGWINCH);
    for (;;) {
        if (tui_decoder_next(d, ev)) return 1;
        int pending = tui_decoder_pending(d);
        /* Wait for input, a resize, or the idle time; SIGWINCH is blocked
         * outside ppoll so that a resize cannot slip between the check and
         * the wait. */
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

int body_editor_run(char **body, const char *header, BodyEditorSave save, void *ctx) {
    tui_tty_raw *raw = tui_tty_raw_enter(STDIN_FILENO, 0);
    if (!raw) return -1;
    char *buf = malloc(BODY_MAX);
    if (!buf) { tui_tty_raw_exit(&raw); return -1; }
    tui_textedit te;
    tui_textedit_init(&te, buf, BODY_MAX, *body ? *body : "");

    static const char on[]  = "\033[?2004h";
    static const char off[] = "\033[?2004l";
    ssize_t w = write(STDOUT_FILENO, on, sizeof(on) - 1);
    (void)w;

    tui_decoder dec;
    tui_decoder_init(&dec);
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    g_resized = 0;
    sigaction(SIGWINCH, &sa, &old_sa);
    const char *note = "";
    int saved_dirty = 0;
    for (;;) {
        int rows = tui_tty_rows(STDOUT_FILENO), cols = tui_tty_cols(STDOUT_FILENO);
        if (rows <= 0) rows = 24;
        if (cols <= 0) cols = 80;
        draw(&te, header, note, rows, cols);
        note = "";

        /* Idle with unsaved changes: save instead of waiting for a key. */
        tui_event ev;
        int want_idle = (te.dirty && !saved_dirty) ? BODY_EDITOR_AUTOSAVE_MS : -1;
        int rc = next_event(STDIN_FILENO, &dec, &ev, want_idle);
        if (rc == 0) {
            if (save) save(te.buf, ctx);
            saved_dirty = 1;
            note = "autosaved";
            continue;
        }
        if (rc < 0 || ev.type == TUI_EV_HANGUP) break;
        if (ev.type == TUI_EV_RESIZE) continue;
        if (ev.type == TUI_EV_KEY && ev.key == TUI_KEY_ESC) break;
        if (rows < MIN_ROWS || cols < MIN_COLS) continue;   /* nothing is drawn: no edits */
        if (TUI_EV_IS_CTRL(&ev, 's')) {
            if (save) save(te.buf, ctx);
            saved_dirty = 1;
            note = "saved";
            continue;
        }
        if (tui_textedit_handle(&te, &ev)) saved_dirty = 0;
    }

    if (save) save(te.buf, ctx);
    free(*body);
    *body = strdup(te.buf);
    w = write(STDOUT_FILENO, off, sizeof(off) - 1);
    (void)w;
    sigaction(SIGWINCH, &old_sa, NULL);
    tui_decoder_free(&dec);
    free(buf);
    tui_tty_raw_exit(&raw);
    return *body ? 0 : -1;
}
