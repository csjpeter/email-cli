#include "test_helpers.h"
#include "setup_wizard.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void test_wizard(void) {
    // 1. Full valid input
    const char *input = "imaps://imap.test.com\ntest@user.com\nsecretpass\nMYFOLDER\n";
    FILE *stream = fmemopen((void*)input, strlen(input), "r");
    ASSERT(stream != NULL, "fmemopen should succeed");

    Config *cfg = setup_wizard_run_internal(stream);
    ASSERT(cfg != NULL, "setup_wizard_run_internal should return config");
    ASSERT(strcmp(cfg->host, "imaps://imap.test.com") == 0, "Host should match input");
    ASSERT(strcmp(cfg->user, "test@user.com") == 0, "User should match input");
    ASSERT(strcmp(cfg->pass, "secretpass") == 0, "Pass should match input");
    ASSERT(strcmp(cfg->folder, "MYFOLDER") == 0, "Folder should match input");
    config_free(cfg);
    fclose(stream);

    // 2. Empty folder defaults to INBOX
    const char *input2 = "h\nu\np\n\n";
    stream = fmemopen((void*)input2, strlen(input2), "r");
    cfg = setup_wizard_run_internal(stream);
    ASSERT(cfg != NULL, "setup_wizard should return config with empty folder");
    ASSERT(strcmp(cfg->folder, "INBOX") == 0, "Folder should default to INBOX");
    config_free(cfg);
    fclose(stream);

    // 3. EOF after host: user field missing → should return NULL
    const char *input3 = "imaps://imap.test.com\n";
    stream = fmemopen((void*)input3, strlen(input3), "r");
    ASSERT(stream != NULL, "fmemopen should succeed for partial input");
    cfg = setup_wizard_run_internal(stream);
    ASSERT(cfg == NULL, "setup_wizard should return NULL when user input is missing");
    fclose(stream);

    // 4. EOF after host+user: password field missing → should return NULL
    const char *input4 = "imaps://imap.test.com\ntest@user.com\n";
    stream = fmemopen((void*)input4, strlen(input4), "r");
    ASSERT(stream != NULL, "fmemopen should succeed for partial input");
    cfg = setup_wizard_run_internal(stream);
    ASSERT(cfg == NULL, "setup_wizard should return NULL when password is missing");
    fclose(stream);
}
