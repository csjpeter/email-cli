/**
 * @file main_sync.c
 * @brief Entry point for email-sync — single-purpose background sync binary.
 *
 * email-sync downloads all messages from all IMAP folders to the local store.
 * It is designed to be called from cron (installed by 'email-cli cron setup')
 * or run directly by the user. It has no subcommands and no interactive mode.
 *
 * Typical crontab entry (installed by 'email-cli cron setup'):
 *   * /15 * * * * /path/to/email-sync >> ~/.cache/email-cli/sync.log 2>&1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include "email_service.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include "fs_util.h"

static void help(void) {
    printf(
        "Usage: email-sync [--account <email>]\n"
        "\n"
        "Downloads all messages from all IMAP folders to the local store.\n"
        "Without --account, every configured account is synced in alphabetical\n"
        "order.  With --account, only the specified account is synced.\n"
        "Messages already stored locally are skipped.\n"
        "\n"
        "Designed to be run from cron (installed by 'email-cli cron setup').\n"
        "\n"
        "Options:\n"
        "  --account <email>   Sync only the account with this email address\n"
        "  --help, -h          Show this help message\n"
        "\n"
        "Exit Codes:\n"
        "  0   Sync completed successfully (or already up to date)\n"
        "  1   Error (no config, connection failure, etc.)\n"
    );
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    const char *account_filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            help();
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--account") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --account requires an email address.\n");
                return EXIT_FAILURE;
            }
            account_filter = argv[++i];
            continue;
        }
        fprintf(stderr, "Unknown option '%s'.\nRun 'email-sync --help' for usage.\n",
                argv[i]);
        return EXIT_FAILURE;
    }

    /* 1. Determine cache directory for logs */
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

    /* 2. Initialize logger */
    if (fs_mkdir_p(log_dir, 0700) != 0)
        fprintf(stderr, "Warning: Could not create log directory %s\n", log_dir);
    if (logger_init(log_file, LOG_DEBUG) != 0)
        fprintf(stderr, "Warning: Logging system failed to initialize.\n");
    logger_log(LOG_INFO, "--- email-sync starting ---");

    /* 3. Run sync (handles account loading, local_store_init, and iteration) */
    int result = email_service_sync_all(account_filter);

    logger_log(LOG_INFO, "--- email-sync finished (result: %d) ---", result);
    logger_close();

    if (result >= 0)
        return EXIT_SUCCESS;
    fprintf(stderr, "\nSync failed. Check logs in %s\n", log_file);
    return EXIT_FAILURE;
}
