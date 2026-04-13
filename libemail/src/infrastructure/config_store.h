#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

/**
 * @file config_store.h
 * @brief Secure configuration management (load/save to disk).
 */

#include "config.h"

/**
 * @brief Loads configuration from the platform config directory.
 * If file does not exist, returns NULL (caller should trigger wizard).
 * @return Pointer to Config struct or NULL.
 */
Config* config_load_from_store(void);

/**
 * @brief Saves configuration to the platform config directory with 0600 permissions.
 * @param cfg The config to save.
 * @return 0 on success, -1 on failure.
 */
int config_save_to_store(const Config *cfg);

#endif /* CONFIG_STORE_H */
