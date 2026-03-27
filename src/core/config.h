#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

/**
 * @file config.h
 * @brief IMAP account configuration type shared across all layers.
 */

/** IMAP account configuration. */
typedef struct {
    char *host;
    char *user;
    char *pass;
    char *folder;
    int   ssl_no_verify; /**< 1 = disable SSL peer verification (for self-signed certs in test envs) */
} Config;

/**
 * @brief Frees all heap-allocated fields of cfg, then frees cfg itself.
 * Safe to call with NULL.
 */
void config_free(Config *cfg);

#endif /* CORE_CONFIG_H */
