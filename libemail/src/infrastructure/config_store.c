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
    if (cfg->gmail_mode) fprintf(fp, "GMAIL_MODE=1\n");
    if (cfg->gmail_refresh_token) fprintf(fp, "GMAIL_REFRESH_TOKEN=%s\n", cfg->gmail_refresh_token);
    if (cfg->gmail_client_id) fprintf(fp, "GMAIL_CLIENT_ID=%s\n", cfg->gmail_client_id);
    if (cfg->gmail_client_secret) fprintf(fp, "GMAIL_CLIENT_SECRET=%s\n", cfg->gmail_client_secret);
}

/** Load a config from a specific file path. */
static Config *load_config_from_path(const char *path) {
    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

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
        else if (strcmp(key, "GMAIL_MODE") == 0) cfg->gmail_mode = atoi(val);
        else if (strcmp(key, "GMAIL_REFRESH_TOKEN") == 0) cfg->gmail_refresh_token = strdup(val);
        else if (strcmp(key, "GMAIL_CLIENT_ID") == 0) cfg->gmail_client_id = strdup(val);
        else if (strcmp(key, "GMAIL_CLIENT_SECRET") == 0) cfg->gmail_client_secret = strdup(val);
    }
    if (!cfg->folder) cfg->folder = strdup("INBOX");

    /* Gmail mode requires user + refresh token; IMAP mode requires host + user + pass */
    if (cfg->gmail_mode) {
        if (!cfg->user || !cfg->gmail_refresh_token) { config_free(cfg); return NULL; }
    } else {
        if (!cfg->host || !cfg->user || !cfg->pass) { config_free(cfg); return NULL; }
    }

    /* TLS enforcement (IMAP mode only — Gmail uses OAuth2 over HTTPS) */
    if (!cfg->gmail_mode && !cfg->ssl_no_verify) {
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
    } else if (cfg->host) {
        /* ssl_no_verify mode — log warnings for non-TLS connections.
         * Skip entirely for Gmail (host is NULL). */
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

/* ── Public API ──────────────────────────────────────────────────────────── */

Config* config_load_from_store(void) {
    /* Reuse config_list_accounts which loads and sorts all accounts
     * alphabetically.  Take the first entry (lowest name) for a
     * deterministic result regardless of readdir ordering. */
    int count = 0;
    AccountEntry *list = config_list_accounts(&count);
    if (!list || count == 0) {
        config_free_account_list(list, count);
        return NULL;
    }
    Config *result = list[0].cfg;
    list[0].cfg = NULL; /* transfer ownership */
    config_free_account_list(list, count);
    return result;
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

    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    write_config_to_fp(fp, cfg);
    fs_ensure_permissions(path, 0600);

    logger_log(LOG_INFO, "Account saved: %s", cfg->user);
    return 0;
}

int config_save_to_store(const Config *cfg) {
    return config_save_account(cfg);
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

AccountEntry *config_list_accounts(int *count_out) {
    *count_out = 0;

    RAII_STRING char *accounts_dir = get_accounts_dir();
    if (!accounts_dir) return NULL;

    RAII_DIR DIR *d = opendir(accounts_dir);
    if (!d) return NULL;

    int cap = 8;
    AccountEntry *list = malloc((size_t)cap * sizeof(AccountEntry));
    if (!list) return NULL;
    int count = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s/config.ini",
                 accounts_dir, ent->d_name);

        Config *cfg = load_config_from_path(path);
        if (!cfg) continue;

        if (count >= cap) {
            cap *= 2;
            AccountEntry *tmp = realloc(list, (size_t)cap * sizeof(AccountEntry));
            if (!tmp) { config_free(cfg); break; }
            list = tmp;
        }
        list[count].name = strdup(ent->d_name);
        list[count].cfg  = cfg;
        count++;
    }

    if (count == 0) { free(list); return NULL; }

    /* Sort alphabetically by name for consistent ordering. */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(list[i].name, list[j].name) > 0) {
                AccountEntry tmp = list[i];
                list[i] = list[j];
                list[j] = tmp;
            }
        }
    }

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
