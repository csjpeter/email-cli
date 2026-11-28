/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-11-28 */
/* Unit tests of the condition waits of the C tests (tests/common/test_scratch.c). */
#include "test_helpers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long wh_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wh_calls;
static int wh_true_on_third(void *arg) { (void)arg; return ++wh_calls >= 3; }
static int wh_never(void *arg) { (void)arg; wh_calls++; return 0; }

/* The wait ends the moment the condition holds, not at its limit. */
static void test_wait_until_ends_when_it_holds(void) {
    wh_calls = 0;
    long long t0 = wh_now_ms();
    int rc = test_wait_until(wh_true_on_third, NULL, 10000);
    long long took = wh_now_ms() - t0;
    char msg[96];
    snprintf(msg, sizeof(msg), "wait_until: ends early (took %lld ms of 10000)", took);
    ASSERT(rc == 0, "wait_until: 0 when the condition holds");
    ASSERT(wh_calls == 3, "wait_until: checks until it holds, then stops");
    ASSERT(took < 2000, msg);
}

/* A condition that never holds gives -1 after the limit, not before. */
static void test_wait_until_times_out(void) {
    wh_calls = 0;
    long long t0 = wh_now_ms();
    int rc = test_wait_until(wh_never, NULL, 200);
    long long took = wh_now_ms() - t0;
    char msg[96];
    snprintf(msg, sizeof(msg), "wait_until: the limit is kept (took %lld ms of 200)", took);
    ASSERT(rc == -1, "wait_until: -1 on timeout");
    ASSERT(took >= 200, msg);
    ASSERT(wh_calls >= 2, "wait_until: polls more than once within the limit");
}

/* A child that exits by itself is reaped, with its status. */
static void test_wait_child_exited(void) {
    pid_t pid = fork();
    if (pid == 0) _exit(7);
    int st = 0;
    ASSERT(test_wait_child(pid, &st, 5000) == 0, "wait_child: 0 for a child that exited");
    ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 7, "wait_child: the status is returned");
    ASSERT(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD, "wait_child: the child is reaped");
    ASSERT(test_wait_child(-1, NULL, 10) == -1, "wait_child: -1 for no pid");
}

/* A child still running at the limit is killed and reaped. */
static void test_wait_child_killed(void) {
    pid_t pid = fork();
    if (pid == 0) { for (;;) pause(); }
    int st = 0;
    ASSERT(test_wait_child(pid, &st, 100) == 1, "wait_child: 1 when the child had to be killed");
    ASSERT(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "wait_child: killed by SIGKILL");
    ASSERT(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD, "wait_child: the killed child is reaped");
}

/* A port is waited for until something listens on it. */
static void test_wh_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    ASSERT(fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
           getsockname(fd, (struct sockaddr *)&a, &len) == 0, "wait_port: a socket is bound");
    int port = ntohs(a.sin_port);
    ASSERT(test_wait_port(port, 100) == -1, "wait_port: -1 while nothing listens");
    ASSERT(listen(fd, 4) == 0, "wait_port: listen");
    ASSERT(test_wait_port(port, 5000) == 0, "wait_port: 0 once something listens");
    close(fd);
}

/* A file is waited for until it exists. */
static void test_wh_file(void) {
    const char *f = test_path("wait-file");
    unlink(f);
    ASSERT(test_wait_file(f, 50) == -1, "wait_file: -1 while the file is missing");
    pid_t pid = fork();
    if (pid == 0) {
        FILE *fp = fopen(f, "w");
        if (fp) fclose(fp);
        _exit(0);
    }
    ASSERT(test_wait_file(f, 5000) == 0, "wait_file: 0 once another process made it");
    test_wait_child(pid, NULL, 5000);
    unlink(f);
}

/* A child sees its parent sleep once the parent blocks in read(). */
static void test_wh_sleeping(void) {
    int to_child[2], from_child[2];
    ASSERT(pipe(to_child) == 0 && pipe(from_child) == 0, "wait_sleeping: pipes");
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        close(to_child[1]);
        close(from_child[0]);
        int ok = test_wait_sleeping(parent, 5000) == 0;
        char c = ok ? 'y' : 'n';
        if (write(from_child[1], &c, 1) != 1) _exit(2);
        _exit(0);
    }
    close(to_child[0]);
    close(from_child[1]);
    char c = 0;
    ssize_t n = read(from_child[0], &c, 1);   /* blocks: the child sees us sleep */
    ASSERT(n == 1 && c == 'y', "wait_sleeping: the child saw the parent block in read()");
    ASSERT(test_wait_child(pid, NULL, 5000) == 0, "wait_sleeping: the child exited");
    close(to_child[1]);
    close(from_child[0]);
    ASSERT(test_wait_sleeping(999999999, 50) == -1, "wait_sleeping: -1 for no such process");
}

/* A reaped child counts as exited even when a SIGCHLD handler took it. */
static int wh_reaped;
static void wh_reaper(int sig) {
    (void)sig;
    int st;
    while (waitpid(-1, &st, WNOHANG) > 0) wh_reaped++;
}

static void test_wait_child_reaped_by_handler(void) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = wh_reaper;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, &old);
    wh_reaped = 0;
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    ASSERT(test_wait_child(pid, NULL, 5000) == 0,
           "wait_child: 0 for a child a SIGCHLD handler reaped");
    sigaction(SIGCHLD, &old, NULL);
}

void test_wait_helpers(void) {
    /* Earlier suites leave a reaping SIGCHLD handler behind (email_service):
     * the status tests need the default, the handler case is tested below. */
    struct sigaction dfl, old;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGCHLD, &dfl, &old);
    RUN_TEST(test_wait_until_ends_when_it_holds);
    RUN_TEST(test_wait_until_times_out);
    RUN_TEST(test_wait_child_exited);
    RUN_TEST(test_wait_child_killed);
    RUN_TEST(test_wh_port);
    RUN_TEST(test_wh_file);
    RUN_TEST(test_wh_sleeping);
    RUN_TEST(test_wait_child_reaped_by_handler);
    sigaction(SIGCHLD, &old, NULL);
}
