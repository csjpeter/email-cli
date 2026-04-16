/**
 * @file main_sync.c
 * @brief Entry point for email-sync — background sync binary with cron management.
 *
 * email-sync downloads all messages from all IMAP folders to the local store.
 * It also manages its own cron scheduling via the 'cron' subcommand.
 *
 * Typical crontab entry (installed by 'email-sync cron setup'):
 *   *\/15 * * * * /path/to/email-sync >> ~/.cache/email-cli/sync.log 2>&1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include "config_store.h"
#include "email_service.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include "local_store.h"
#include "fs_util.h"

static void help(void) {
    printf(
        "Usage: email-sync [--account <email>]\n"
        "       email-sync cron <setup|remove|status>\n"
        "\n"
        "Downloads all messages from all IMAP folders to the local store.\n"
        "Without --account, every configured account is synced in alphabetical\n"
        "order.  With --account, only the specified account is synced.\n"
        "Messages already stored locally are skipped.\n"
        "\n"
        "Subcommands:\n"
        "  cron setup    Install a crontab entry to run email-sync periodically.\n"
        "                Uses SYNC_INTERVAL from config (default: 5 minutes).\n"
        "  cron remove   Remove the email-sync crontab entry.\n"
        "  cron status   Show whether an automatic sync entry is installed.\n"
        "\n"
        "Options:\n"
        "  --account <email>   Sync only the account with this email address\n"
        "  --help, -h          Show this help message\n"
        "\n"
        "Exit Codes:\n"
        "  0   Completed successfully (or already up to date)\n"
        "  1   Error (no config, connection failure, etc.)\n"
    );
}

static void help_cron(void) {
    printf(
        "Usage: email-sync cron <setup|remove|status>\n"
        "\n"
        "Manages automatic background synchronisation via user crontab.\n"
        "No sudo or system-level access is required.\n"
        "\n"
        "Subcommands:\n"
        "  setup    Install a crontab entry to run email-sync periodically.\n"
        "           Uses SYNC_INTERVAL from config (default: 5 minutes if not set).\n"
        "           Saves the interval to config if a default was applied.\n"
        "  remove   Remove the email-sync crontab entry.\n"
        "  status   Show whether an automatic sync entry is currently installed.\n"
        "\n"
        "Examples:\n"
        "  email-sync cron setup\n"
        "  email-sync cron status\n"
        "  email-sync cron remove\n"
    );
}

#ifndef EMAIL_CLI_VERSION
#define EMAIL_CLI_VERSION "unknown"
#endif

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    const char *cmd = argc > 1 ? argv[1] : NULL;

    if (cmd && (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0)) {
        printf("email-sync %s\n", EMAIL_CLI_VERSION);
        return EXIT_SUCCESS;
    }

    /* Handle --help / -h */
    if (cmd && (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0)) {
        help();
        return EXIT_SUCCESS;
    }

    /* cron status and remove don't need config — handle early */
    if (cmd && strcmp(cmd, "cron") == 0) {
        const char *subcmd = argc > 2 ? argv[2] : "";
        if (strcmp(subcmd, "--help") == 0 || strcmp(subcmd, "-h") == 0) {
            help_cron();
            return EXIT_SUCCESS;
        }
        if (strcmp(subcmd, "status") == 0)
            return email_service_cron_status() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        if (strcmp(subcmd, "remove") == 0)
            return email_service_cron_remove() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        if (strcmp(subcmd, "setup") != 0) {
            fprintf(stderr, "Usage: email-sync cron <setup|remove|status>\n");
            return EXIT_FAILURE;
        }
        /* 'cron setup' falls through — needs config */
    }

    /* Determine cache directory for logs */
    const char *cache_base = platform_cache_dir();
    if (!cache_base) {
        fprintf(stderr, "Fatal: Could not determine cache directory.\n");
        return EXIT_FAILURE;
    }

    RAII_STRING char *log_dir  = NULL;
    RAII_STRING char *log_file = NULL;
    if (asprintf(&log_dir,  "%s/email-cli/logs", cache_base) == -1 ||
        asprintf(&log_file, "%s/sync-session.log", log_dir)  == -1) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return EXIT_FAILURE;
    }

    /* Initialize logger */
    if (fs_mkdir_p(log_dir, 0700) != 0)
        fprintf(stderr, "Warning: Could not create log directory %s\n", log_dir);
    if (logger_init(log_file, LOG_DEBUG) != 0)
        fprintf(stderr, "Warning: Logging system failed to initialize.\n");

    /* 'cron setup' needs config for sync_interval */
    if (cmd && strcmp(cmd, "cron") == 0) {
        logger_log(LOG_INFO, "--- email-sync cron setup ---");
        Config *cfg = config_load_from_store();
        if (!cfg) {
            fprintf(stderr, "Error: No configuration found. Run 'email-cli config' or "
                            "the setup wizard first.\n");
            logger_close();
            return EXIT_FAILURE;
        }
        if (cfg->sync_interval <= 0) {
            cfg->sync_interval = 5;
            printf("sync_interval not configured; using default of 5 minutes.\n");
            if (config_save_to_store(cfg) != 0)
                fprintf(stderr, "Warning: could not save sync_interval to config.\n");
        }
        int rc = email_service_cron_setup(cfg);
        config_free(cfg);
        logger_close();
        return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    logger_log(LOG_INFO, "--- email-sync starting ---");

    /* Parse remaining options (sync mode) */
    const char *account_filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--account") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --account requires an email address.\n");
                logger_close();
                return EXIT_FAILURE;
            }
            account_filter = argv[++i];
            continue;
        }
        fprintf(stderr, "Unknown option '%s'.\nRun 'email-sync --help' for usage.\n",
                argv[i]);
        logger_close();
        return EXIT_FAILURE;
    }

    /* Run sync (handles account loading, local_store_init, and iteration) */
    int result = email_service_sync_all(account_filter);

    logger_log(LOG_INFO, "--- email-sync finished (result: %d) ---", result);
    logger_close();

    if (result >= 0)
        return EXIT_SUCCESS;
    fprintf(stderr, "\nSync failed. Check logs in %s\n", log_file);
    return EXIT_FAILURE;
}
