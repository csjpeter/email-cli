#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

/**
 * @file config_store.h
 * @brief Secure configuration management.
 */

typedef struct {
    char *host;
    char *user;
    char *pass;
    char *folder;
    int ssl_no_verify; /**< 1 = disable SSL peer verification (for self-signed certs in test envs) */
} Config;

/**
 * @brief Loads configuration from ~/.config/email-cli/config.ini.
 * If file does not exist, returns NULL (caller should trigger wizard).
 * @return Pointer to Config struct or NULL.
 */
Config* config_load_from_store(void);

/**
 * @brief Saves configuration to ~/.config/email-cli/config.ini with 0600 permissions.
 * @param cfg The config to save.
 * @return 0 on success, -1 on failure.
 */
int config_save_to_store(const Config *cfg);

/**
 * @brief Frees memory allocated by a Config struct.
 */
void config_free(Config *cfg);

#endif // CONFIG_STORE_H
