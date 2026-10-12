/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-27 */
/* Interface of the platform's home, config and cache directory resolution. */
#ifndef PLATFORM_PATH_H
#define PLATFORM_PATH_H

#include <stddef.h>

/**
 * @file path.h
 * @brief Platform-specific base directory resolution.
 */

/**
 * Returns the user's home directory path.
 * $HOME or getpwuid(getuid())->pw_dir
 * The returned string is owned by the platform layer (do not free).
 * Returns NULL if the home directory cannot be determined.
 */
const char *platform_home_dir(void);

/**
 * Returns the base directory for user-specific cache files.
 * $XDG_CACHE_HOME or ~/.cache
 * The returned string is owned by the platform layer (do not free).
 */
const char *platform_cache_dir(void);

/**
 * Returns the base directory for user-specific configuration files.
 * $XDG_CONFIG_HOME or ~/.config
 * The returned string is owned by the platform layer (do not free).
 */
const char *platform_config_dir(void);

/**
 * Returns the base directory for user-specific persistent data files.
 * $XDG_DATA_HOME or ~/.local/share
 * The returned string is owned by the platform layer (do not free).
 */
const char *platform_data_dir(void);

/**
 * Returns the full path to the currently running executable.
 * readlink("/proc/self/exe")
 *
 * Writes into the caller-provided buffer.
 * Returns 0 on success, -1 if the path cannot be determined.
 */
int platform_executable_path(char *buf, size_t size);

#endif /* PLATFORM_PATH_H */
