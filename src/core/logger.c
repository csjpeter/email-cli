#include "logger.h"
#include "raii.h"
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#define MAX_LOG_SIZE (5 * 1024 * 1024) // 5MB
#define MAX_ROTATED_LOGS 5

static FILE *g_log_fp = NULL;
static LogLevel g_log_level = LOG_INFO;
static char *g_log_path = NULL;
static int g_log_stderr = 1;

/** @brief Converts a LogLevel enum to its string representation. */
static const char* level_to_str(LogLevel level) {
    switch (level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        default:        return "UNKNOWN";
    }
}

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

int logger_init(const char *log_file_path, LogLevel level) {
    g_log_level = level;
    g_log_path = strdup(log_file_path);

    // Check size and rotate if necessary
    struct stat st;
    if (stat(g_log_path, &st) == 0 && st.st_size > MAX_LOG_SIZE) {
        rotate_logs();
    }

    g_log_fp = fopen(g_log_path, "a");
    if (!g_log_fp) {
        free(g_log_path);
        g_log_path = NULL;
        return -1;
    }

    logger_log(LOG_INFO, "Logging initialized. Level: %s", level_to_str(level));
    return 0;
}

void logger_log(LogLevel level, const char *format, ...) {
    if (level < g_log_level || !g_log_fp) return;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[26];
    strftime(time_str, 26, "%Y-%m-%d %H:%M:%S", tm_info);

    va_list args;
    
    // Log to file
    fprintf(g_log_fp, "[%s] [%s] ", time_str, level_to_str(level));
    va_start(args, format);
    vfprintf(g_log_fp, format, args);
    va_end(args);
    fprintf(g_log_fp, "\n");
    fflush(g_log_fp);

    // Also log to stderr if ERROR and enabled
    if (level == LOG_ERROR && g_log_stderr) {
        fprintf(stderr, "ERROR: ");
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        fprintf(stderr, "\n");
    }
}

void logger_close(void) {
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
