#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config_store.h"
#include "setup_wizard.h"
#include "email_service.h"
#include "raii.h"
#include "logger.h"
#include "fs_util.h"

/* ── Help pages ──────────────────────────────────────────────────────── */

static void help_general(void) {
    printf("Usage: email-cli <command> [arguments]\n"
           "\n"
           "Commands:\n"
           "  list            List unread messages in the configured mailbox\n"
           "  show <uid>      Display the full content of a message by its UID\n"
           "  help [command]  Show this help, or detailed help for a command\n"
           "\n"
           "Run 'email-cli help <command>' for more information on a command.\n");
}

static void help_list(void) {
    printf("Usage: email-cli list\n"
           "\n"
           "Lists all unread (UNSEEN) messages in the configured mailbox.\n"
           "\n"
           "Displays a table with UID, From, Subject, and Date for each\n"
           "unread message. Use 'email-cli show <uid>' to read a message.\n");
}

static void help_show(void) {
    printf("Usage: email-cli show <uid>\n"
           "\n"
           "Displays the full content of the message identified by <uid>.\n"
           "\n"
           "  <uid>   Numeric IMAP UID shown by 'email-cli list'\n"
           "\n"
           "The message is fetched from the server on first access and cached\n"
           "locally at ~/.cache/email-cli/messages/<folder>/<uid>.eml.\n"
           "Subsequent reads are served from the local cache.\n");
}

/* ── Argument parsing helpers ────────────────────────────────────────── */

/** Parses a positive integer UID from a string. Returns 0 on failure. */
static int parse_uid(const char *s) {
    if (!s || !*s) return 0;
    char *end;
    long v = strtol(s, &end, 10);
    return (*end == '\0' && v > 0) ? (int)v : 0;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* 1. Determine home directory (needed for log path) */
    const char *home = fs_get_home_dir();
    if (!home) {
        fprintf(stderr, "Fatal: Could not determine home directory.\n");
        return EXIT_FAILURE;
    }

    RAII_STRING char *log_dir  = NULL;
    RAII_STRING char *log_file = NULL;
    if (asprintf(&log_dir,  "%s/.cache/email-cli/logs", home) == -1 ||
        asprintf(&log_file, "%s/session.log", log_dir)        == -1) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return EXIT_FAILURE;
    }

    /* 2. Dispatch commands that need no logger or config */
    const char *cmd = (argc > 1) ? argv[1] : NULL;

    if (!cmd || strcmp(cmd, "help") == 0) {
        if (argc > 2) {
            const char *topic = argv[2];
            if (strcmp(topic, "list") == 0)      { help_list(); return EXIT_SUCCESS; }
            if (strcmp(topic, "show") == 0)      { help_show(); return EXIT_SUCCESS; }
            fprintf(stderr, "Unknown command '%s'. ", topic);
            fprintf(stderr, "Run 'email-cli help' for available commands.\n");
            return EXIT_FAILURE;
        }
        help_general();
        return EXIT_SUCCESS;
    }

    /* 3. Initialize logger */
    if (fs_mkdir_p(log_dir, 0700) != 0)
        fprintf(stderr, "Warning: Could not create log directory %s\n", log_dir);
    if (logger_init(log_file, LOG_DEBUG) != 0)
        fprintf(stderr, "Warning: Logging system failed to initialize.\n");

    logger_log(LOG_INFO, "--- email-cli starting (cmd: %s) ---", cmd);

    /* 4. Load configuration (run wizard on first use) */
    Config *cfg = config_load_from_store();
    if (!cfg) {
        logger_log(LOG_INFO, "No configuration found. Starting setup wizard.");
        cfg = setup_wizard_run();
        if (cfg) {
            if (config_save_to_store(cfg) != 0) {
                logger_log(LOG_ERROR, "Failed to save configuration.");
                fprintf(stderr, "Error: Failed to save configuration to disk.\n");
            }
        } else {
            logger_log(LOG_ERROR, "Configuration aborted by user.");
            fprintf(stderr, "Configuration aborted. Exiting.\n");
            logger_close();
            return EXIT_FAILURE;
        }
    }

    /* 5. Initialize libcurl */
    curl_global_init(CURL_GLOBAL_ALL);

    /* 6. Dispatch command */
    int result = -1;

    if (strcmp(cmd, "list") == 0) {
        if (argc > 2) {
            fprintf(stderr, "Error: 'list' takes no arguments.\n");
            help_list();
        } else {
            result = email_service_list_unseen(cfg);
        }
    } else if (strcmp(cmd, "show") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: 'show' requires a UID argument.\n");
            help_show();
        } else {
            int uid = parse_uid(argv[2]);
            if (!uid) {
                fprintf(stderr,
                        "Error: UID must be a positive integer (got '%s').\n",
                        argv[2]);
            } else {
                result = email_service_read(cfg, uid);
            }
        }
    } else {
        fprintf(stderr, "Unknown command '%s'. ", cmd);
        fprintf(stderr, "Run 'email-cli help' for available commands.\n");
    }

    /* 7. Cleanup */
    config_free(cfg);
    curl_global_cleanup();
    logger_log(LOG_INFO, "--- email-cli session finished ---");
    logger_close();

    if (result == 0) {
        printf("\nSuccess: Fetch complete.\n");
        return EXIT_SUCCESS;
    }
    fprintf(stderr, "\nFailed. Check logs in %s\n", log_file);
    return EXIT_FAILURE;
}
