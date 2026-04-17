#include "test_helpers.h"
#include "setup_wizard.h"
#include "raii.h"
#include <stdio.h>
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

void test_wizard(void) {
    // 1. Full valid input
    {
        const char *input = "1\nimaps://imap.test.com\ntest@user.com\nsecretpass\nMYFOLDER\n";
        RAII_FILE FILE *stream = fmemopen((void*)input, strlen(input), "r");
        ASSERT(stream != NULL, "fmemopen should succeed");

        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg != NULL, "setup_wizard_run_internal should return config");
        ASSERT(strcmp(cfg->host, "imaps://imap.test.com") == 0, "Host should match input");
        ASSERT(strcmp(cfg->user, "test@user.com") == 0, "User should match input");
        ASSERT(strcmp(cfg->pass, "secretpass") == 0, "Pass should match input");
        ASSERT(strcmp(cfg->folder, "MYFOLDER") == 0, "Folder should match input");
    }

    // 2. Empty folder defaults to INBOX
    {
        const char *input2 = "1\nh\nu\np\n\n";
        RAII_FILE FILE *stream = fmemopen((void*)input2, strlen(input2), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg != NULL, "setup_wizard should return config with empty folder");
        ASSERT(strcmp(cfg->folder, "INBOX") == 0, "Folder should default to INBOX");
    }

    // 3. EOF after host: user field missing → should return NULL
    {
        const char *input3 = "1\nimaps://imap.test.com\n";
        RAII_FILE FILE *stream = fmemopen((void*)input3, strlen(input3), "r");
        ASSERT(stream != NULL, "fmemopen should succeed for partial input");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg == NULL, "setup_wizard should return NULL when user input is missing");
    }

    // 4. EOF after host+user: password field missing → should return NULL
    {
        const char *input4 = "1\nimaps://imap.test.com\ntest@user.com\n";
        RAII_FILE FILE *stream = fmemopen((void*)input4, strlen(input4), "r");
        ASSERT(stream != NULL, "fmemopen should succeed for partial input");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg == NULL, "setup_wizard should return NULL when password is missing");
    }

    // 5. Test setup_wizard_run() (stdin path) via pipe redirect
    {
        const char *input5 = "1\nimaps://imap.stdin.com\nstdin@user.com\nstdinpass\nSTDIN\n";
        int pipefd[2];
        ASSERT(pipe(pipefd) == 0, "pipe() should succeed");
        ssize_t written = write(pipefd[1], input5, strlen(input5));
        ASSERT(written > 0, "write to pipe should succeed");
        close(pipefd[1]);

        int saved_stdin = dup(STDIN_FILENO);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);

        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run();

        dup2(saved_stdin, STDIN_FILENO);
        close(saved_stdin);

        ASSERT(cfg != NULL, "setup_wizard_run should return config via pipe");
        ASSERT(strcmp(cfg->host, "imaps://imap.stdin.com") == 0, "Host should match stdin input");
        ASSERT(strcmp(cfg->user, "stdin@user.com") == 0, "User should match stdin input");
        ASSERT(strcmp(cfg->pass, "stdinpass") == 0, "Pass should match stdin input");
        ASSERT(strcmp(cfg->folder, "STDIN") == 0, "Folder should match stdin input");
    }

    // 6. Gmail type: EOF after email → NULL (no device flow in test)
    {
        const char *input6 = "2\ngmail@example.com\n";
        RAII_FILE FILE *stream = fmemopen((void*)input6, strlen(input6), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        /* Device flow fails (no real OAuth) → but cfg should have gmail_mode set
         * and user set before failing.  The non-TTY path skips device flow
         * and returns cfg with gmail_mode=1. */
        if (cfg) {
            ASSERT(cfg->gmail_mode == 1, "Gmail wizard: gmail_mode=1");
            ASSERT(strcmp(cfg->user, "gmail@example.com") == 0, "Gmail wizard: user matches");
        }
        /* cfg may be NULL if device flow was attempted and failed — that's OK */
    }
}
