#include "test_helpers.h"
#include "fs_util.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

void test_fs_util(void) {
    // 1. Test Home Dir
    const char *home = fs_get_home_dir();
    ASSERT(home != NULL, "Home directory should not be NULL");

    // 2. Test Mkdir P
    char test_dir[1024];
    snprintf(test_dir, sizeof(test_dir), "/tmp/email-cli-test-%d/a/b/c", getpid());
    
    int res = fs_mkdir_p(test_dir, 0700);
    ASSERT(res == 0, "fs_mkdir_p should return 0");

    struct stat st;
    ASSERT(stat(test_dir, &st) == 0, "Directory should exist");
    ASSERT((st.st_mode & 0777) == 0700, "Directory should have 0700 permissions");

    // Cleanup
    rmdir("/tmp/email-cli-test-pid/a/b/c"); // simplified
}
