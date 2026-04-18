#include "config_store.h"
#include "config.h"
#include "fs_util.h"
#include "platform/path.h"
#include "platform/credential_key.h"
#include "raii.h"
#include "logger.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
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

/* ── Global application settings ────────────────────────────────────────── */

static int g_obfuscation_loaded = 0;
static int g_credential_obfuscation = 1; /* default: ON */

static char *get_settings_path(void) {
    const char *config_base = platform_config_dir();
    if (!config_base) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/%s/settings.ini", config_base, CONFIG_APP_DIR) == -1)
        return NULL;
    return path;
}

static void write_settings(const char *path) {
    const char *config_base = platform_config_dir();
    if (!config_base) return;
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/%s", config_base, CONFIG_APP_DIR);
    if (fs_mkdir_p(dir, 0700) != 0) return;
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "credential_obfuscation=%d\n", g_credential_obfuscation);
    fclose(fp);
    fs_ensure_permissions(path, 0600);
}

static void load_settings_once(void) {
    if (g_obfuscation_loaded) return;
    g_obfuscation_loaded = 1;

    RAII_STRING char *path = get_settings_path();
    if (!path) return;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* First run — create settings.ini with defaults */
        write_settings(path);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *key = strtok(line, "=");
        char *val = strtok(NULL, "\n");
        if (!key || !val) continue;
        key = trim(key); val = trim(val);
        if (strcmp(key, "credential_obfuscation") == 0)
            g_credential_obfuscation = atoi(val);
    }
    fclose(fp);
}

int app_settings_get_obfuscation(void) {
    load_settings_once();
    return g_credential_obfuscation;
}

int app_settings_set_obfuscation(int enabled) {
    load_settings_once();
    g_credential_obfuscation = enabled ? 1 : 0;
    RAII_STRING char *path = get_settings_path();
    if (!path) return -1;
    write_settings(path);
    return 0;
}

/* ── Base64 encode / decode ──────────────────────────────────────────────── */

static const char B64CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Returns heap-allocated base64 string. Caller must free(). */
static char *b64_encode(const unsigned char *src, size_t src_len) {
    size_t out_len = ((src_len + 2) / 3) * 4 + 1;
    char *out = malloc(out_len);
    if (!out) return NULL;
    size_t i, j = 0;
    for (i = 0; i + 2 < src_len; i += 3) {
        out[j++] = B64CHARS[src[i] >> 2];
        out[j++] = B64CHARS[((src[i] & 3) << 4)   | (src[i+1] >> 4)];
        out[j++] = B64CHARS[((src[i+1] & 0xf) << 2) | (src[i+2] >> 6)];
        out[j++] = B64CHARS[src[i+2] & 0x3f];
    }
    size_t rem = src_len - i;
    if (rem == 1) {
        out[j++] = B64CHARS[src[i] >> 2];
        out[j++] = B64CHARS[(src[i] & 3) << 4];
        out[j++] = '='; out[j++] = '=';
    } else if (rem == 2) {
        out[j++] = B64CHARS[src[i] >> 2];
        out[j++] = B64CHARS[((src[i] & 3) << 4) | (src[i+1] >> 4)];
        out[j++] = B64CHARS[(src[i+1] & 0xf) << 2];
        out[j++] = '=';
    }
    out[j] = '\0';
    return out;
}

static int b64_char_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return 0;
    return -1;
}

/**
 * Decode base64 string into *out (heap-allocated). Caller must free().
 * Returns 0 on success, -1 on error.
 */
