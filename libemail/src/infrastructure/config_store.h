/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-24 */
/* Interface of the configuration store. */
#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

/**
 * @file config_store.h
 * @brief Secure configuration management (load/save to disk).
 *
 * Accounts are stored exclusively as:
 *   ~/.config/email-cli/accounts/<email>/config.ini
 *
 * Global application settings are stored in:
 *   ~/.config/email-cli/settings.ini
 *
 * Use config_load_account(name) to load a specific account by email address.
 * config_load_from_store() is kept for single-account convenience.
 * The TUI accounts screen uses config_list_accounts() to show all profiles.
 *
 * Credential obfuscation is enabled by default. Passwords and tokens are
 * stored as "enc:<base64>" using AES-256-GCM with a key derived from
 * stable system data. Use config_migrate_credentials() to convert all
 * accounts to the current obfuscation setting after changing it.
 */

#include "config.h"
#include "op_history.h"

/**
 * @brief One entry in the account list returned by config_list_accounts().
 */
typedef struct {
    char   *name;   /**< Profile name / directory name (email address). */
    Config *cfg;    /**< Loaded configuration (credentials already decrypted). */
} AccountEntry;

/**
 * @brief Lists all configured accounts from ~/.config/email-cli/accounts/.
 * @param count_out  Set to the number of entries returned.
 * @return Heap-allocated array of AccountEntry, or NULL if none found.
 *         Free with config_free_account_list().
 */
AccountEntry *config_list_accounts(int *count_out);

/** @brief Frees an account list returned by config_list_accounts(). */
void config_free_account_list(AccountEntry *list, int count);

/**
 * @brief Saves a named account profile to
 *        ~/.config/email-cli/accounts/<cfg->user>/config.ini (mode 0600).
 * Credentials are encrypted according to the current obfuscation setting.
 * @return 0 on success, -1 on failure.
 */
int config_save_account(const Config *cfg);

/**
 * @brief The words of the last failed config_save_account / config_delete_account:
 *        what failed, the path and the errno text.  Empty when none.
 */
const char *config_last_error(void);

/**
 * @brief Deletes a named account profile directory.
 * @return 0 on success or not-found, -1 on error.
 */
int config_delete_account(const char *name);

/**
 * @brief Loads a specific account by email address (or directory name).
 * @param name  The account email address, e.g. "user\@example.com".
 * @return Pointer to Config struct, or NULL if not found.
 *         Caller must free with config_free().
 */
Config *config_load_account(const char *name);

/** Why config_load_account_quiet() returned no account. */
enum {
    CFG_LOAD_OK = 0,        /**< loaded */
    CFG_LOAD_NOT_FOUND,     /**< no such account */
    CFG_LOAD_UNREADABLE,    /**< the file cannot be read, is incomplete or insecure */
    CFG_LOAD_UNDECRYPTABLE  /**< an enc: credential cannot be decrypted */
};

/**
 * @brief Like config_load_account(), for a library: prints and logs nothing,
 *        does not rewrite the file, and tells the causes apart.
 * @param name    Account directory name or address.
 * @param reason  Receives a CFG_LOAD_* value; may be NULL.
 * @return the Config (free with config_free()) or NULL.
 */
Config *config_load_account_quiet(const char *name, int *reason);

/**
 * @brief Loads the first configured account from the platform config directory.
 *        Convenience wrapper for single-account setups.
 * @return Pointer to Config struct or NULL.
 */
Config *config_load_from_store(void);

/**
 * @brief Saves configuration to the platform config directory.
 * @return 0 on success, -1 on failure.
 */
int config_save_to_store(const Config *cfg);

/**
 * @brief Re-saves all accounts using the current obfuscation setting.
 *
 * Call this after changing credential_obfuscation in settings.ini to
 * convert all stored credentials to the new format (encrypted ↔ plaintext).
 *
 * @return 0 on success, -1 if any account could not be migrated.
 */
int config_migrate_credentials(void);

/* ── Application settings ──────────────────────────────────────────────── */

/**
 * @brief Returns 1 if credential obfuscation is enabled, 0 otherwise.
 * Reads ~/.config/email-cli/settings.ini on first call; default is 1 (ON).
 */
int app_settings_get_obfuscation(void);

/**
 * @brief Sets and persists the credential obfuscation setting.
 * Does NOT migrate existing credentials — call config_migrate_credentials().
 * @return 0 on success, -1 on error.
 */
int app_settings_set_obfuscation(int enabled);

/**
 * @brief Reverts a recorded step of operation "account" (one field of an
 *        account) or "setting": the current value must still be the step's
 *        new value.  A new step that names the reverted one is recorded.
 * @param refusal  The reason, when the step is refused.
 * @return 0 when reverted, -1 when refused.
 */
int config_revert_step(const OpStep *step, char *refusal, size_t size);

#endif /* CONFIG_STORE_H */
