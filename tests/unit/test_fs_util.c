#include "test_helpers.h"
#include "fs_util.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

void test_fs_util(void) {
    // 1. Test Home Dir with HOME set
    const char *home = fs_get_home_dir();
    ASSERT(home != NULL, "Home directory should not be NULL");

    // 2. Test fs_get_home_dir fallback via getpwuid when HOME is unset
    char *saved_home = getenv("HOME");
    unsetenv("HOME");
    home = fs_get_home_dir();
    ASSERT(home != NULL, "fs_get_home_dir should fall back to getpwuid");
    if (saved_home) setenv("HOME", saved_home, 1);

    // 3. Test Mkdir P (nested)
    char test_dir[256];
    snprintf(test_dir, sizeof(test_dir), "/tmp/email-cli-test-%d/a/b/c", getpid());
    int res = fs_mkdir_p(test_dir, 0700);
    ASSERT(res == 0, "fs_mkdir_p should return 0");

    struct stat st;
    ASSERT(stat(test_dir, &st) == 0, "Directory should exist");
    ASSERT((st.st_mode & 0777) == 0700, "Directory should have 0700 permissions");

    // 4. Test Mkdir P with trailing slash
    char test_dir_slash[256];
    snprintf(test_dir_slash, sizeof(test_dir_slash),
             "/tmp/email-cli-test-%d/a/b/c/", getpid());
    res = fs_mkdir_p(test_dir_slash, 0700);
    ASSERT(res == 0, "fs_mkdir_p should handle trailing slash");

    // 5. Test fs_mkdir_p on already-existing directory (idempotent)
    res = fs_mkdir_p(test_dir, 0700);
    ASSERT(res == 0, "fs_mkdir_p should succeed on existing directory");

    // 6. Test fs_ensure_permissions
    res = fs_ensure_permissions(test_dir, 0755);
    ASSERT(res == 0, "fs_ensure_permissions should succeed");
    ASSERT(stat(test_dir, &st) == 0, "Directory should still exist");
    ASSERT((st.st_mode & 0777) == 0755, "Directory should have 0755 permissions");

    // 7. Test fs_mkdir_p with "/" — strips trailing slash → empty tmp → return 0 (line 49)
    res = fs_mkdir_p("/", 0700);
    ASSERT(res == 0, "fs_mkdir_p(\"/\") should return 0");
}
