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
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

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

    /* TLS enforcement — only imaps:// / smtps:// are accepted.
     * Exception: SSL_NO_VERIFY=1 also permits imap:// and smtp:// for local
     * test environments (self-signed or no-TLS mock servers). */
    if (!cfg->ssl_no_verify) {
        if (strncmp(cfg->host, "imaps://", 8) != 0) {
            fprintf(stderr,
                "Error: EMAIL_HOST must start with imaps:// (TLS required).\n"
                "  Got:      %s\n"
                "  Example:  imaps://imap.example.com\n\n"
                "Plain imap:// is rejected to protect your password.\n"
                "For test environments only, add SSL_NO_VERIFY=1 to config.\n",
                cfg->host);
            logger_log(LOG_ERROR,
                       "Rejected insecure EMAIL_HOST (use imaps://): %s", cfg->host);
            config_free(cfg);
            return NULL;
        }
        if (cfg->smtp_host && cfg->smtp_host[0] &&
            strncmp(cfg->smtp_host, "smtps://", 8) != 0) {
            fprintf(stderr,
                "Error: SMTP_HOST must start with smtps:// (TLS required).\n"
                "  Got:      %s\n"
                "  Example:  smtps://smtp.example.com\n\n"
                "Plain smtp:// is rejected to protect your password.\n"
                "For test environments only, add SSL_NO_VERIFY=1 to config.\n",
                cfg->smtp_host);
            logger_log(LOG_ERROR,
                       "Rejected insecure SMTP_HOST (use smtps://): %s",
                       cfg->smtp_host);
            config_free(cfg);
            return NULL;
        }
    } else {
        if (strncmp(cfg->host, "imaps://", 8) != 0)
            logger_log(LOG_WARN,
                       "SSL_NO_VERIFY=1: connecting without TLS to %s "
                       "(test/dev mode only)", cfg->host);
        if (cfg->smtp_host && cfg->smtp_host[0] &&
            strncmp(cfg->smtp_host, "smtps://", 8) != 0)
            logger_log(LOG_WARN,
                       "SSL_NO_VERIFY=1: SMTP without TLS to %s "
                       "(test/dev mode only)", cfg->smtp_host);
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

/* ── Multi-account support ───────────────────────────────────────────── */

/** Returns heap-allocated path to the accounts/ directory. Caller must free. */
static char *get_accounts_dir(void) {
    const char *config_base = platform_config_dir();
    if (!config_base) return NULL;
    char *dir = NULL;
    if (asprintf(&dir, "%s/%s/accounts", config_base, CONFIG_APP_DIR) == -1)
        return NULL;
    return dir;
}

/** Write one config struct to an open FILE. */
static void write_config_to_fp(FILE *fp, const Config *cfg) {
    fprintf(fp, "EMAIL_HOST=%s\n",   cfg->host   ? cfg->host   : "");
    fprintf(fp, "EMAIL_USER=%s\n",   cfg->user   ? cfg->user   : "");
    fprintf(fp, "EMAIL_PASS=%s\n",   cfg->pass   ? cfg->pass   : "");
    fprintf(fp, "EMAIL_FOLDER=%s\n", cfg->folder ? cfg->folder : "INBOX");
    if (cfg->sent_folder) fprintf(fp, "EMAIL_SENT_FOLDER=%s\n", cfg->sent_folder);
    if (cfg->ssl_no_verify) fprintf(fp, "SSL_NO_VERIFY=1\n");
    fprintf(fp, "SYNC_INTERVAL=%d\n", cfg->sync_interval);
    if (cfg->smtp_host) fprintf(fp, "SMTP_HOST=%s\n", cfg->smtp_host);
    if (cfg->smtp_port) fprintf(fp, "SMTP_PORT=%d\n", cfg->smtp_port);
    if (cfg->smtp_user) fprintf(fp, "SMTP_USER=%s\n", cfg->smtp_user);
    if (cfg->smtp_pass) fprintf(fp, "SMTP_PASS=%s\n", cfg->smtp_pass);
}

int config_save_account(const Config *cfg) {
    if (!cfg || !cfg->user || !cfg->user[0]) return -1;

    RAII_STRING char *accounts_dir = get_accounts_dir();
    if (!accounts_dir) return -1;

    char account_dir[1024];
    snprintf(account_dir, sizeof(account_dir), "%s/%s", accounts_dir, cfg->user);

    if (fs_mkdir_p(account_dir, 0700) != 0) return -1;

    char path[1088];
    snprintf(path, sizeof(path), "%s/config.ini", account_dir);

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    write_config_to_fp(fp, cfg);
    fclose(fp);
    fs_ensure_permissions(path, 0600);

    /* Also update legacy config.ini so CLI tools see latest account */
    (void)config_save_to_store(cfg);

    logger_log(LOG_INFO, "Account saved: %s", cfg->user);
    return 0;
}

int config_delete_account(const char *name) {
    if (!name || !name[0]) return -1;

    RAII_STRING char *accounts_dir = get_accounts_dir();
    if (!accounts_dir) return -1;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/config.ini", accounts_dir, name);
    unlink(path);

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", accounts_dir, name);
    if (rmdir(dir) != 0 && errno != ENOENT) {
        logger_log(LOG_WARN, "Could not remove account dir %s", dir);
        return -1;
    }
    logger_log(LOG_INFO, "Account deleted: %s", name);
    return 0;
}

/** Load a config from a specific file path. */
static Config *load_config_from_path(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) { fclose(fp); return NULL; }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *key = strtok(line, "=");
        char *val = strtok(NULL, "\n");
        if (!key || !val) continue;
        key = trim(key); val = trim(val);
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
    fclose(fp);
    if (!cfg->folder) cfg->folder = strdup("INBOX");
    if (!cfg->host || !cfg->user || !cfg->pass) { config_free(cfg); return NULL; }

    /* TLS enforcement (same rules as config_load_from_store) */
    if (!cfg->ssl_no_verify) {
        if (strncmp(cfg->host, "imaps://", 8) != 0) {
            fprintf(stderr,
                "Error: EMAIL_HOST must start with imaps:// (TLS required).\n"
                "  Got: %s\n", cfg->host);
            logger_log(LOG_ERROR,
                       "Rejected insecure EMAIL_HOST in account config: %s",
                       cfg->host);
            config_free(cfg);
            return NULL;
        }
        if (cfg->smtp_host && cfg->smtp_host[0] &&
            strncmp(cfg->smtp_host, "smtps://", 8) != 0) {
            fprintf(stderr,
                "Error: SMTP_HOST must start with smtps:// (TLS required).\n"
                "  Got: %s\n", cfg->smtp_host);
            logger_log(LOG_ERROR,
                       "Rejected insecure SMTP_HOST in account config: %s",
                       cfg->smtp_host);
            config_free(cfg);
            return NULL;
        }
    } else {
        if (strncmp(cfg->host, "imaps://", 8) != 0)
            logger_log(LOG_WARN,
                       "SSL_NO_VERIFY=1: connecting without TLS to %s "
                       "(test/dev mode only)", cfg->host);
        if (cfg->smtp_host && cfg->smtp_host[0] &&
            strncmp(cfg->smtp_host, "smtps://", 8) != 0)
            logger_log(LOG_WARN,
                       "SSL_NO_VERIFY=1: SMTP without TLS to %s "
                       "(test/dev mode only)", cfg->smtp_host);
    }
    return cfg;
}

