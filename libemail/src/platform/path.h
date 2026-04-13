#ifndef PLATFORM_PATH_H
#define PLATFORM_PATH_H

/**
 * @file path.h
 * @brief Platform-specific base directory resolution.
 */

/**
 * Returns the user's home directory path.
 * POSIX: $HOME or getpwuid(getuid())->pw_dir
 * Windows: %USERPROFILE%
 * The returned string is owned by the platform layer (do not free).
 * Returns NULL if the home directory cannot be determined.
 */
const char *platform_home_dir(void);

/**
 * Returns the base directory for user-specific cache files.
 * POSIX: $XDG_CACHE_HOME or ~/.cache
 * Windows: %LOCALAPPDATA%
 * The returned string is owned by the platform layer (do not free).
 */
const char *platform_cache_dir(void);

/**
 * Returns the base directory for user-specific configuration files.
 * POSIX: $XDG_CONFIG_HOME or ~/.config
 * Windows: %APPDATA%
 * The returned string is owned by the platform layer (do not free).
 */
const char *platform_config_dir(void);

/**
 * Returns the base directory for user-specific persistent data files.
 * POSIX: $XDG_DATA_HOME or ~/.local/share
 * Windows: %APPDATA%
 * The returned string is owned by the platform layer (do not free).
 */
const char *platform_data_dir(void);

#endif /* PLATFORM_PATH_H */
