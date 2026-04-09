#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include "config_store.h"
#include "setup_wizard.h"
#include "email_service.h"
#include "curl_adapter.h"
#include "platform/terminal.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include "local_store.h"
#include "fs_util.h"

/* Number of non-data lines printed around the message table */
#define LIST_HEADER_LINES 6
/* Default limit when stdout is not a terminal or --batch is given */
#define BATCH_DEFAULT_LIMIT 100

static int detect_page_size(int batch) {
    if (batch || !terminal_is_tty(STDOUT_FILENO))
        return BATCH_DEFAULT_LIMIT;
    int rows = terminal_rows();
    if (rows > LIST_HEADER_LINES + 2)
        return rows - LIST_HEADER_LINES;
    return 20; /* safe fallback */
}

/* ── Help pages ──────────────────────────────────────────────────────── */

static void help_general(void) {
    printf(
        "Usage: email-cli [--batch] <command> [options]\n"
        "\n"
        "Global options:\n"
        "  --batch           Disable interactive pager; use fixed page size (%d).\n"
        "                    Implied when stdout is redirected to a pipe or file.\n"
        "\n"
        "Commands:\n"
        "  list              List messages in the configured mailbox\n"
        "  show <uid>        Display the full content of a message by its UID\n"
        "  folders           List available IMAP folders\n"
        "  sync              Download all messages in all folders to local store\n"
        "  cron              Manage automatic background sync (setup/remove/status)\n"
        "  help [command]    Show this help, or detailed help for a command\n"
        "\n"
        "Run 'email-cli help <command>' for more information.\n",
        BATCH_DEFAULT_LIMIT
    );
}

