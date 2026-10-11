/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-11 */
/* Per-process scratch directory of the C test suites, under build/. */
#include "test_scratch.h"

#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* Runs in forked children too: only the process that made the directory
 * removes it. */
static void remove_at_exit(void)
{
    if (g_owner != getpid() || g_root[0] == '\0') return;
    (void)test_rmtree(g_root);
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
        atexit(remove_at_exit);
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