static int b64_decode(const char *src, unsigned char **out, size_t *out_len) {
    size_t src_len = strlen(src);
    if (src_len == 0 || src_len % 4 != 0) return -1;

    size_t dec_len = (src_len / 4) * 3;
    if (src[src_len - 1] == '=') dec_len--;
    if (src[src_len - 2] == '=') dec_len--;

    unsigned char *buf = malloc(dec_len + 1);
    if (!buf) return -1;

    size_t j = 0;
    for (size_t i = 0; i < src_len; i += 4) {
        int a = b64_char_val(src[i]);
        int b = b64_char_val(src[i+1]);
        int c = b64_char_val(src[i+2]);
        int d = b64_char_val(src[i+3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) { free(buf); return -1; }
        buf[j++] = (unsigned char)((a << 2) | (b >> 4));
        if (src[i+2] != '=') buf[j++] = (unsigned char)((b << 4) | (c >> 2));
        if (src[i+3] != '=') buf[j++] = (unsigned char)((c << 6) | d);
    }
    buf[j] = '\0';
    *out = buf;
    *out_len = j;
    return 0;
}

/* ── Credential encryption / decryption (AES-256-GCM) ───────────────────── */

/**
 * Encrypt plaintext with AES-256-GCM using a key derived from the email.
 * Returns heap-allocated "enc:<base64(iv|ciphertext|tag)>" string,
 * or NULL if key derivation failed (caller falls back to plaintext).
 */
static char *encrypt_credential(const char *plaintext, const char *email) {
    if (!plaintext || !*plaintext)
        return strdup(plaintext ? plaintext : "");

    unsigned char key[32];
    if (platform_derive_credential_key(email, key) != 0)
        return NULL; /* no key source available — store plaintext */

    unsigned char iv[12];
    if (RAND_bytes(iv, sizeof(iv)) != 1) return NULL;

    size_t pt_len = strlen(plaintext);
    unsigned char *ct = malloc(pt_len + 1);
    if (!ct) return NULL;

    unsigned char tag[16];
    int outl = 0, finl = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(ct); return NULL; }
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv);
    EVP_EncryptUpdate(ctx, ct, &outl, (const unsigned char *)plaintext, (int)pt_len);
    EVP_EncryptFinal_ex(ctx, ct + outl, &finl);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(ctx);

    /* Pack: iv[12] | ciphertext[pt_len] | tag[16] */
    size_t packed_len = 12 + pt_len + 16;
    unsigned char *packed = malloc(packed_len);
    if (!packed) { free(ct); return NULL; }
    memcpy(packed,              iv,  12);
    memcpy(packed + 12,         ct,  pt_len);
    memcpy(packed + 12 + pt_len, tag, 16);
    free(ct);

    char *b64 = b64_encode(packed, packed_len);
    free(packed);
    if (!b64) return NULL;

    char *result = NULL;
    if (asprintf(&result, "enc:%s", b64) == -1) result = NULL;
    free(b64);
    return result;
}

/**
 * Decrypt a credential value.
 * - If value starts with "enc:", decrypt using a key derived from email.
 * - Otherwise return a copy of the plaintext value.
 * Returns heap-allocated plaintext, or NULL on decryption failure.
 */
static char *decrypt_credential(const char *value, const char *email) {
    if (!value) return NULL;
    if (strncmp(value, "enc:", 4) != 0) return strdup(value);

    unsigned char *packed = NULL;
    size_t packed_len = 0;
    if (b64_decode(value + 4, &packed, &packed_len) != 0) return NULL;
    if (packed_len < 12 + 16) { free(packed); return NULL; }

    unsigned char key[32];
    if (platform_derive_credential_key(email, key) != 0) {
        free(packed);
        return NULL;
    }

    unsigned char *iv  = packed;
    size_t ct_len      = packed_len - 12 - 16;
    unsigned char *ct  = packed + 12;
    unsigned char *tag = packed + 12 + ct_len;

    unsigned char *pt = malloc(ct_len + 1);
    if (!pt) { free(packed); return NULL; }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(packed); free(pt); return NULL; }

    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag);
    int outl = 0, finl = 0;
    EVP_DecryptUpdate(ctx, pt, &outl, ct, (int)ct_len);
    int ok = EVP_DecryptFinal_ex(ctx, pt + outl, &finl);
    EVP_CIPHER_CTX_free(ctx);
    free(packed);

    if (ok != 1) {
        /* Authentication failed — wrong key (system data changed) */
        free(pt);
        return NULL;
    }
    pt[ct_len] = '\0';
    return (char *)pt;
}

/* ── Config read / write ────────────────────────────────────────────────── */