static void help_list(void) {
    printf(
        "Usage: email-cli list [options]\n"
        "\n"
        "Lists messages in the configured mailbox folder.\n"
        "\n"
        "Options:\n"
        "  --all              All messages are always shown; this flag has no\n"
        "                     effect and is kept for backwards compatibility.\n"
        "                     Unread messages are marked with 'N' and listed first.\n"
        "  --folder <name>    Use <name> instead of the configured folder.\n"
        "  --limit <n>        Show at most <n> messages per page.\n"
        "                     Defaults to terminal height when output is a\n"
        "                     terminal, or %d when piped / --batch is used.\n"
        "  --offset <n>       Start listing from the <n>-th message (1-based).\n"
        "  --batch            Disable terminal detection; use limit=%d.\n"
        "\n"
        "Examples:\n"
        "  email-cli list\n"
        "  email-cli list --all\n"
        "  email-cli list --all --offset 21\n"
        "  email-cli list --folder INBOX.Sent --limit 50\n"
        "  email-cli list --all --batch\n",
        BATCH_DEFAULT_LIMIT, BATCH_DEFAULT_LIMIT
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
        "The message is fetched from the server on first access and stored\n"
        "locally at ~/.local/share/email-cli/messages/<folder>/<uid>.eml.\n"
        "Subsequent reads are served from the local store.\n"
        "\n"
        "Long messages are paginated automatically when output is a terminal.\n"
        "Use --batch or pipe to a file to disable the interactive pager.\n"
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

static void help_sync(void) {
    printf(
        "Usage: email-cli sync\n"
        "\n"
        "Downloads all messages in every IMAP folder to the local store.\n"
        "Messages already stored locally are skipped.\n"
        "Attachments are stored as part of the raw RFC 2822 message data\n"
        "(not extracted to separate files).\n"
        "\n"
        "Progress is printed per folder:\n"
        "  Syncing INBOX ...\n"
        "  42 fetched, 10 already stored\n"
        "\n"
        "Examples:\n"
        "  email-cli sync\n"
        "  email-cli sync --batch\n"
    );
}

static void help_cron(void) {
    printf(
        "Usage: email-cli cron <setup|remove|status>\n"
        "\n"
        "Manages automatic background synchronisation via user crontab.\n"
        "No sudo or system-level access is required.\n"
        "\n"
        "Subcommands:\n"
        "  setup    Install a crontab entry to run 'email-cli sync' periodically.\n"
        "           Uses SYNC_INTERVAL from config (default: 5 minutes if not set).\n"
        "           Saves the interval to config if a default was applied.\n"
        "  remove   Remove the 'email-cli sync' crontab entry.\n"
        "  status   Show whether an automatic sync entry is currently installed.\n"
        "\n"
        "Examples:\n"
        "  email-cli cron setup\n"
        "  email-cli cron status\n"
        "  email-cli cron remove\n"
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
    /* 0. Set locale so wcwidth() and mbsrtowcs() work correctly for
     *    multi-byte UTF-8 characters (needed for column-width calculations). */
    setlocale(LC_ALL, "");

    /* 1. Determine cache directory for logs */
    const char *cache_base = platform_cache_dir();
    if (!cache_base) {
        fprintf(stderr, "Fatal: Could not determine cache directory.\n");
        return EXIT_FAILURE;
    }

    RAII_STRING char *log_dir  = NULL;
    RAII_STRING char *log_file = NULL;
    if (asprintf(&log_dir,  "%s/email-cli/logs", cache_base) == -1 ||
        asprintf(&log_file, "%s/session.log", log_dir)        == -1) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return EXIT_FAILURE;
    }

    /* 2. Global flags: scan all args for --batch */
    int batch = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--batch") == 0) batch = 1;

    /* Command: first non-global-flag arg */
    const char *cmd = NULL;
    int cmd_idx = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--batch") == 0) continue;
        if (strcmp(argv[i], "--help") == 0) continue;
        cmd = argv[i]; cmd_idx = i; break;
    }

    /* --help anywhere in the args: treat as "help <cmd>" */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            if (cmd && strcmp(cmd, "--help") != 0) {
                /* e.g. email-cli list --help */
                if (strcmp(cmd, "list")    == 0) { help_list();    return EXIT_SUCCESS; }
                if (strcmp(cmd, "show")    == 0) { help_show();    return EXIT_SUCCESS; }
                if (strcmp(cmd, "folders") == 0) { help_folders(); return EXIT_SUCCESS; }
                if (strcmp(cmd, "sync")    == 0) { help_sync();    return EXIT_SUCCESS; }
                if (strcmp(cmd, "cron")    == 0) { help_cron();    return EXIT_SUCCESS; }
            }
            /* email-cli --help  or  email-cli help --help */
            help_general();
            return EXIT_SUCCESS;
        }
    }

    /* Page size / pager capability (used by list and show) */
    int pager     = !batch && terminal_is_tty(STDOUT_FILENO);
    int page_size = detect_page_size(batch);

    if (cmd && strcmp(cmd, "help") == 0) {
        /* First arg after the command is the topic */
        const char *topic = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            topic = argv[i]; break;
        }
        if (topic) {
            if (strcmp(topic, "list")           == 0) { help_list();    return EXIT_SUCCESS; }
            if (strcmp(topic, "show")           == 0) { help_show();    return EXIT_SUCCESS; }
            if (strcmp(topic, "folders")        == 0) { help_folders(); return EXIT_SUCCESS; }
            if (strcmp(topic, "sync")           == 0) { help_sync();    return EXIT_SUCCESS; }
            if (strcmp(topic, "cron")           == 0) { help_cron();    return EXIT_SUCCESS; }
            fprintf(stderr, "Unknown command '%s'.\n", topic);
            fprintf(stderr, "Run 'email-cli help' for available commands.\n");
            return EXIT_FAILURE;
        }
        help_general();
        return EXIT_SUCCESS;
    }

    /* No command in batch/non-tty mode: show help and exit. */
    if (!cmd && !pager) {
        help_general();
        return EXIT_SUCCESS;
    }

    /* Config-free commands: dispatch before loading config. */
    if (cmd && strcmp(cmd, "cron") == 0) {
        const char *subcmd = argc > cmd_idx + 1 ? argv[cmd_idx + 1] : "";
        if (strcmp(subcmd, "status") == 0)
            return email_service_cron_status() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        if (strcmp(subcmd, "remove") == 0)
            return email_service_cron_remove() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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

    /* 5. Initialize local store and libcurl */
    if (local_store_init(cfg->host) != 0)
        logger_log(LOG_WARN, "Failed to initialize local store for %s", cfg->host);
    curl_global_init(CURL_GLOBAL_ALL);

    /* 6. Dispatch */
    int result = -1;

    if (!cmd) {
        /* Interactive TUI: start with unread messages in the configured folder. */
        char *tui_folder = strdup(cfg->folder ? cfg->folder : "INBOX");
        if (!tui_folder) {
            result = -1;
        } else {
            for (;;) {
                EmailListOpts opts = {0, tui_folder, page_size, 0, 1};
                int ret = email_service_list(cfg, &opts);
                if (ret != 1) { result = (ret >= 0) ? 0 : -1; break; }
                /* User pressed Backspace → show folder browser */
                char *sel = email_service_list_folders_interactive(cfg, tui_folder);
                free(tui_folder);
                tui_folder = sel;
                if (!tui_folder) { result = 0; break; }
            }
            free(tui_folder);
        }

    } else if (strcmp(cmd, "list") == 0) {
        EmailListOpts opts = {0, NULL, 0, 0, pager};
        int ok = 1, explicit_limit = -1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) {
                continue; /* already handled globally */
            } else if (strcmp(argv[i], "--all") == 0) {
                opts.all = 1;
            } else if (strcmp(argv[i], "--folder") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --folder requires a folder name.\n");
                    ok = 0;
                } else {
                    opts.folder = argv[++i];
                }
            } else if (strcmp(argv[i], "--limit") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --limit requires a number.\n");
                    ok = 0;
                } else {
                    char *end;
                    long v = strtol(argv[++i], &end, 10);
                    if (*end != '\0' || v <= 0) {
                        fprintf(stderr, "Error: --limit must be a positive integer.\n");
                        ok = 0;
                    } else {
                        explicit_limit = (int)v;
                    }
                }
            } else if (strcmp(argv[i], "--offset") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --offset requires a number.\n");
                    ok = 0;
                } else {
                    char *end;
                    long v = strtol(argv[++i], &end, 10);
                    if (*end != '\0' || v < 1) {
                        fprintf(stderr, "Error: --offset must be a positive integer.\n");
                        ok = 0;
                    } else {
                        opts.offset = (int)v;
                    }
                }
            } else {
                unknown_option("list", argv[i]);
                ok = 0;
            }
        }
        if (ok) {
            opts.limit = (explicit_limit >= 0) ? explicit_limit : page_size;
            result = email_service_list(cfg, &opts);
        }

    } else if (strcmp(cmd, "show") == 0) {
        /* UID is the first non --batch arg after "show" */
        const char *uid_str = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            uid_str = argv[i]; break;
        }
        if (!uid_str) {
            fprintf(stderr, "Error: 'show' requires a UID argument.\n");
            help_show();
        } else {
            int uid = parse_uid(uid_str);
            if (!uid)
                fprintf(stderr,
                        "Error: UID must be a positive integer (got '%s').\n",
                        uid_str);
            else
                result = email_service_read(cfg, uid, pager, page_size);
        }

    } else if (strcmp(cmd, "folders") == 0) {
        int tree = 0, ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (strcmp(argv[i], "--tree") == 0)
                tree = 1;
            else { unknown_option("folders", argv[i]); ok = 0; }
        }
        if (ok) result = email_service_list_folders(cfg, tree);

    } else if (strcmp(cmd, "sync") == 0) {
        result = email_service_sync(cfg);

    } else if (strcmp(cmd, "cron") == 0) {
        /* 'cron status' and 'cron remove' are handled before config loading above.
         * Only 'cron setup' reaches here. */
        const char *subcmd = argc > cmd_idx + 1 ? argv[cmd_idx + 1] : "";
        if (strcmp(subcmd, "setup") == 0) {
            if (cfg->sync_interval <= 0) {
                cfg->sync_interval = 5;
                printf("sync_interval not configured; using default of 5 minutes.\n");
                if (config_save_to_store(cfg) != 0)
                    fprintf(stderr, "Warning: could not save sync_interval to config.\n");
            }
            return email_service_cron_setup(cfg) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            fprintf(stderr, "Usage: email-cli cron <setup|remove|status>\n");
            return EXIT_FAILURE;
        }

    } else {
        fprintf(stderr, "Unknown command '%s'.\n", cmd);
        fprintf(stderr, "Run 'email-cli help' for available commands.\n");
    }

    /* 7. Cleanup */
    config_free(cfg);
    curl_global_cleanup();
    logger_log(LOG_INFO, "--- email-cli session finished ---");
    logger_close();

    if (result >= 0)
        return EXIT_SUCCESS;
    fprintf(stderr, "\nFailed. Check logs in %s\n", log_file);
    return EXIT_FAILURE;
}
