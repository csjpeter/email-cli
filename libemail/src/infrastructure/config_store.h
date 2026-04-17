#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

/**
 * @file config_store.h
 * @brief Secure configuration management (load/save to disk).
 *
 * Accounts are stored exclusively as:
 *   ~/.config/email-cli/accounts/<email>/config.ini
 *
 * CLI tools use config_load_from_store() which returns the first account found.
 * The TUI accounts screen uses config_list_accounts() to show all profiles.
 */

#include "config.h"

/**
 * @brief One entry in the account list returned by config_list_accounts().
 *
 * Both @p name and @p cfg are heap-allocated; free them with
 * config_free_account_list() or individually (free(name), config_free(cfg)).
 */
typedef struct {
    char   *name;   /**< Profile name / directory name (email address). */
    Config *cfg;    /**< Loaded configuration. */
} AccountEntry;

/**
 * @brief Lists all configured accounts from ~/.config/email-cli/accounts/.
 *
 * @param count_out  Set to the number of entries returned.
 * @return Heap-allocated array of AccountEntry, or NULL if none found.
 *         Free with config_free_account_list().
 */
AccountEntry *config_list_accounts(int *count_out);

/**
 * @brief Frees an account list returned by config_list_accounts().
 */
void config_free_account_list(AccountEntry *list, int count);

/**
 * @brief Saves a named account profile to
 *        ~/.config/email-cli/accounts/<cfg->user>/config.ini (mode 0600).
 *
 * Creates the directory if necessary.
 *
 * @param cfg  Configuration to save; cfg->user is used as the profile name.
 * @return 0 on success, -1 on failure.
 */
int config_save_account(const Config *cfg);

/**
 * @brief Deletes a named account profile directory.
 *
 * Removes ~/.config/email-cli/accounts/<name>/ and its contents.
 * Has no effect on the legacy root config.ini.
 *
 * @param name  Profile name (must match cfg->user used when saving).
 * @return 0 on success or not-found, -1 on error.
 */
int config_delete_account(const char *name);

/**
 * @brief Loads configuration from the platform config directory.
 * If file does not exist, returns NULL (caller should trigger wizard).
 * @return Pointer to Config struct or NULL.
 */
Config *config_load_from_store(void);

/**
 * @brief Saves configuration to the platform config directory with 0600 permissions.
 * @param cfg The config to save.
 * @return 0 on success, -1 on failure.
 */
int config_save_to_store(const Config *cfg);

#endif /* CONFIG_STORE_H */
