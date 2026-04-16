#ifndef PLATFORM_PATH_H
#define PLATFORM_PATH_H

#include <stddef.h>

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

/**
 * Returns the full path to the currently running executable.
 * POSIX/Linux: readlink("/proc/self/exe")
 * POSIX/macOS: _NSGetExecutablePath or realpath("/proc/curproc/file")
 * Windows: GetModuleFileNameA(NULL, ...)
 *
 * Writes into the caller-provided buffer.
 * Returns 0 on success, -1 if the path cannot be determined.
 */
int platform_executable_path(char *buf, size_t size);

#endif /* PLATFORM_PATH_H */
