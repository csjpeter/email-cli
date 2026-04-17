#ifndef FS_UTIL_H
#define FS_UTIL_H

#include <sys/types.h>

/**
 * @file fs_util.h
 * @brief File system helpers for directory creation and permissions.
 */

/**
 * @brief Recursively create a directory with specific permissions.
 * @param path The directory path to create.
 * @param mode The permissions (e.g., 0700).
 * @return 0 on success, -1 on failure.
 */
int fs_mkdir_p(const char *path, mode_t mode);

/**
 * @brief Ensures a file has specific permissions.
 * @param path Path to the file.
 * @param mode The permissions (e.g., 0600).
 * @return 0 on success, -1 on failure.
 */
int fs_ensure_permissions(const char *path, mode_t mode);

/**
 * @brief Gets the user's home directory.
 * @return String (must NOT be freed), or NULL.
 */
const char *fs_get_home_dir(void);

#endif /* FS_UTIL_H */
