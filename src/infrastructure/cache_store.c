#include "cache_store.h"
#include "fs_util.h"
#include "raii.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

char *cache_load(const char *folder, int uid) {
    RAII_STRING char *path = cache_path(folder, uid);
    if (!path) return NULL;

    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) return NULL;
    long size = ftell(fp);
    if (size <= 0) return NULL;
    rewind(fp);

    char *buf = malloc((size_t)size + 1);
    if (!buf) return NULL;

    if ((long)fread(buf, 1, (size_t)size, fp) != size) {
        free(buf);
        return NULL;
    }

    buf[size] = '\0';
    return buf;
}
