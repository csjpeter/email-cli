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
 * CLI tools use config_load_from_store() which returns the first account found.
 * The TUI accounts screen uses config_list_accounts() to show all profiles.
 *
 * Credential obfuscation is enabled by default. Passwords and tokens are
 * stored as "enc:<base64>" using AES-256-GCM with a key derived from
 * stable system data. Use config_migrate_credentials() to convert all
 * accounts to the current obfuscation setting after changing it.
 */

#include "config.h"

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
 * @brief Deletes a named account profile directory.
 * @return 0 on success or not-found, -1 on error.
 */
int config_delete_account(const char *name);

/**
 * @brief Loads the first configured account from the platform config directory.
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

#endif /* CONFIG_STORE_H */
