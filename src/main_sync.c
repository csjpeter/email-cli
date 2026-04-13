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
#include "config_store.h"
#include "email_service.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include "local_store.h"
#include "fs_util.h"

static void help(void) {
    printf(
        "Usage: email-sync [options]\n"
        "\n"
        "Downloads all messages from all IMAP folders to the local store.\n"
        "Messages already stored locally are skipped.\n"
        "\n"
        "Designed to be run from cron (installed by 'email-cli cron setup').\n"
        "\n"
        "Options:\n"
        "  --help, -h    Show this help message\n"
        "\n"
        "Exit Codes:\n"
        "  0   Sync completed successfully (or already up to date)\n"
        "  1   Error (no config, connection failure, etc.)\n"
    );
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            help();
            return EXIT_SUCCESS;
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

    /* 3. Load configuration — no wizard: must already exist */
    Config *cfg = config_load_from_store();
    if (!cfg) {
        fprintf(stderr,
                "Error: No configuration found.\n"
                "Run 'email-cli' once to complete the setup wizard.\n");
        logger_close();
        return EXIT_FAILURE;
    }

    /* 4. Initialize local store */
    if (local_store_init(cfg->host) != 0)
        logger_log(LOG_WARN, "Failed to initialize local store for %s", cfg->host);

    /* 5. Run sync */
    int result = email_service_sync(cfg);

    config_free(cfg);
    logger_log(LOG_INFO, "--- email-sync finished (result: %d) ---", result);
    logger_close();

    if (result >= 0)
        return EXIT_SUCCESS;
    fprintf(stderr, "\nSync failed. Check logs in %s\n", log_file);
    return EXIT_FAILURE;
}
