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
    printf(
        "Usage: email-cli <command> [options]\n"
        "\n"
        "Commands:\n"
        "  list              List unread messages in the configured mailbox\n"
        "  show <uid>        Display the full content of a message by its UID\n"
        "  folders           List available IMAP folders\n"
        "  help [command]    Show this help, or detailed help for a command\n"
        "\n"
        "Run 'email-cli help <command>' for more information.\n"
    );
}

static void help_list(void) {
    printf(
        "Usage: email-cli list [options]\n"
        "\n"
        "Lists messages in the configured mailbox folder.\n"
        "\n"
        "Options:\n"
        "  --all             Show all messages, not just unread ones.\n"
        "                    Unread messages are marked with 'N' and\n"
        "                    always appear at the top of the list.\n"
        "  --folder <name>   Use <name> instead of the configured folder.\n"
        "\n"
        "Examples:\n"
        "  email-cli list\n"
        "  email-cli list --all\n"
        "  email-cli list --folder INBOX.Sent\n"
        "  email-cli list --all --folder INBOX.Archive\n"
    );
}

static void help_show(void) {
    printf(
        "Usage: email-cli show <uid>\n"
        "\n"
        "Displays the full content of the message identified by <uid>.\n"
        "\n"
        "  <uid>   Numeric IMAP UID shown by 'email-cli list'\n"
        "\n"
        "The message is fetched from the server on first access and cached\n"
        "locally at ~/.cache/email-cli/messages/<folder>/<uid>.eml.\n"
        "Subsequent reads are served from the local cache.\n"
    );
}

static void help_folders(void) {
    printf(
        "Usage: email-cli folders [options]\n"
        "\n"
        "Lists all available IMAP folders on the server.\n"
        "\n"
        "Options:\n"
        "  --tree    Render the folder hierarchy as a tree.\n"
        "\n"
        "Examples:\n"
        "  email-cli folders\n"
        "  email-cli folders --tree\n"
    );
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int parse_uid(const char *s) {
    if (!s || !*s) return 0;
    char *end;
    long v = strtol(s, &end, 10);
    return (*end == '\0' && v > 0) ? (int)v : 0;
}

static void unknown_option(const char *cmd, const char *opt) {
    fprintf(stderr, "Unknown option '%s' for '%s'.\n", opt, cmd);
    fprintf(stderr, "Run 'email-cli help %s' for usage.\n", cmd);
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* 1. Determine home directory */
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

    /* 2. Commands that need no logger or config */
    const char *cmd = (argc > 1) ? argv[1] : NULL;

    if (!cmd || strcmp(cmd, "help") == 0) {
        if (argc > 2) {
            const char *topic = argv[2];
            if (strcmp(topic, "list")    == 0) { help_list();    return EXIT_SUCCESS; }
            if (strcmp(topic, "show")    == 0) { help_show();    return EXIT_SUCCESS; }
            if (strcmp(topic, "folders") == 0) { help_folders(); return EXIT_SUCCESS; }
            fprintf(stderr, "Unknown command '%s'.\n", topic);
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

    /* 4. Load configuration */
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

    /* 6. Dispatch */
    int result = -1;

    if (strcmp(cmd, "list") == 0) {
        EmailListOpts opts = {0, NULL};
        int ok = 1;
        for (int i = 2; i < argc && ok; i++) {
            if (strcmp(argv[i], "--all") == 0) {
                opts.all = 1;
            } else if (strcmp(argv[i], "--folder") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --folder requires a folder name.\n");
                    ok = 0;
                } else {
                    opts.folder = argv[++i];
                }
            } else {
                unknown_option("list", argv[i]);
                ok = 0;
            }
        }
        if (ok) result = email_service_list(cfg, &opts);

    } else if (strcmp(cmd, "show") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: 'show' requires a UID argument.\n");
            help_show();
        } else {
            int uid = parse_uid(argv[2]);
            if (!uid)
                fprintf(stderr, "Error: UID must be a positive integer (got '%s').\n", argv[2]);
            else
                result = email_service_read(cfg, uid);
        }

    } else if (strcmp(cmd, "folders") == 0) {
        int tree = 0, ok = 1;
        for (int i = 2; i < argc && ok; i++) {
            if (strcmp(argv[i], "--tree") == 0)
                tree = 1;
            else { unknown_option("folders", argv[i]); ok = 0; }
        }
        if (ok) result = email_service_list_folders(cfg, tree);

    } else {
        fprintf(stderr, "Unknown command '%s'.\n", cmd);
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