/** Write one config struct to an open FILE, encrypting credentials if enabled. */
static void write_config_to_fp(FILE *fp, const Config *cfg) {
    int obfus = app_settings_get_obfuscation();
    const char *email = cfg->user ? cfg->user : "";

    fprintf(fp, "EMAIL_HOST=%s\n",   cfg->host   ? cfg->host   : "");
    fprintf(fp, "EMAIL_USER=%s\n",   cfg->user   ? cfg->user   : "");

    /* Credentials: encrypt when obfuscation is on */
    {
        char *enc = (obfus && cfg->pass && *cfg->pass)
                    ? encrypt_credential(cfg->pass, email) : NULL;
        fprintf(fp, "EMAIL_PASS=%s\n", enc ? enc : (cfg->pass ? cfg->pass : ""));
        free(enc);
    }

    fprintf(fp, "EMAIL_FOLDER=%s\n", cfg->folder ? cfg->folder : "INBOX");
    if (cfg->sent_folder) fprintf(fp, "EMAIL_SENT_FOLDER=%s\n", cfg->sent_folder);
    if (cfg->ssl_no_verify) fprintf(fp, "SSL_NO_VERIFY=1\n");
    fprintf(fp, "SYNC_INTERVAL=%d\n", cfg->sync_interval);
    if (cfg->smtp_host) fprintf(fp, "SMTP_HOST=%s\n", cfg->smtp_host);
    if (cfg->smtp_port) fprintf(fp, "SMTP_PORT=%d\n", cfg->smtp_port);
    if (cfg->smtp_user) fprintf(fp, "SMTP_USER=%s\n", cfg->smtp_user);
    if (cfg->smtp_pass) {
        char *enc = (obfus && *cfg->smtp_pass)
                    ? encrypt_credential(cfg->smtp_pass, email) : NULL;
        fprintf(fp, "SMTP_PASS=%s\n", enc ? enc : cfg->smtp_pass);
        free(enc);
    }
    if (cfg->gmail_mode) fprintf(fp, "GMAIL_MODE=1\n");
    if (cfg->gmail_refresh_token) {
        char *enc = (obfus && *cfg->gmail_refresh_token)
                    ? encrypt_credential(cfg->gmail_refresh_token, email) : NULL;
        fprintf(fp, "GMAIL_REFRESH_TOKEN=%s\n", enc ? enc : cfg->gmail_refresh_token);
        free(enc);
    }
    if (cfg->gmail_client_id) fprintf(fp, "GMAIL_CLIENT_ID=%s\n", cfg->gmail_client_id);
    if (cfg->gmail_client_secret) fprintf(fp, "GMAIL_CLIENT_SECRET=%s\n", cfg->gmail_client_secret);
}

