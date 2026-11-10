/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-24 */
/* Traffic log (--debug only) with rotation, and the stderr error output. */
#include "logger.h"
#include "raii.h"
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>

#define MAX_LOG_SIZE (5 * 1024 * 1024) // 5MB
#define MAX_ROTATED_LOGS 5

static FILE *g_log_fp = NULL;
static char *g_log_path = NULL;
static int g_log_stderr = 1;
static int g_stderr_only = 0;   /* no file, but errors still go to stderr */
static unsigned g_stderr_errors;

/**
 * @brief Rotates log files: session.log → session.log.1, dropping session.log.5.
 */
static void rotate_logs() {
    if (!g_log_path) return;

    // session.log.5 -> deleted
    // session.log.4 -> session.log.5
    // ...
    // session.log -> session.log.1

    char old_name[1024], new_name[1024];

    // Remove the oldest log
    snprintf(old_name, sizeof(old_name), "%s.%d", g_log_path, MAX_ROTATED_LOGS);
    unlink(old_name);

    // Rotate existing logs
    for (int i = MAX_ROTATED_LOGS - 1; i >= 1; i--) {
        snprintf(old_name, sizeof(old_name), "%s.%d", g_log_path, i);
        snprintf(new_name, sizeof(new_name), "%s.%d", g_log_path, i + 1);
        rename(old_name, new_name);
    }

    // Move current log
    snprintf(new_name, sizeof(new_name), "%s.1", g_log_path);
    rename(g_log_path, new_name);
}

int logger_debug_requested(int *argc, char **argv) {
    int on = 0;
    int w = 1;
    for (int r = 1; r < *argc; r++) {
        if (strcmp(argv[r], "--debug") == 0) { on = 1; continue; }
        argv[w++] = argv[r];
    }
    *argc = w;
    argv[w] = NULL;
    return on;
}

int logger_init(const char *log_file_path) {
    /* A program and libemail-client (linked static: one copy of this state)
     * both open the log; the second call must not leak the first one's file
     * and path. */
    if (g_log_fp && g_log_path && strcmp(g_log_path, log_file_path) == 0)
        return 0;
    logger_close();
    g_log_path = strdup(log_file_path);
    if (!g_log_path) return -1;

    // Check size and rotate if necessary
    struct stat st;
    if (stat(g_log_path, &st) == 0 && st.st_size > MAX_LOG_SIZE) {
        rotate_logs();
    }

    /* The traffic log is private: 0600 (the directory is made 0700). */
    int fd = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    g_log_fp = fd >= 0 ? fdopen(fd, "a") : NULL;
    if (!g_log_fp) {
        if (fd >= 0) close(fd);
        free(g_log_path);
        g_log_path = NULL;
        return -1;
    }
    return 0;
}

int logger_traffic_enabled(void) {
    return g_log_fp != NULL;
}

/** Replaces line[from, to) with "xxxxx", keeping the rest; bounded by cap. */
static void mask_range(char *line, size_t cap, size_t from, size_t to) {
    size_t len = strlen(line);
    if (to > len) to = len;
    if (from >= to) return;
    size_t tail = len - to;
    size_t mark = 5;
    if (from + mark >= cap) mark = cap > from + 1 ? cap - from - 1 : 0;
    if (from + mark + tail >= cap) tail = cap - 1 - from - mark;
    memmove(line + from + mark, line + to, tail);
    memcpy(line + from, "xxxxx", mark);
    line[from + mark + tail] = '\0';
}

/** End of the token starting at @p p: a quoted string or up to a space. */
static size_t token_end(const char *line, size_t p) {
    if (line[p] == '"') {
        const char *q = strchr(line + p + 1, '"');
        return q ? (size_t)(q - line) + 1 : strlen(line);
    }
    while (line[p] && line[p] != ' ') p++;
    return p;
}

