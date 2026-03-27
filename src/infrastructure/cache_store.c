#include "cache_store.h"
#include "fs_util.h"
#include "raii.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/** @brief Returns a heap-allocated path for a cached message file. Caller must free. */
static char *cache_path(const char *folder, int uid) {
    const char *home = fs_get_home_dir();
    if (!home) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/.cache/email-cli/messages/%s/%d.eml",
                 home, folder, uid) == -1)
        return NULL;
    return path;
}

int cache_exists(const char *folder, int uid) {
    RAII_STRING char *path = cache_path(folder, uid);
    if (!path) return 0;
    RAII_FILE FILE *fp = fopen(path, "r");
    return fp != NULL;
}

int cache_save(const char *folder, int uid, const char *content, size_t len) {
    const char *home = fs_get_home_dir();
    if (!home) return -1;

    RAII_STRING char *dir = NULL;
    if (asprintf(&dir, "%s/.cache/email-cli/messages/%s", home, folder) == -1)
        return -1;
    if (fs_mkdir_p(dir, 0700) != 0) {
        logger_log(LOG_ERROR, "Failed to create cache directory %s", dir);
        return -1;
    }

    RAII_STRING char *path = cache_path(folder, uid);
    if (!path) return -1;

    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) {
        logger_log(LOG_ERROR, "Failed to open cache file for writing: %s", path);
        return -1;
    }

    if (fwrite(content, 1, len, fp) != len) {
        logger_log(LOG_ERROR, "Failed to write cache file: %s", path);
        return -1;
    }

    logger_log(LOG_DEBUG, "Cached UID %d/%s to %s", uid, folder, path);
    return 0;
}

/* ── Shared file I/O helper ──────────────────────────────────────────── */

static char *load_file(const char *path) {
    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) return NULL;
    long size = ftell(fp);
    if (size <= 0) return NULL;
    rewind(fp);
    char *buf = malloc((size_t)size + 1);
    if (!buf) return NULL;
    if ((long)fread(buf, 1, (size_t)size, fp) != size) { free(buf); return NULL; }
    buf[size] = '\0';
    return buf;
}

char *cache_load(const char *folder, int uid) {
    RAII_STRING char *path = cache_path(folder, uid);
    if (!path) return NULL;
    return load_file(path);
}

/* ── Header cache ────────────────────────────────────────────────────── */

static char *hcache_path(const char *folder, int uid) {
    const char *home = fs_get_home_dir();
    if (!home) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/.cache/email-cli/headers/%s/%d.hdr",
                 home, folder, uid) == -1)
        return NULL;
    return path;
}

int hcache_exists(const char *folder, int uid) {
    RAII_STRING char *path = hcache_path(folder, uid);
    if (!path) return 0;
    RAII_FILE FILE *fp = fopen(path, "r");
    return fp != NULL;
}

int hcache_save(const char *folder, int uid, const char *content, size_t len) {
    const char *home = fs_get_home_dir();
    if (!home) return -1;
    RAII_STRING char *dir = NULL;
    if (asprintf(&dir, "%s/.cache/email-cli/headers/%s", home, folder) == -1)
        return -1;
    if (fs_mkdir_p(dir, 0700) != 0) {
        logger_log(LOG_ERROR, "Failed to create header cache dir %s", dir);
        return -1;
    }
    RAII_STRING char *path = hcache_path(folder, uid);
    if (!path) return -1;
    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    if (fwrite(content, 1, len, fp) != len) return -1;
    logger_log(LOG_DEBUG, "Cached headers UID %d/%s", uid, folder);
    return 0;
}

char *hcache_load(const char *folder, int uid) {
    RAII_STRING char *path = hcache_path(folder, uid);
    if (!path) return NULL;
    return load_file(path);
}

static int cmp_int_evict(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

void hcache_evict_stale(const char *folder,
                         const int *keep_uids, int keep_count) {
    const char *home = fs_get_home_dir();
    if (!home) return;

    RAII_STRING char *dir = NULL;
    if (asprintf(&dir, "%s/.cache/email-cli/headers/%s", home, folder) == -1)
        return;

    /* Sort a local copy for bsearch */
    int *sorted = malloc((size_t)keep_count * sizeof(int));
    if (!sorted) return;
    memcpy(sorted, keep_uids, (size_t)keep_count * sizeof(int));
    qsort(sorted, (size_t)keep_count, sizeof(int), cmp_int_evict);

    RAII_DIR DIR *d = opendir(dir);
    if (!d) { free(sorted); return; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        const char *dot  = strrchr(name, '.');
        if (!dot || strcmp(dot, ".hdr") != 0) continue;
        char *end;
        long uid = strtol(name, &end, 10);
        if (end != dot || uid <= 0) continue;
        int key = (int)uid;
        if (!bsearch(&key, sorted, (size_t)keep_count,
                     sizeof(int), cmp_int_evict)) {
            RAII_STRING char *path = NULL;
            if (asprintf(&path, "%s/%s", dir, name) != -1) {
                remove(path);
                logger_log(LOG_DEBUG,
                           "Evicted stale header: UID %ld in %s", uid, folder);
            }
        }
    }
    free(sorted);
}
