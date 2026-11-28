/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-11 */
/* Per-process scratch directory of the C test suites, under build/. */
#include "test_scratch.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef TEST_BUILD_DIR
#error "TEST_BUILD_DIR must be defined by the build system"
#endif
#ifndef TEST_CATEGORY
#error "TEST_CATEGORY must be defined by the build system"
#endif

#define MAX_PATHS 1024

static char  g_root[PATH_MAX];
static pid_t g_owner;
static char *g_paths[MAX_PATHS];
static int   g_npaths;

static int mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int nftw_cb(const char *path, const struct stat *sb, int type,
                   struct FTW *ftw)
{
    (void)sb; (void)ftw;
    if (type == FTW_D || type == FTW_DNR || type == FTW_DP) {
        (void)chmod(path, 0700);
        return rmdir(path) != 0 && errno != ENOENT ? 1 : 0;
    }
    return unlink(path) != 0 && errno != ENOENT ? 1 : 0;
}

int test_rmtree(const char *path)
{
    size_t rl = strlen(g_root);
    if (rl == 0 || strncmp(path, g_root, rl) != 0 ||
        (path[rl] != '/' && path[rl] != '\0') || strstr(path, "..") != NULL) {
        fprintf(stderr, "test_rmtree: refusing unsafe path '%s'\n", path);
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    (void)chmod(path, 0700);
    return nftw(path, nftw_cb, 32, FTW_DEPTH | FTW_PHYS);
}

static int g_filter_selected;
static int g_filter_set;
static pid_t g_filter_owner;

/* Runs in forked children too: only the process that made the directory
 * removes it, and only the process that read the filter judges it. */
static void at_exit_handler(void)
{
    if (g_owner == getpid() && g_root[0] != '\0') (void)test_rmtree(g_root);
    if (g_filter_set && g_filter_owner == getpid() && g_filter_selected == 0) {
        fprintf(stderr, "TEST_FILTER='%s' matched no test\n",
                getenv("TEST_FILTER"));
        _exit(EXIT_FAILURE);
    }
}

static void register_at_exit(void)
{
    static int done;
    if (done) return;
    done = 1;
    atexit(at_exit_handler);
}

const char *test_scratch_dir(void)
{
    if (g_root[0] == '\0') {
        snprintf(g_root, sizeof(g_root), "%s/tests/%s/work-%d",
                 TEST_BUILD_DIR, TEST_CATEGORY, (int)getpid());
        g_owner = getpid();
        if (mkdir_p(g_root) != 0) {
            fprintf(stderr, "test_scratch: cannot create '%s': %s\n",
                    g_root, strerror(errno));
            exit(EXIT_FAILURE);
        }
        register_at_exit();
    }
    return g_root;
}

const char *test_path(const char *rel)
{
    const char *root = test_scratch_dir();
    char full[PATH_MAX];
    if (rel[0] == '\0') snprintf(full, sizeof(full), "%s", root);
    else snprintf(full, sizeof(full), "%s/%s", root, rel);
    for (int i = 0; i < g_npaths; i++)
        if (strcmp(g_paths[i], full) == 0) return g_paths[i];
    if (g_npaths >= MAX_PATHS) {
        fprintf(stderr, "test_path: too many distinct paths\n");
        exit(EXIT_FAILURE);
    }
    g_paths[g_npaths] = strdup(full);
    if (!g_paths[g_npaths]) exit(EXIT_FAILURE);
    return g_paths[g_npaths++];
}

char *test_mkdtemp(char *buf, size_t n, const char *prefix)
{
    snprintf(buf, n, "%s/%s-XXXXXX", test_scratch_dir(), prefix);
    if (!mkdtemp(buf)) {
        fprintf(stderr, "test_mkdtemp: '%s': %s\n", buf, strerror(errno));
        return NULL;
    }
    return buf;
}

void test_set_home(const char *home)
{
    char p[PATH_MAX];
    setenv("HOME", home, 1);
    snprintf(p, sizeof(p), "%s/.config", home);
    setenv("XDG_CONFIG_HOME", p, 1);
    snprintf(p, sizeof(p), "%s/.local/share", home);
    setenv("XDG_DATA_HOME", p, 1);
    snprintf(p, sizeof(p), "%s/.cache", home);
    setenv("XDG_CACHE_HOME", p, 1);
}

static int read_file(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = '\0';
    return 0;
}

#define MAX_MOCK_LOGS 16
static struct { pid_t pid; char path[PATH_MAX]; } g_mock_logs[MAX_MOCK_LOGS];
static int g_nmock_logs;

/* The mock has written its port, or it has exited before that. */
struct mock_up { const char *portfile; pid_t pid; int port; int exited; };

static int mock_published(void *arg)
{
    struct mock_up *m = arg;
    char buf[32];
    if (read_file(m->portfile, buf, sizeof(buf)) == 0 && (m->port = atoi(buf)) > 0)
        return 1;
    int st;
    if (waitpid(m->pid, &st, WNOHANG) == m->pid) { m->exited = 1; return 1; }
    return 0;
}

pid_t test_spawn_mock(const char *bin, const char *prefix, int want_port,
                      int *port_out)
{
    static int seq;
    char portfile[PATH_MAX], logfile[PATH_MAX], var[128], num[16];
    int n = ++seq;
    snprintf(portfile, sizeof(portfile), "%s/mock-%s-%d.port",
             test_scratch_dir(), prefix, n);
    snprintf(logfile, sizeof(logfile), "%s/mock-%s-%d.log",
             test_scratch_dir(), prefix, n);
    if (unlink(portfile) != 0 && errno != ENOENT) {
        fprintf(stderr, "test_spawn_mock: %s: %s\n", portfile, strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        snprintf(var, sizeof(var), "%s_PORT_FILE", prefix);
        setenv(var, portfile, 1);
        snprintf(var, sizeof(var), "%s_PORT", prefix);
        if (want_port > 0) {
            snprintf(num, sizeof(num), "%d", want_port);
            setenv(var, num, 1);
        } else {
            unsetenv(var);
        }
        int lfd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (lfd < 0) _exit(126);
        dup2(lfd, 1);
        dup2(lfd, 2);
        close(lfd);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }

    struct mock_up m = { portfile, pid, 0, 0 };
    if (test_wait_until(mock_published, &m, 10000) == 0) {
        if (m.exited) {
            char log[2048] = "";
            (void)read_file(logfile, log, sizeof(log));
            fprintf(stderr, "test_spawn_mock: %s exited before it listened:\n%s\n",
                    bin, log);
            return -1;
        }
        if (port_out) *port_out = m.port;
        int slot = g_nmock_logs % MAX_MOCK_LOGS;
        g_mock_logs[slot].pid = pid;
        snprintf(g_mock_logs[slot].path, PATH_MAX, "%s", logfile);
        g_nmock_logs++;
        return pid;
    }
    fprintf(stderr, "test_spawn_mock: %s did not publish a port in 10 s\n", bin);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

const char *test_mock_log(pid_t pid)
{
    for (int i = 0; i < MAX_MOCK_LOGS && i < g_nmock_logs; i++)
        if (g_mock_logs[i].pid == pid) return g_mock_logs[i].path;
    return NULL;
}

int test_log_count(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof(line), fp))
        if (strstr(line, needle)) n++;
    fclose(fp);
    return n;
}

struct log_want { const char *path, *needle; int want; };

static int log_reached(void *arg)
{
    const struct log_want *w = arg;
    return test_log_count(w->path, w->needle) >= w->want;
}

int test_log_wait(const char *path, const char *needle, int want, int ms)
{
    struct log_want w = { path, needle, want };
    return test_wait_until(log_reached, &w, ms);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int test_wait_until(int (*cond)(void *arg), void *arg, int ms)
{
    long long end = now_ms() + ms;
    for (;;) {
        if (cond(arg)) return 0;
        if (now_ms() >= end) return -1;
        struct timespec ts = {0, TEST_POLL_MS * 1000L * 1000L};
        nanosleep(&ts, NULL);   /* the poll interval, not a guess at a duration */
    }
}

struct child_want { pid_t pid; int *status; int gone; };

static int child_exited(void *arg)
{
    struct child_want *c = arg;
    int st = 0;
    pid_t r = waitpid(c->pid, &st, WNOHANG);
    if (r == 0) return 0;
    if (r == c->pid && c->status) *c->status = st;
    if (r < 0) c->gone = 1;
    return 1;
}

int test_wait_child(pid_t pid, int *status, int ms)
{
    if (pid <= 0) return -1;
    struct child_want c = { pid, status, 0 };
    if (test_wait_until(child_exited, &c, ms) == 0) return c.gone ? -1 : 0;
    fprintf(stderr, "test_wait_child: pid %d still runs after %d ms: killed\n",
            (int)pid, ms);
    kill(pid, SIGKILL);
    int st = 0;
    if (waitpid(pid, &st, 0) == pid && status) *status = st;
    return 1;
}

static int port_accepts(void *arg)
{
    int port = *(const int *)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in a = {0};
    a.sin_family      = AF_INET;
    a.sin_port        = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return ok;
}

int test_wait_port(int port, int ms)
{
    return test_wait_until(port_accepts, &port, ms);
}

static int file_exists(void *arg)
{
    return access((const char *)arg, F_OK) == 0;
}

int test_wait_file(const char *path, int ms)
{
    return test_wait_until(file_exists, (void *)path, ms);
}

static int proc_sleeps(void *arg)
{
    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)*(const pid_t *)arg);
    if (read_file(path, buf, sizeof(buf)) != 0) return 0;
    const char *rp = strrchr(buf, ')');          /* the name may hold spaces */
    return rp && rp[1] == ' ' && rp[2] == 'S';
}

int test_wait_sleeping(pid_t pid, int ms)
{
    return test_wait_until(proc_sleeps, &pid, ms);
}

int test_unused_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(a);
    int port = -1;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &len) == 0)
        port = ntohs(a.sin_port);
    close(fd);
    return port;
}

int test_selected(const char *name)
{
    const char *f = getenv("TEST_FILTER");
    if (!f || !f[0]) return 1;
    if (!g_filter_set) {
        g_filter_set = 1;
        g_filter_owner = getpid();
        register_at_exit();
    }
    if (!strstr(name, f)) return 0;
    g_filter_selected++;
    return 1;
}
