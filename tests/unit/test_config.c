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

    // 10. SMTP fields round-trip (covers lines 79-82 in config_store.c)
    {
        setenv("HOME", "/tmp/email-cli-test-home", 1);
        const char *acct_path =
            "/tmp/email-cli-test-home/.config/email-cli/accounts/test@test.com/config.ini";
        FILE *fp = fopen(acct_path, "w");
        if (fp) {
            fprintf(fp, "EMAIL_HOST=imaps://imap.test.com\n");
            fprintf(fp, "EMAIL_USER=test@test.com\n");
            fprintf(fp, "EMAIL_PASS=password123\n");
            fprintf(fp, "SMTP_HOST=smtps://smtp.test.com\n");
            fprintf(fp, "SMTP_PORT=465\n");
            fprintf(fp, "SMTP_USER=test@test.com\n");
            fprintf(fp, "SMTP_PASS=smtppass\n");
            fclose(fp);
        }
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded != NULL, "smtp fields: should load");
        if (loaded) {
            ASSERT(loaded->smtp_host && strcmp(loaded->smtp_host, "smtps://smtp.test.com") == 0,
                   "smtp fields: smtp_host matches");
            ASSERT(loaded->smtp_port == 465, "smtp fields: smtp_port matches");
            ASSERT(loaded->smtp_user && strcmp(loaded->smtp_user, "test@test.com") == 0,
                   "smtp fields: smtp_user matches");
            ASSERT(loaded->smtp_pass && strcmp(loaded->smtp_pass, "smtppass") == 0,
                   "smtp fields: smtp_pass matches");
        }
        unlink(acct_path);
    }

    // 11. Gmail mode config load (covers lines 83-86 in config_store.c)
    {
        setenv("HOME", "/tmp/email-cli-test-home", 1);
        const char *acct_path =
            "/tmp/email-cli-test-home/.config/email-cli/accounts/test@test.com/config.ini";
        FILE *fp = fopen(acct_path, "w");
        if (fp) {
            fprintf(fp, "EMAIL_USER=gmailuser@gmail.com\n");
            fprintf(fp, "GMAIL_MODE=1\n");
            fprintf(fp, "GMAIL_REFRESH_TOKEN=myrefreshtoken\n");
            fprintf(fp, "GMAIL_CLIENT_ID=myclientid\n");
            fprintf(fp, "GMAIL_CLIENT_SECRET=myclientsecret\n");
            fclose(fp);
        }
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded != NULL, "gmail mode: should load");
        if (loaded) {
            ASSERT(loaded->gmail_mode == 1, "gmail mode: gmail_mode=1");
            ASSERT(loaded->gmail_refresh_token &&
                   strcmp(loaded->gmail_refresh_token, "myrefreshtoken") == 0,
                   "gmail mode: refresh token matches");
            ASSERT(loaded->gmail_client_id &&
                   strcmp(loaded->gmail_client_id, "myclientid") == 0,
                   "gmail mode: client_id matches");
            ASSERT(loaded->gmail_client_secret &&
                   strcmp(loaded->gmail_client_secret, "myclientsecret") == 0,
                   "gmail mode: client_secret matches");
        }
        unlink(acct_path);
    }

    // 12. Gmail mode: missing refresh token → NULL (covers line 92)
    {
        setenv("HOME", "/tmp/email-cli-test-home", 1);
        const char *acct_path =
            "/tmp/email-cli-test-home/.config/email-cli/accounts/test@test.com/config.ini";
        FILE *fp = fopen(acct_path, "w");
        if (fp) {
            fprintf(fp, "EMAIL_USER=gmailuser@gmail.com\n");
            fprintf(fp, "GMAIL_MODE=1\n");
            /* no GMAIL_REFRESH_TOKEN */
            fclose(fp);
        }
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded == NULL,
               "gmail missing token: config_load should return NULL");
        unlink(acct_path);
    }

    // 13. SMTP host without smtps:// prefix → rejected (covers lines 110-118)
    {
        setenv("HOME", "/tmp/email-cli-test-home", 1);
        const char *acct_path =
            "/tmp/email-cli-test-home/.config/email-cli/accounts/test@test.com/config.ini";
        FILE *fp = fopen(acct_path, "w");
        if (fp) {
            fprintf(fp, "EMAIL_HOST=imaps://imap.test.com\n");
            fprintf(fp, "EMAIL_USER=test@test.com\n");
            fprintf(fp, "EMAIL_PASS=password123\n");
            fprintf(fp, "SMTP_HOST=smtp.test.com\n");  /* missing smtps:// */
            fclose(fp);
        }
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded == NULL,
               "insecure smtp host: config_load should return NULL");
        unlink(acct_path);
    }

    // 14. ssl_no_verify=1 with non-TLS host and SMTP → warnings logged
    //     (covers lines 124 and 128-129)
    {
        setenv("HOME", "/tmp/email-cli-test-home", 1);
        const char *acct_path =
            "/tmp/email-cli-test-home/.config/email-cli/accounts/test@test.com/config.ini";
        FILE *fp = fopen(acct_path, "w");
        if (fp) {
            fprintf(fp, "EMAIL_HOST=imap.insecure.com\n");
            fprintf(fp, "EMAIL_USER=test@test.com\n");
            fprintf(fp, "EMAIL_PASS=password123\n");
            fprintf(fp, "SSL_NO_VERIFY=1\n");
            fprintf(fp, "SMTP_HOST=smtp.insecure.com\n");
            fclose(fp);
        }
        RAII_WITH_CLEANUP(config_cleanup) Config *loaded = config_load_from_store();
        ASSERT(loaded != NULL,
               "ssl_no_verify non-tls: should still load (warnings only)");
        if (loaded) {
            ASSERT(loaded->ssl_no_verify == 1, "ssl_no_verify non-tls: flag set");
        }
        unlink(acct_path);
    }

    // 15. config_delete_account: non-empty dir → rmdir fails, returns -1
    //     (covers lines 194-195)
    {
        setenv("HOME", "/tmp/email-cli-test-home", 1);
        const char *dir =
            "/tmp/email-cli-test-home/.config/email-cli/accounts/nodelete@test.com";
        const char *extra_file =
            "/tmp/email-cli-test-home/.config/email-cli/accounts/nodelete@test.com/extra.txt";
        fs_mkdir_p(dir, 0700);
        FILE *fp = fopen(extra_file, "w");
        if (fp) { fprintf(fp, "extra\n"); fclose(fp); }

        int rc = config_delete_account("nodelete@test.com");
        ASSERT(rc == -1, "delete non-empty dir: should return -1");

        /* cleanup */
        unlink(extra_file);
        rmdir(dir);
    }

    // 16. Multiple accounts → sorting path exercised (covers lines 243-264)
    {
        setenv("HOME", "/tmp/email-cli-test-multiacct", 1);
        unsetenv("XDG_CONFIG_HOME");
        const char *base =
            "/tmp/email-cli-test-multiacct/.config/email-cli/accounts";

        /* Account 1: bob@beta.com — different domain */
        char path[256];
        snprintf(path, sizeof(path), "%s/bob@beta.com", base);
        fs_mkdir_p(path, 0700);
        snprintf(path, sizeof(path), "%s/bob@beta.com/config.ini", base);
        {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "EMAIL_HOST=imaps://imap.beta.com\n");
                fprintf(fp, "EMAIL_USER=bob@beta.com\n");
                fprintf(fp, "EMAIL_PASS=pass1\n");
                fclose(fp);
            }
        }
        /* Account 2: alice@alpha.com — different domain */
        snprintf(path, sizeof(path), "%s/alice@alpha.com", base);
        fs_mkdir_p(path, 0700);
        snprintf(path, sizeof(path), "%s/alice@alpha.com/config.ini", base);
        {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "EMAIL_HOST=imaps://imap.alpha.com\n");
                fprintf(fp, "EMAIL_USER=alice@alpha.com\n");
                fprintf(fp, "EMAIL_PASS=pass2\n");
                fclose(fp);
            }
        }
        /* Account 3: zara@alpha.com — same domain as alice → hits same-domain
         * sort branch (lines 256-259). zara > alice alphabetically, so alice
         * should sort before zara. */
        snprintf(path, sizeof(path), "%s/zara@alpha.com", base);
        fs_mkdir_p(path, 0700);
        snprintf(path, sizeof(path), "%s/zara@alpha.com/config.ini", base);
        {
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "EMAIL_HOST=imaps://imap.alpha.com\n");
                fprintf(fp, "EMAIL_USER=zara@alpha.com\n");
                fprintf(fp, "EMAIL_PASS=pass3\n");
                fclose(fp);
            }
        }

        int count = 0;
        AccountEntry *list = config_list_accounts(&count);
        ASSERT(count == 3, "multi-acct sort: count=3");
        if (list && count >= 3) {
            /* After sorting: alpha.com accounts first (alice, zara), then beta.com */
            ASSERT(strstr(list[0].name, "alpha.com") != NULL,
                   "multi-acct sort: first account is alpha.com");
            ASSERT(strstr(list[1].name, "alpha.com") != NULL,
                   "multi-acct sort: second account is alpha.com");
            /* Within alpha.com: alice < zara */
            ASSERT(strncmp(list[0].name, "alice", 5) == 0,
                   "multi-acct sort: alice before zara");
            ASSERT(strstr(list[2].name, "beta.com") != NULL,
                   "multi-acct sort: beta.com last");
        }
        config_free_account_list(list, count);

        /* cleanup */
        unlink("/tmp/email-cli-test-multiacct/.config/email-cli/accounts/bob@beta.com/config.ini");
        rmdir("/tmp/email-cli-test-multiacct/.config/email-cli/accounts/bob@beta.com");
        unlink("/tmp/email-cli-test-multiacct/.config/email-cli/accounts/alice@alpha.com/config.ini");
        rmdir("/tmp/email-cli-test-multiacct/.config/email-cli/accounts/alice@alpha.com");
        unlink("/tmp/email-cli-test-multiacct/.config/email-cli/accounts/zara@alpha.com/config.ini");
        rmdir("/tmp/email-cli-test-multiacct/.config/email-cli/accounts/zara@alpha.com");
    }

    // 17. config_list_accounts realloc path: create 9 accounts (cap starts at
    //     8, so the 9th triggers realloc) — covers lines 227-230
    {
        setenv("HOME", "/tmp/email-cli-test-manyacct", 1);
        unsetenv("XDG_CONFIG_HOME");
        const char *base =
            "/tmp/email-cli-test-manyacct/.config/email-cli/accounts";

        /* usernames chosen to produce a valid IMAP host pattern */
        const char *users[] = {
            "u1@svc.io", "u2@svc.io", "u3@svc.io", "u4@svc.io",
            "u5@svc.io", "u6@svc.io", "u7@svc.io", "u8@svc.io",
            "u9@svc.io"
        };
        int nacct = (int)(sizeof(users) / sizeof(users[0]));
        char path[256];
        for (int i = 0; i < nacct; i++) {
            snprintf(path, sizeof(path), "%s/%s", base, users[i]);
            fs_mkdir_p(path, 0700);
            snprintf(path, sizeof(path), "%s/%s/config.ini", base, users[i]);
            FILE *fp = fopen(path, "w");
            if (fp) {
                fprintf(fp, "EMAIL_HOST=imaps://imap.svc.io\n");
                fprintf(fp, "EMAIL_USER=%s\n", users[i]);
                fprintf(fp, "EMAIL_PASS=pass\n");
                fclose(fp);
            }
        }

        int count = 0;
        AccountEntry *list = config_list_accounts(&count);
        ASSERT(count == nacct, "many accounts: count=9");
        config_free_account_list(list, count);

        /* cleanup */
        for (int i = 0; i < nacct; i++) {
            snprintf(path, sizeof(path),
                     "/tmp/email-cli-test-manyacct/.config/email-cli/accounts/%s/config.ini",
                     users[i]);
            unlink(path);
            snprintf(path, sizeof(path),
                     "/tmp/email-cli-test-manyacct/.config/email-cli/accounts/%s",
                     users[i]);
            rmdir(path);
        }
    }

    if (old_home) setenv("HOME", old_home, 1);
    else unsetenv("HOME");
    if (old_xdg) setenv("XDG_CONFIG_HOME", old_xdg, 1);
}
