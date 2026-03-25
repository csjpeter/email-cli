#include "config_store.h"
#include "fs_util.h"
#include "raii.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CONFIG_REL_DIR ".config/email-cli"
#define CONFIG_FILE "config.ini"

static char* trim(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static char* get_config_path() {
    const char *home = fs_get_home_dir();
    if (!home) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/%s/%s", home, CONFIG_REL_DIR, CONFIG_FILE) == -1) {
        return NULL;
    }
    return path;
}

Config* config_load_from_store(void) {
    RAII_STRING char *path = get_config_path();
    if (!path) return NULL;

    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *key = strtok(line, "=");
        char *val = strtok(NULL, "\n");
        if (key && val) {
            key = trim(key);
            val = trim(val);
            if (strcmp(key, "EMAIL_HOST") == 0) cfg->host = strdup(val);
            else if (strcmp(key, "EMAIL_USER") == 0) cfg->user = strdup(val);
            else if (strcmp(key, "EMAIL_PASS") == 0) cfg->pass = strdup(val);
            else if (strcmp(key, "EMAIL_FOLDER") == 0) cfg->folder = strdup(val);
        }
    }

    // Set default folder if missing
    if (!cfg->folder) cfg->folder = strdup("INBOX");

    if (!cfg->host || !cfg->user || !cfg->pass) {
        logger_log(LOG_WARN, "Config found but incomplete.");
        config_free(cfg);
        return NULL;
    }

    return cfg;
}

int config_save_to_store(const Config *cfg) {
    const char *home = fs_get_home_dir();
    if (!home) return -1;

    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", home, CONFIG_REL_DIR);
    
    // Create directory with 0700
    if (fs_mkdir_p(dir_path, 0700) != 0) {
        logger_log(LOG_ERROR, "Failed to create config directory %s", dir_path);
        return -1;
    }

    RAII_STRING char *path = get_config_path();
    // Use raw fopen to ensure we set 0600 on new file creation
    // But fopen doesn't take mode for new file, so we'll use open() or chmod after
    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) {
        logger_log(LOG_ERROR, "Failed to open config file for writing: %s", path);
        return -1;
    }
    
    // Set 0600 immediately
    if (fs_ensure_permissions(path, 0600) != 0) {
        logger_log(LOG_WARN, "Failed to set strict permissions on %s", path);
    }

    fprintf(fp, "EMAIL_HOST=%s\n", cfg->host);
    fprintf(fp, "EMAIL_USER=%s\n", cfg->user);
    fprintf(fp, "EMAIL_PASS=%s\n", cfg->pass);
    fprintf(fp, "EMAIL_FOLDER=%s\n", cfg->folder ? cfg->folder : "INBOX");

    logger_log(LOG_INFO, "Config saved to %s", path);
    return 0;
}

void config_free(Config *cfg) {
    if (!cfg) return;
    free(cfg->host);
    free(cfg->user);
    free(cfg->pass);
    free(cfg->folder);
    free(cfg);
}
