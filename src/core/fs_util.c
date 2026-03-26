#include "fs_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>

/**
 * @brief Creates a directory and all missing parent directories.
 * @param path  Directory path to create.
 * @param mode  Permission bits applied to each created directory.
 * @return 0 on success, -1 on error.
 */
int fs_mkdir_p(const char *path, mode_t mode) {
    char tmp[1024];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    
    // Explicitly set mode in case of umask
    return chmod(tmp, mode);
}

/**
 * @brief Sets the permission bits on an existing file or directory.
 * @param path  Path to the file.
 * @param mode  Desired permission bits.
 * @return 0 on success, -1 on error.
 */
int fs_ensure_permissions(const char *path, mode_t mode) {
    return chmod(path, mode);
}

/**
 * @brief Returns the user's home directory path.
 *
 * Checks the HOME environment variable first; falls back to the passwd entry.
 * @return Pointer to the home path string, or NULL if unavailable.
 */
const char* fs_get_home_dir(void) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }
    return home;
}