/** Load a config from a specific file path. Decrypts enc: credentials transparently. */
static Config *load_config_from_path(const char *path) {
    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    char line[1024]; /* wider than before — enc: values can be long */
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(line);
        char *val = trim(eq + 1);
        /* Strip trailing newline from val */
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r'))
            val[--vlen] = '\0';

        if      (strcmp(key, "EMAIL_HOST")          == 0) cfg->host               = strdup(val);
        else if (strcmp(key, "EMAIL_USER")          == 0) cfg->user               = strdup(val);
        else if (strcmp(key, "EMAIL_PASS")          == 0) cfg->pass               = strdup(val);
        else if (strcmp(key, "EMAIL_FOLDER")        == 0) cfg->folder             = strdup(val);
        else if (strcmp(key, "EMAIL_SENT_FOLDER")   == 0) cfg->sent_folder        = strdup(val);
        else if (strcmp(key, "SSL_NO_VERIFY")       == 0) cfg->ssl_no_verify      = atoi(val);
        else if (strcmp(key, "SYNC_INTERVAL")       == 0) cfg->sync_interval      = atoi(val);
        else if (strcmp(key, "SMTP_HOST")           == 0) cfg->smtp_host          = strdup(val);
        else if (strcmp(key, "SMTP_PORT")           == 0) cfg->smtp_port          = atoi(val);
        else if (strcmp(key, "SMTP_USER")           == 0) cfg->smtp_user          = strdup(val);
        else if (strcmp(key, "SMTP_PASS")           == 0) cfg->smtp_pass          = strdup(val);
        else if (strcmp(key, "GMAIL_MODE")          == 0) cfg->gmail_mode         = atoi(val);
        else if (strcmp(key, "GMAIL_REFRESH_TOKEN") == 0) cfg->gmail_refresh_token = strdup(val);
        else if (strcmp(key, "GMAIL_CLIENT_ID")     == 0) cfg->gmail_client_id    = strdup(val);
        else if (strcmp(key, "GMAIL_CLIENT_SECRET") == 0) cfg->gmail_client_secret = strdup(val);
    }
    if (!cfg->folder) cfg->folder = strdup("INBOX");

    /* Decrypt any enc: credential fields using the account email as key context */
    const char *email = cfg->user ? cfg->user : "";

    if (cfg->pass && strncmp(cfg->pass, "enc:", 4) == 0) {
        char *dec = decrypt_credential(cfg->pass, email);
        free(cfg->pass);
        if (dec) {
            cfg->pass = dec;
        } else {
            fprintf(stderr,
                "Warning: Could not decrypt stored password for '%s'.\n"
                "  The system key may have changed. Re-enter the password with:\n"
                "    email-cli config password\n", email);
            logger_log(LOG_WARN, "credential decrypt failed for %s", email);
            cfg->pass = NULL;
        }
    }

    if (cfg->smtp_pass && strncmp(cfg->smtp_pass, "enc:", 4) == 0) {
        char *dec = decrypt_credential(cfg->smtp_pass, email);
        free(cfg->smtp_pass);
        cfg->smtp_pass = dec; /* NULL on failure is acceptable for SMTP */
    }

    if (cfg->gmail_refresh_token && strncmp(cfg->gmail_refresh_token, "enc:", 4) == 0) {
        char *dec = decrypt_credential(cfg->gmail_refresh_token, email);
        free(cfg->gmail_refresh_token);
        if (dec) {
            cfg->gmail_refresh_token = dec;
        } else {
            fprintf(stderr,
                "Warning: Could not decrypt stored refresh token for '%s'.\n"
                "  Re-run the Gmail OAuth2 setup: email-cli add-account\n", email);
            logger_log(LOG_WARN, "refresh token decrypt failed for %s", email);
            cfg->gmail_refresh_token = NULL;
        }
    }

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
    load_settings_once(); /* ensure settings.ini exists */
    int count = 0;
    AccountEntry *list = config_list_accounts(&count);
    if (!list || count == 0) {
        config_free_account_list(list, count);
        return NULL;
    }
    Config *result = list[0].cfg;
    list[0].cfg = NULL;
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
    load_settings_once(); /* ensure settings.ini exists */
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

    /* Sort by domain first, then by username within domain */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            const char *na = list[i].name ? list[i].name : "";
            const char *nb = list[j].name ? list[j].name : "";
            const char *at_a = strchr(na, '@');
            const char *at_b = strchr(nb, '@');
            const char *dom_a = at_a ? at_a + 1 : na;
            const char *dom_b = at_b ? at_b + 1 : nb;
            int dc = strcmp(dom_a, dom_b);
            int swap;
            if (dc != 0) {
                swap = dc > 0;
            } else {
                size_t ul_a = at_a ? (size_t)(at_a - na) : strlen(na);
                size_t ul_b = at_b ? (size_t)(at_b - nb) : strlen(nb);
                int uc = strncmp(na, nb, ul_a < ul_b ? ul_a : ul_b);
                swap = (uc != 0) ? (uc > 0) : (ul_a > ul_b);
            }
            if (swap) {
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

int config_migrate_credentials(void) {
    int count = 0;
    AccountEntry *list = config_list_accounts(&count);
    if (!list) return 0; /* no accounts — nothing to migrate */

    int errors = 0;
    for (int i = 0; i < count; i++) {
        if (list[i].cfg && config_save_account(list[i].cfg) != 0) {
            fprintf(stderr, "Warning: could not migrate account '%s'\n",
                    list[i].name ? list[i].name : "?");
            errors++;
        }
    }
    config_free_account_list(list, count);
    return errors ? -1 : 0;
}
