#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

/**
 * @file config.h
 * @brief IMAP account configuration type shared across all layers.
 */

/** IMAP account configuration. */
typedef struct {
    /* IMAP (incoming) */
    char *host;            /**< IMAP URL, e.g. "imaps://imap.example.com". */
    char *user;            /**< IMAP / default username (often an email address). */
    char *pass;            /**< IMAP password. */
    char *folder;          /**< Default IMAP folder name. */
    int   ssl_no_verify;   /**< 1 = disable SSL peer verification (for self-signed certs in test envs). */
    int   sync_interval;   /**< Sync interval in minutes; 0 = online mode, >0 = cron mode. */
    /* SMTP (outgoing) */
    char *smtp_host;       /**< SMTP URL, e.g. "smtp://smtp.example.com" or "smtps://…". NULL = derive from host. */
    int   smtp_port;       /**< SMTP port override; 0 = use protocol default (587 / 465). */
    char *smtp_user;       /**< SMTP username; NULL = reuse cfg->user. */
    char *smtp_pass;       /**< SMTP password; NULL = reuse cfg->pass. */
} Config;

/**
 * @brief Frees all heap-allocated fields of cfg, then frees cfg itself.
 * Safe to call with NULL.
 */
void config_free(Config *cfg);

#endif /* CORE_CONFIG_H */
