#include "test_helpers.h"
#include "config_store.h"
#include "fs_util.h"
#include "logger.h"
#include "raii.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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
    unsetenv("XDG_CONFIG_HOME");   /* ensure we use the HOME-based path */
    fs_mkdir_p("/tmp/email-cli-test-home/.config/email-cli", 0700);

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
        FILE *fp = fopen("/tmp/email-cli-test-home/.config/email-cli/config.ini", "w");
        if (fp) {
            fprintf(fp, "EMAIL_USER=test@test.com\n");
            fprintf(fp, "EMAIL_PASS=password123\n");
            fclose(fp);
        }
        /* logger not initialised → logger_log() returns early, no output */
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded == NULL, "config_load_from_store should return NULL for incomplete config");
    }

    // 4. Test Load - no config file → should return NULL
    {
        unlink("/tmp/email-cli-test-home/.config/email-cli/config.ini");
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
        FILE *fp = fopen("/tmp/email-cli-test-home/.config/email-cli/config.ini", "w");
        if (fp) {
            fprintf(fp, "EMAIL_HOST=box.example.com\n");
            fprintf(fp, "EMAIL_USER=test@test.com\n");
            fprintf(fp, "EMAIL_PASS=password123\n");
            fclose(fp);
        }
        /* logger not initialised → logger_log() returns early, no output */
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded == NULL, "config_load_from_store should return NULL for host without protocol");
        unlink("/tmp/email-cli-test-home/.config/email-cli/config.ini");
    }

    // 7. Test config_free with NULL (should not crash)
    config_free(NULL);

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
    if (old_xdg) setenv("XDG_CONFIG_HOME", old_xdg, 1);
}
