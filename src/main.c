#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config_store.h"
#include "setup_wizard.h"
#include "email_service.h"
#include "raii.h"
#include "logger.h"
#include "fs_util.h"

static void usage() {
    printf("Usage: email-cli [options]\n");
    printf("Options:\n");
    printf("  --clean-logs    Delete all log files in the cache directory\n");
    printf("  --help          Show this help message\n");
}

int main(int argc, char *argv[]) {
    // 1. Basic Path Setup
    const char *home = fs_get_home_dir();
    if (!home) {
        fprintf(stderr, "Fatal: Could not determine home directory.\n");
        return EXIT_FAILURE;
    }

    RAII_STRING char *log_dir = NULL;
    RAII_STRING char *log_file = NULL;
    if (asprintf(&log_dir, "%s/.cache/email-cli/logs", home) == -1 ||
        asprintf(&log_file, "%s/session.log", log_dir) == -1) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return EXIT_FAILURE;
    }

    // 2. Handle CLI Arguments (Early)
    if (argc > 1) {
        if (strcmp(argv[1], "--clean-logs") == 0) {
            printf("Cleaning logs in %s...\n", log_dir);
            if (logger_clean_logs(log_dir) == 0) {
                printf("Logs cleaned successfully.\n");
                return EXIT_SUCCESS;
            } else {
                fprintf(stderr, "Failed to clean logs.\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[1], "--help") == 0) {
            usage();
            return EXIT_SUCCESS;
        }
    }

    // 3. Initialize Logger
    if (fs_mkdir_p(log_dir, 0700) != 0) {
        fprintf(stderr, "Warning: Could not create log directory %s\n", log_dir);
    }
    if (logger_init(log_file, LOG_DEBUG) != 0) {
        fprintf(stderr, "Warning: Logging system failed to initialize.\n");
    }

    logger_log(LOG_INFO, "--- email-cli starting ---");

    // 4. Load Configuration
    Config *cfg = config_load_from_store();
    if (!cfg) {
        logger_log(LOG_INFO, "No configuration found. Starting setup wizard.");
        cfg = setup_wizard_run();
        if (cfg) {
            if (config_save_to_store(cfg) != 0) {
                logger_log(LOG_ERROR, "Failed to save configuration.");
                printf("Error: Failed to save configuration to disk.\n");
            }
        } else {
            logger_log(LOG_ERROR, "Configuration aborted by user.");
            printf("Configuration aborted. Exiting.\n");
            logger_close();
            return EXIT_FAILURE;
        }
    }

    // 5. Initialize libcurl global
    curl_global_init(CURL_GLOBAL_ALL);

    // 6. Call high-level service to fetch emails
    int result = email_service_fetch_recent(cfg);

    // 7. Cleanup
    config_free(cfg);
    curl_global_cleanup();
    logger_log(LOG_INFO, "--- email-cli session finished ---");
    logger_close();

    if (result == 0) {
        printf("\nSuccess: Fetch complete.\n");
        return EXIT_SUCCESS;
    } else {
        printf("\nFailure: Fetch failed. Check logs in %s\n", log_file);
        return EXIT_FAILURE;
    }
}
