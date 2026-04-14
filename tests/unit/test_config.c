#include "test_helpers.h"
#include "config_store.h"
#include "fs_util.h"
#include "logger.h"
#include "raii.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static void config_cleanup(void *ptr) {
    Config **cfg = (Config **)ptr;
    if (cfg && *cfg) {
        config_free(*cfg);
        *cfg = NULL;
    }
}

void test_config_store(void) {
    char *old_home = getenv("HOME");
    char *old_xdg  = getenv("XDG_CONFIG_HOME");
    setenv("HOME", "/tmp/email-cli-test-home", 1);
    unsetenv("XDG_CONFIG_HOME");
    /* Pre-clean: remove any leftover account from previous test runs */
    unlink("/tmp/email-cli-test-home/.config/email-cli/accounts/test@test.com/config.ini");
    rmdir("/tmp/email-cli-test-home/.config/email-cli/accounts/test@test.com");
    rmdir("/tmp/email-cli-test-home/.config/email-cli/accounts");

    // 1. Test Save
    {
        Config cfg = {0};
        cfg.host   = strdup("imaps://imap.test.com");
        cfg.user   = strdup("test@test.com");
        cfg.pass   = strdup("password123");
        cfg.folder = strdup("INBOX");

        int res = config_save_to_store(&cfg);
        ASSERT(res == 0, "config_save_to_store should return 0");

        free(cfg.host);
        free(cfg.user);
        free(cfg.pass);
        free(cfg.folder);
    }

    // 2. Test Load - full config
    {
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded != NULL, "config_load_from_store should not return NULL");
        ASSERT(strcmp(loaded->host, "imaps://imap.test.com") == 0, "Host should match");
        ASSERT(strcmp(loaded->user, "test@test.com") == 0, "User should match");
        ASSERT(strcmp(loaded->pass, "password123") == 0, "Pass should match");
    }

    // 3. Test Load - incomplete config (missing host) → should return NULL
    {
        const char *acct_path =
            "/tmp/email-cli-test-home/.config/email-cli/accounts/test@test.com/config.ini";
        FILE *fp = fopen(acct_path, "w");
        if (fp) {
            fprintf(fp, "EMAIL_USER=test@test.com\n");
            fprintf(fp, "EMAIL_PASS=password123\n");
            fclose(fp);
        }
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded == NULL, "config_load_from_store should return NULL for incomplete config");
    }

    // 4. Test Load - no config file → should return NULL
    {
        unlink("/tmp/email-cli-test-home/.config/email-cli/accounts/test@test.com/config.ini");
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded == NULL, "config_load_from_store should return NULL when file is missing");
    }

    // 5. Test Save/Load with ssl_no_verify=1
    {
        Config cfg2 = {0};
        cfg2.host        = strdup("imaps://imap.test.com");
        cfg2.user        = strdup("test@test.com");
        cfg2.pass        = strdup("password123");
        cfg2.folder      = strdup("INBOX");
        cfg2.ssl_no_verify = 1;

        int res = config_save_to_store(&cfg2);
        ASSERT(res == 0, "config_save_to_store with ssl_no_verify=1 should return 0");

        RAII_WITH_CLEANUP(config_cleanup) Config *loaded2 = config_load_from_store();
        ASSERT(loaded2 != NULL, "config_load_from_store should not return NULL");
        ASSERT(loaded2->ssl_no_verify == 1, "ssl_no_verify should be 1 after load");

        free(cfg2.host);
        free(cfg2.user);
        free(cfg2.pass);
        free(cfg2.folder);
    }

    // 6. Test Load - host without protocol prefix → should return NULL with error
    {
        const char *acct_path =
            "/tmp/email-cli-test-home/.config/email-cli/accounts/test@test.com/config.ini";
        FILE *fp = fopen(acct_path, "w");
        if (fp) {
            fprintf(fp, "EMAIL_HOST=box.example.com\n");
            fprintf(fp, "EMAIL_USER=test@test.com\n");
            fprintf(fp, "EMAIL_PASS=password123\n");
            fclose(fp);
        }
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded == NULL, "config_load_from_store should return NULL for host without protocol");
        unlink(acct_path);
    }

    // 7. Test config_free with NULL (should not crash)
    config_free(NULL);

    // 8. config_save_to_store fails: fs_mkdir_p fails
    // Make .config directory non-writable so subdirectory creation fails
    {
        setenv("HOME", "/tmp/email-cli-test-home-fail", 1);
        fs_mkdir_p("/tmp/email-cli-test-home-fail/.config", 0700);
        chmod("/tmp/email-cli-test-home-fail/.config", 0000);

        Config cfg_fail = {0};
        cfg_fail.host   = strdup("imaps://imap.test.com");
        cfg_fail.user   = strdup("test@test.com");
        cfg_fail.pass   = strdup("password123");
        cfg_fail.folder = strdup("INBOX");

        int res = config_save_to_store(&cfg_fail);
        ASSERT(res == -1, "config_save_to_store: mkdir fail should return -1");

        chmod("/tmp/email-cli-test-home-fail/.config", 0700);
        free(cfg_fail.host);
        free(cfg_fail.user);
        free(cfg_fail.pass);
        free(cfg_fail.folder);
    }

    // 9. config_save_to_store fails: fopen fails
    // Create account dir, then place a directory at the config.ini path
    {
        setenv("HOME", "/tmp/email-cli-test-home-fopen", 1);
        fs_mkdir_p("/tmp/email-cli-test-home-fopen/.config/email-cli/accounts/test@test.com",
                   0700);
        // Create a directory at the config file path to make fopen("w") fail
        mkdir("/tmp/email-cli-test-home-fopen/.config/email-cli/accounts/test@test.com/config.ini",
              0700);

        Config cfg_fopen = {0};
        cfg_fopen.host   = strdup("imaps://imap.test.com");
        cfg_fopen.user   = strdup("test@test.com");
        cfg_fopen.pass   = strdup("password123");
        cfg_fopen.folder = strdup("INBOX");

        int res = config_save_to_store(&cfg_fopen);
        ASSERT(res == -1, "config_save_to_store: fopen on dir should return -1");

        rmdir("/tmp/email-cli-test-home-fopen/.config/email-cli/accounts/test@test.com/config.ini");
        free(cfg_fopen.host);
        free(cfg_fopen.user);
        free(cfg_fopen.pass);
        free(cfg_fopen.folder);
    }

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
    if (old_xdg) setenv("XDG_CONFIG_HOME", old_xdg, 1);
}
