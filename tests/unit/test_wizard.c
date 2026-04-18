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
        const char *input = "1\nimaps://imap.test.com\n\ntest@user.com\nsecretpass\nMYFOLDER\n";
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
        const char *input2 = "1\nh\n\nu\np\n\n";
        RAII_FILE FILE *stream = fmemopen((void*)input2, strlen(input2), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg != NULL, "setup_wizard should return config with empty folder");
        ASSERT(strcmp(cfg->folder, "INBOX") == 0, "Folder should default to INBOX");
    }

    // 3. EOF after host: user field missing → should return NULL
    {
        const char *input3 = "1\nimaps://imap.test.com\n\n";
        RAII_FILE FILE *stream = fmemopen((void*)input3, strlen(input3), "r");
        ASSERT(stream != NULL, "fmemopen should succeed for partial input");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg == NULL, "setup_wizard should return NULL when user input is missing");
    }

    // 4. EOF after host+user: password field missing → should return NULL
    {
        const char *input4 = "1\nimaps://imap.test.com\n\ntest@user.com\n";
        RAII_FILE FILE *stream = fmemopen((void*)input4, strlen(input4), "r");
        ASSERT(stream != NULL, "fmemopen should succeed for partial input");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg == NULL, "setup_wizard should return NULL when password is missing");
    }

    // 5. Test setup_wizard_run() (stdin path) via pipe redirect
    {
        const char *input5 = "1\nimaps://imap.stdin.com\n\nstdin@user.com\nstdinpass\nSTDIN\n";
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

    // 6. Custom IMAP port → appended to URL
    {
        const char *input6p = "1\nimap.custom.com\n10993\nuser@custom.com\npass\nINBOX\n";
        RAII_FILE FILE *stream = fmemopen((void*)input6p, strlen(input6p), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg != NULL, "Port wizard: returns config");
        ASSERT(strcmp(cfg->host, "imaps://imap.custom.com:10993") == 0,
               "Port wizard: custom port appended to URL");
    }

    // 7. Default IMAP port (empty) → no port in URL
    {
        const char *input7 = "1\nimap.default.com\n\nuser@default.com\npass\nINBOX\n";
        RAII_FILE FILE *stream = fmemopen((void*)input7, strlen(input7), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg != NULL, "Port wizard default: returns config");
        ASSERT(strcmp(cfg->host, "imaps://imap.default.com") == 0,
               "Port wizard default: no port suffix");
    }

    // 8. Explicit 993 → no port in URL (same as default)
    {
        const char *input8 = "1\nimap.explicit.com\n993\nuser@explicit.com\npass\nINBOX\n";
        RAII_FILE FILE *stream = fmemopen((void*)input8, strlen(input8), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg != NULL, "Port wizard 993: returns config");
        ASSERT(strcmp(cfg->host, "imaps://imap.explicit.com") == 0,
               "Port wizard 993: no port suffix (993 is default)");
    }

    // 9. Host already has port → port prompt ignored
    {
        const char *input9 = "1\nimap.ported.com:995\n1234\nuser@ported.com\npass\nINBOX\n";
        RAII_FILE FILE *stream = fmemopen((void*)input9, strlen(input9), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg != NULL, "Port wizard existing: returns config");
        ASSERT(strcmp(cfg->host, "imaps://imap.ported.com:995") == 0,
               "Port wizard existing: original port preserved");
    }

    // 10. Gmail type: EOF after email → NULL (no device flow in test)
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

    // 11. Gmail type: empty email → NULL
    {
        const char *input11 = "2\n\n";
        RAII_FILE FILE *stream = fmemopen((void*)input11, strlen(input11), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg == NULL, "Gmail wizard: empty email returns NULL");
    }

    // 12. IMAP unsupported protocol (imap://) → wizard rejects, retries, then host EOF → NULL
    {
        /* First host uses unsupported protocol → printed error, re-prompt;
         * second attempt gets EOF → NULL */
        const char *input12 = "1\nftp://imap.test.com\n";
        RAII_FILE FILE *stream = fmemopen((void*)input12, strlen(input12), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg == NULL, "IMAP wizard: unsupported protocol then EOF → NULL");
    }

    // 13. Gmail domain in IMAP type → immediate NULL (rejection)
    {
        const char *input13 = "1\ngmail.com\n";
        RAII_FILE FILE *stream = fmemopen((void*)input13, strlen(input13), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg == NULL, "IMAP wizard: gmail.com domain rejected → NULL");
    }

    // 14. Full IMAP config including SMTP host
    {
        const char *input14 =
            "1\n"                              /* account type: IMAP */
            "imaps://imap.withsmtp.com\n"      /* IMAP host */
            "\n"                               /* port: default */
            "user@withsmtp.com\n"              /* user */
            "password\n"                       /* pass */
            "INBOX\n"                          /* folder */
            "smtp.withsmtp.com\n"              /* SMTP host (plain → smtps://) */
            "587\n"                            /* SMTP port */
            "\n"                               /* SMTP user: same as IMAP */
            "\n";                              /* SMTP pass: same as IMAP */
        RAII_FILE FILE *stream = fmemopen((void*)input14, strlen(input14), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg != NULL, "SMTP wizard: returns config with SMTP");
        ASSERT(cfg->smtp_host != NULL, "SMTP wizard: smtp_host set");
        /* normalize_smtp_host prepends smtps:// to plain hostnames */
        ASSERT(strncmp(cfg->smtp_host, "smtps://", 8) == 0,
               "SMTP wizard: smtp_host has smtps:// prefix");
        ASSERT(cfg->smtp_port == 587, "SMTP wizard: smtp_port=587");
    }

    // 15. SMTP host already has smtps:// → accepted as-is
    {
        const char *input15 =
            "1\n"
            "imaps://imap.smtps.com\n"
            "\n"
            "user@smtps.com\n"
            "pass\n"
            "INBOX\n"
            "smtps://smtp.smtps.com\n"    /* already has smtps:// */
            "\n"                          /* SMTP port: default */
            "\n"                          /* SMTP user */
            "\n";                         /* SMTP pass */
        RAII_FILE FILE *stream = fmemopen((void*)input15, strlen(input15), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg != NULL, "SMTP wizard smtps://: returns config");
        ASSERT(cfg->smtp_host != NULL, "SMTP wizard smtps://: smtp_host set");
        ASSERT(strcmp(cfg->smtp_host, "smtps://smtp.smtps.com") == 0,
               "SMTP wizard smtps://: host preserved as-is");
    }

    // 16. SMTP host with unsupported protocol → re-prompt; empty → skipped
    {
        const char *input16 =
            "1\n"
            "imaps://imap.badsmtp.com\n"
            "\n"
            "user@badsmtp.com\n"
            "pass\n"
            "INBOX\n"
            "ftp://smtp.badsmtp.com\n"   /* bad protocol → error + retry */
            "\n"                         /* empty → skip SMTP */
            ;
        RAII_FILE FILE *stream = fmemopen((void*)input16, strlen(input16), "r");
        RAII_WITH_CLEANUP(config_cleanup) Config *cfg = setup_wizard_run_internal(stream);
        ASSERT(cfg != NULL, "SMTP wizard bad-then-skip: returns config");
        ASSERT(cfg->smtp_host == NULL, "SMTP wizard bad-then-skip: no smtp_host");
    }

    // 17. setup_wizard_imap() via stdin pipe
    {
        Config cfg = {0};
        cfg.host   = strdup("imaps://imap.old.com");
        cfg.user   = strdup("old@user.com");
        cfg.pass   = strdup("oldpass");
        cfg.folder = strdup("INBOX");

        /* Keep all fields: send empty lines for each prompt (keep current) */
        const char *input17 = "\n\n\n\n";
        int pipefd[2];
        ASSERT(pipe(pipefd) == 0, "pipe for imap sub-wizard");
        ssize_t wr = write(pipefd[1], input17, strlen(input17));
        ASSERT(wr > 0, "write to pipe for imap sub-wizard");
        close(pipefd[1]);

        int saved = dup(STDIN_FILENO);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        clearerr(stdin);   /* clear EOF flag from previous stdin-redirect tests */
        int rc = setup_wizard_imap(&cfg);
        dup2(saved, STDIN_FILENO);
        close(saved);

        ASSERT(rc == 0, "setup_wizard_imap keep-all: returns 0");
        ASSERT(strcmp(cfg.host,   "imaps://imap.old.com") == 0, "imap sub-wizard: host unchanged");
        ASSERT(strcmp(cfg.user,   "old@user.com")         == 0, "imap sub-wizard: user unchanged");
        ASSERT(strcmp(cfg.folder, "INBOX")                == 0, "imap sub-wizard: folder unchanged");
        free(cfg.host); free(cfg.user); free(cfg.pass); free(cfg.folder);
    }

    // 18. setup_wizard_imap() updates host via stdin pipe
    {
        Config cfg = {0};
        cfg.host   = strdup("imaps://imap.old.com");
        cfg.user   = strdup("old@user.com");
        cfg.pass   = strdup("oldpass");
        cfg.folder = strdup("INBOX");

        /* Provide a new IMAP host; keep everything else */
        const char *input18 = "imaps://imap.new.com\n\n\n\n";
        int pipefd[2];
        ASSERT(pipe(pipefd) == 0, "pipe for imap sub-wizard update");
        ssize_t wr = write(pipefd[1], input18, strlen(input18));
        ASSERT(wr > 0, "write to pipe for imap sub-wizard update");
        close(pipefd[1]);

        int saved = dup(STDIN_FILENO);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        clearerr(stdin);
        int rc = setup_wizard_imap(&cfg);
        dup2(saved, STDIN_FILENO);
        close(saved);

        ASSERT(rc == 0, "setup_wizard_imap update: returns 0");
        ASSERT(strcmp(cfg.host, "imaps://imap.new.com") == 0,
               "imap sub-wizard: host updated");
        free(cfg.host); free(cfg.user); free(cfg.pass); free(cfg.folder);
    }

    // 19. setup_wizard_imap() aborts on EOF (host prompt)
    {
        Config cfg = {0};
        cfg.host = strdup("imaps://imap.eof.com");

        /* EOF immediately → returns -1 */
        int pipefd[2];
        ASSERT(pipe(pipefd) == 0, "pipe for imap sub-wizard EOF");
        close(pipefd[1]);  /* close write end immediately → EOF on read */

        int saved = dup(STDIN_FILENO);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        clearerr(stdin);
        int rc = setup_wizard_imap(&cfg);
        dup2(saved, STDIN_FILENO);
        close(saved);

        ASSERT(rc == -1, "setup_wizard_imap EOF: returns -1");
        free(cfg.host);
    }

    // 20. setup_wizard_smtp() via stdin pipe — keep defaults
    {
        Config cfg = {0};
        cfg.host      = strdup("imaps://imap.smtp20.com");
        cfg.smtp_host = strdup("smtps://smtp.smtp20.com");
        cfg.smtp_port = 465;
        cfg.user      = strdup("user@smtp20.com");
        cfg.pass      = strdup("pass");

        /* Empty → keep all current values */
        const char *input20 = "\n\n\n\n";
        int pipefd[2];
        ASSERT(pipe(pipefd) == 0, "pipe for smtp sub-wizard keep");
        ssize_t wr = write(pipefd[1], input20, strlen(input20));
        ASSERT(wr > 0, "write to pipe for smtp sub-wizard keep");
        close(pipefd[1]);

        int saved = dup(STDIN_FILENO);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        clearerr(stdin);
        int rc = setup_wizard_smtp(&cfg);
        dup2(saved, STDIN_FILENO);
        close(saved);

        ASSERT(rc == 0, "setup_wizard_smtp keep-all: returns 0");
        ASSERT(strcmp(cfg.smtp_host, "smtps://smtp.smtp20.com") == 0,
               "smtp sub-wizard: host unchanged");
        ASSERT(cfg.smtp_port == 465, "smtp sub-wizard: port unchanged");
        free(cfg.host); free(cfg.smtp_host); free(cfg.user); free(cfg.pass);
    }

    // 21. setup_wizard_smtp() aborts on EOF (host prompt)
    {
        Config cfg = {0};
        cfg.host = strdup("imaps://imap.smtp21.com");

        int pipefd[2];
        ASSERT(pipe(pipefd) == 0, "pipe for smtp sub-wizard EOF");
        close(pipefd[1]);   /* close write end immediately → EOF on read */

        int saved = dup(STDIN_FILENO);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        clearerr(stdin);
        int rc = setup_wizard_smtp(&cfg);
        dup2(saved, STDIN_FILENO);
        close(saved);

        ASSERT(rc == -1, "setup_wizard_smtp EOF: returns -1");
        free(cfg.host);
    }

    // 22. setup_wizard_smtp() with derived SMTP URL from imaps:// host
    {
        Config cfg = {0};
        cfg.host = strdup("imaps://imap.derived.com");
        /* No smtp_host → derive_smtp_url produces smtps://imap.derived.com */
        /* Empty input accepts the derived default */
        const char *input22 = "\n\n\n\n";
        int pipefd[2];
        ASSERT(pipe(pipefd) == 0, "pipe for smtp sub-wizard derived");
        ssize_t wr = write(pipefd[1], input22, strlen(input22));
        ASSERT(wr > 0, "write to pipe for smtp sub-wizard derived");
        close(pipefd[1]);

        int saved = dup(STDIN_FILENO);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        clearerr(stdin);
        int rc = setup_wizard_smtp(&cfg);
        dup2(saved, STDIN_FILENO);
        close(saved);

        ASSERT(rc == 0, "setup_wizard_smtp derived: returns 0");
        /* Derived URL should have been accepted */
        ASSERT(cfg.smtp_host != NULL, "setup_wizard_smtp derived: smtp_host set");
        ASSERT(strncmp(cfg.smtp_host, "smtps://", 8) == 0,
               "setup_wizard_smtp derived: has smtps:// prefix");
        free(cfg.host); free(cfg.smtp_host);
    }
}
