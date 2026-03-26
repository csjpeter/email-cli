#include "test_helpers.h"
#include "config_store.h"
#include "fs_util.h"
#include "logger.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

void test_config_store(void) {
    char *old_home = getenv("HOME");
    setenv("HOME", "/tmp/email-cli-test-home", 1);
    fs_mkdir_p("/tmp/email-cli-test-home/.config/email-cli", 0700);

    // 1. Test Save
    Config cfg = {0};
    cfg.host   = strdup("imaps://imap.test.com");
    cfg.user   = strdup("test@test.com");
    cfg.pass   = strdup("password123");
    cfg.folder = strdup("INBOX");

    int res = config_save_to_store(&cfg);
    ASSERT(res == 0, "config_save_to_store should return 0");

    // 2. Test Load - full config
    Config *loaded = config_load_from_store();
    ASSERT(loaded != NULL, "config_load_from_store should not return NULL");
    ASSERT(strcmp(loaded->host, cfg.host) == 0, "Host should match");
    ASSERT(strcmp(loaded->user, cfg.user) == 0, "User should match");
    ASSERT(strcmp(loaded->pass, cfg.pass) == 0, "Pass should match");
    config_free(loaded);

    // 3. Test Load - incomplete config (missing host) → should return NULL
    FILE *fp = fopen("/tmp/email-cli-test-home/.config/email-cli/config.ini", "w");
    if (fp) {
        fprintf(fp, "EMAIL_USER=test@test.com\n");
        fprintf(fp, "EMAIL_PASS=password123\n");
        fclose(fp);
    }
    // Suppress the expected warning log
    logger_init("/tmp/email-cli-config-test.log", LOG_ERROR);
    loaded = config_load_from_store();
    ASSERT(loaded == NULL, "config_load_from_store should return NULL for incomplete config");
    logger_close();
    unlink("/tmp/email-cli-config-test.log");

    // 4. Test Load - no config file → should return NULL
    unlink("/tmp/email-cli-test-home/.config/email-cli/config.ini");
    loaded = config_load_from_store();
    ASSERT(loaded == NULL, "config_load_from_store should return NULL when file is missing");

    // 5. Test Save/Load with ssl_no_verify=1
    Config cfg2 = {0};
    cfg2.host        = strdup("imaps://imap.test.com");
    cfg2.user        = strdup("test@test.com");
    cfg2.pass        = strdup("password123");
    cfg2.folder      = strdup("INBOX");
    cfg2.ssl_no_verify = 1;

    res = config_save_to_store(&cfg2);
    ASSERT(res == 0, "config_save_to_store with ssl_no_verify=1 should return 0");

    Config *loaded2 = config_load_from_store();
    ASSERT(loaded2 != NULL, "config_load_from_store should not return NULL");
    ASSERT(loaded2->ssl_no_verify == 1, "ssl_no_verify should be 1 after load");
    config_free(loaded2);

    free(cfg2.host);
    free(cfg2.user);
    free(cfg2.pass);
    free(cfg2.folder);

    // 6. Test Load - host without protocol prefix → should return NULL with error
    fp = fopen("/tmp/email-cli-test-home/.config/email-cli/config.ini", "w");
    if (fp) {
        fprintf(fp, "EMAIL_HOST=box.example.com\n");
        fprintf(fp, "EMAIL_USER=test@test.com\n");
        fprintf(fp, "EMAIL_PASS=password123\n");
        fclose(fp);
    }
    logger_init("/tmp/email-cli-config-test.log", LOG_ERROR);
    loaded = config_load_from_store();
    ASSERT(loaded == NULL, "config_load_from_store should return NULL for host without protocol");
    logger_close();
    unlink("/tmp/email-cli-config-test.log");
    unlink("/tmp/email-cli-test-home/.config/email-cli/config.ini");

    // 7. Test config_free with NULL (should not crash)
    config_free(NULL);

    // Cleanup
    free(cfg.host);
    free(cfg.user);
    free(cfg.pass);
    free(cfg.folder);

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
}
