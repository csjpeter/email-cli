#include "test_helpers.h"
#include "setup_wizard.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void test_wizard(void) {
    // Simulate user input using a memory stream
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

    // Test default folder case
    const char *input2 = "h\nu\np\n\n";
    stream = fmemopen((void*)input2, strlen(input2), "r");
    cfg = setup_wizard_run_internal(stream);
    ASSERT(strcmp(cfg->folder, "INBOX") == 0, "Folder should default to INBOX");
    config_free(cfg);
    fclose(stream);
}