AccountEntry *config_list_accounts(int *count_out) {
    *count_out = 0;

    /* Collect into a growable array */
    int cap = 8;
    AccountEntry *list = malloc((size_t)cap * sizeof(AccountEntry));
    if (!list) return NULL;
    int count = 0;

    /* 1. Legacy config.ini */
    RAII_STRING char *legacy_path = get_config_path();
    if (legacy_path) {
        Config *cfg = load_config_from_path(legacy_path);
        if (cfg) {
            list[count].name   = strdup(cfg->user ? cfg->user : "default");
            list[count].cfg    = cfg;
            list[count].legacy = 1;
            count++;
        }
    }

    /* 2. Named accounts under accounts/ */
    RAII_STRING char *accounts_dir = get_accounts_dir();
    if (accounts_dir) {
        DIR *d = opendir(accounts_dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;

                char path[1024];
                snprintf(path, sizeof(path), "%s/%s/config.ini",
                         accounts_dir, ent->d_name);

                Config *cfg = load_config_from_path(path);
                if (!cfg) continue;

                /* Skip if same user as legacy entry (avoid duplicate) */
                int dup = 0;
                for (int i = 0; i < count; i++) {
                    if (list[i].cfg->user && cfg->user &&
                        strcmp(list[i].cfg->user, cfg->user) == 0) {
                        dup = 1; break;
                    }
                }
                if (dup) { config_free(cfg); continue; }

                if (count >= cap) {
                    cap *= 2;
                    AccountEntry *tmp = realloc(list,
                                                (size_t)cap * sizeof(AccountEntry));
                    if (!tmp) { config_free(cfg); break; }
                    list = tmp;
                }
                list[count].name   = strdup(ent->d_name);
                list[count].cfg    = cfg;
                list[count].legacy = 0;
                count++;
            }
            closedir(d);
        }
    }

    if (count == 0) { free(list); return NULL; }
    *count_out = count;
    return list;
}

void config_free_account_list(AccountEntry *list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) {
        free(list[i].name);
        config_free(list[i].cfg);
    }
    free(list);
}