/** Masks the value of KEY in JSON ("key":"v") or form (key=v) text. */
static void mask_key_value(char *line, size_t cap, const char *key) {
    size_t kl = strlen(key);
    size_t from = 0;
    for (;;) {
        if (from > strlen(line)) return;
        char *hit = strcasestr(line + from, key);
        if (!hit) return;
        size_t p = (size_t)(hit - line) + kl;
        from = p;
        if (line[p] == '"') p++;                     /* closing quote of the key */
        while (line[p] == ' ') p++;
        if (line[p] != ':' && line[p] != '=') continue;
        p++;
        while (line[p] == ' ') p++;
        size_t e;
        if (line[p] == '"') {
            p++;
            e = p;
            while (line[e] && line[e] != '"') e++;
        } else {
            e = p;
            while (line[e] && line[e] != '&' && line[e] != ' ' && line[e] != ',' &&
                   line[e] != '}' && line[e] != '"') e++;
        }
        mask_range(line, cap, p, e);
        from = p + 5;
    }
}

void logger_mask_secret(char *line, size_t cap) {
    if (!line || cap < 8) return;

    /* Authorization: <scheme> <value> */
    char *ah = strcasestr(line, "Authorization:");
    if (ah) {
        size_t p = (size_t)(ah - line) + 14;
        while (line[p] == ' ') p++;
        size_t sch = p;
        while (line[sch] && line[sch] != ' ') sch++;
        if (line[sch] == ' ') mask_range(line, cap, sch + 1, strlen(line));
        else mask_range(line, cap, p, strlen(line));
    }

    /* LOGIN user pass | AUTHENTICATE mech data | AUTH mech data: the command
     * is within the first words of the line (after the direction and tag). */
    static const char *const cmds[] = { "AUTHENTICATE", "AUTH", "LOGIN" };
    size_t best = (size_t)-1, wl = 0;
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        size_t l = strlen(cmds[i]);
        const char *h = line;
        while ((h = strcasestr(h, cmds[i])) != NULL) {
            size_t pos = (size_t)(h - line);
            if (pos >= 48) break;
            int before = pos == 0 || line[pos - 1] == ' ' || line[pos - 1] == ']';
            if (before && line[pos + l] == ' ') {
                if (pos < best) { best = pos; wl = l; }
                break;
            }
            h++;
        }
    }
    if (best != (size_t)-1) {
        size_t p = best + wl + 1;
        while (line[p] == ' ') p++;
        size_t e = token_end(line, p);       /* the user, or the mechanism */
        while (line[e] == ' ') e++;
        if (line[e]) mask_range(line, cap, e, strlen(line));
    }

    mask_key_value(line, cap, "access_token");
    mask_key_value(line, cap, "refresh_token");
    mask_key_value(line, cap, "client_secret");
    mask_key_value(line, cap, "password");
}

void logger_traffic(const char *format, ...) {
    if (!g_log_fp) return;
    char buf[8192];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n < 0) return;
    logger_mask_secret(buf, sizeof(buf));

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_str[26];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_info);
    fprintf(g_log_fp, "[%s] %s\n", time_str, buf);
    fflush(g_log_fp);
}

void logger_error(const char *format, ...) {
    if (!g_log_stderr || !(g_log_fp || g_stderr_only)) return;
    g_stderr_errors++;
    va_list args;
    va_start(args, format);
    fprintf(stderr, "ERROR: ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void logger_stderr_only(void) {
    g_stderr_only = 1;
}

void logger_close(void) {
    g_stderr_only = 0;
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    if (g_log_path) {
        free(g_log_path);
        g_log_path = NULL;
    }
}

void logger_set_stderr(int enable) {
    g_log_stderr = enable;
}

int logger_clean_logs(const char *log_dir) {
    RAII_DIR DIR *dir = opendir(log_dir);
    if (!dir) return -1;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG && strstr(entry->d_name, "session.log")) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", log_dir, entry->d_name);
            unlink(path);
        }
    }
    return 0;
}

unsigned logger_stderr_error_count(void) {
    return g_stderr_errors;
}

void logger_note_stderr_error(void) {
    g_stderr_errors++;
}
