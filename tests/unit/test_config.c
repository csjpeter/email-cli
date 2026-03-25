#include "test_helpers.h"
#include "config_store.h"
#include "fs_util.h"
#include <string.h>
#include <stdlib.h>

void test_config_store(void) {
    Config cfg;
    cfg.host = strdup("imaps://imap.test.com");
    cfg.user = strdup("test@test.com");
    cfg.pass = strdup("password123");
    cfg.folder = strdup("INBOX");

    // 1. Test Save (using a temp home for test)
    // Actually config_store uses fs_get_home_dir which uses getenv("HOME")
    // We can override HOME for the test
    char *old_home = getenv("HOME");
    setenv("HOME", "/tmp/email-cli-test-home", 1);
    fs_mkdir_p("/tmp/email-cli-test-home/.config/email-cli", 0700);

    int res = config_save_to_store(&cfg);
    ASSERT(res == 0, "config_save_to_store should return 0");

    // 2. Test Load
    Config *loaded = config_load_from_store();
    ASSERT(loaded != NULL, "config_load_from_store should not return NULL");
    ASSERT(strcmp(loaded->host, cfg.host) == 0, "Host should match");
    ASSERT(strcmp(loaded->user, cfg.user) == 0, "User should match");
    ASSERT(strcmp(loaded->pass, cfg.pass) == 0, "Pass should match");

    // Cleanup
    config_free(loaded);
    free(cfg.host);
    free(cfg.user);
    free(cfg.pass);
    free(cfg.folder);
    
    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}
