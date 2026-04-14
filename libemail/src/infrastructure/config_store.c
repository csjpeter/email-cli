#include "config_store.h"
#include "config.h"
#include "fs_util.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CONFIG_APP_DIR "email-cli"
#define CONFIG_FILE "config.ini"

/** @brief Trims leading and trailing whitespace from a string in-place. */
static char* trim(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

/** @brief Returns a heap-allocated path to the config file. Caller must free. */
static char* get_config_path(void) {
    const char *config_base = platform_config_dir();
    if (!config_base) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/%s/%s", config_base, CONFIG_APP_DIR, CONFIG_FILE) == -1) {
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
            else if (strcmp(key, "EMAIL_SENT_FOLDER") == 0) cfg->sent_folder = strdup(val);
            else if (strcmp(key, "SSL_NO_VERIFY") == 0) cfg->ssl_no_verify = atoi(val);
            else if (strcmp(key, "SYNC_INTERVAL") == 0) cfg->sync_interval = atoi(val);
            else if (strcmp(key, "SMTP_HOST") == 0) cfg->smtp_host = strdup(val);
            else if (strcmp(key, "SMTP_PORT") == 0) cfg->smtp_port = atoi(val);
            else if (strcmp(key, "SMTP_USER") == 0) cfg->smtp_user = strdup(val);
            else if (strcmp(key, "SMTP_PASS") == 0) cfg->smtp_pass = strdup(val);
        }
    }

    // Set default folder if missing
    if (!cfg->folder) cfg->folder = strdup("INBOX");

    if (!cfg->host || !cfg->user || !cfg->pass) {
        logger_log(LOG_WARN, "Config found but incomplete.");
        config_free(cfg);
        return NULL;
    }

    if (strncmp(cfg->host, "imap://", 7) != 0 &&
        strncmp(cfg->host, "imaps://", 8) != 0) {
        fprintf(stderr,
            "Error: EMAIL_HOST must start with imap:// or imaps://\n"
            "  Got:      %s\n"
            "  Example:  imaps://imap.example.com\n\n"
            "Delete ~/.config/email-cli/config.ini and run again to reconfigure.\n",
            cfg->host);
        logger_log(LOG_ERROR, "Invalid EMAIL_HOST (missing protocol): %s", cfg->host);
        config_free(cfg);
        return NULL;
    }

    return cfg;
}

int config_save_to_store(const Config *cfg) {
    const char *config_base = platform_config_dir();
    if (!config_base) return -1;

    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", config_base, CONFIG_APP_DIR);

    /* Create directory with 0700 */
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
    if (cfg->sent_folder) fprintf(fp, "EMAIL_SENT_FOLDER=%s\n", cfg->sent_folder);
    if (cfg->ssl_no_verify)
        fprintf(fp, "SSL_NO_VERIFY=1\n");
    fprintf(fp, "SYNC_INTERVAL=%d\n", cfg->sync_interval);
    if (cfg->smtp_host) fprintf(fp, "SMTP_HOST=%s\n", cfg->smtp_host);
    if (cfg->smtp_port) fprintf(fp, "SMTP_PORT=%d\n", cfg->smtp_port);
    if (cfg->smtp_user) fprintf(fp, "SMTP_USER=%s\n", cfg->smtp_user);
    if (cfg->smtp_pass) fprintf(fp, "SMTP_PASS=%s\n", cfg->smtp_pass);

    logger_log(LOG_INFO, "Config saved to %s", path);
    return 0;
}

