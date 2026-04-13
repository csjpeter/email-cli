/**
 * @file main_ro.c
 * @brief Entry point for email-cli-ro — read-only, non-interactive CLI.
 *
 * email-cli-ro is a strict subset of email-cli:
 *   - All output is batch/non-interactive (no TUI, no pager prompts).
 *   - No write operations (no SMTP, no IMAP flag changes, no cron writes).
 *   - No setup wizard — configuration must already exist.
 *   - Safe to give to AI agents: there is no code path that sends email.
 *
 * Supported commands: list, show, folders, sync, help.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include "config_store.h"
#include "email_service.h"
#include "platform/terminal.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include "local_store.h"
#include "fs_util.h"

#define BATCH_DEFAULT_LIMIT 100

/* ── Help pages ──────────────────────────────────────────────────────── */

static void help_general(void) {
    printf(
        "Usage: email-cli-ro <command> [options]\n"
        "\n"
        "Read-only email CLI. All output is non-interactive (batch mode).\n"
        "Safe for use by AI agents: no send or write operations are available.\n"
        "\n"
        "Commands:\n"
        "  list              List messages in the configured mailbox\n"
        "  show <uid>        Display the full content of a message by its UID\n"
        "  folders           List available IMAP folders\n"
        "  sync              Download all messages in all folders to local store\n"
        "  help [command]    Show this help, or detailed help for a command\n"
        "\n"
        "Run 'email-cli-ro help <command>' for more information.\n"
    );
}

static void help_list(void) {
    printf(
        "Usage: email-cli-ro list [options]\n"
        "\n"
        "Lists messages in the configured mailbox folder.\n"
        "\n"
        "Options:\n"
        "  --all              All messages are always shown; this flag has no\n"
        "                     effect and is kept for backwards compatibility.\n"
        "                     Unread messages are marked with 'N' and listed first.\n"
        "  --folder <name>    Use <name> instead of the configured folder.\n"
        "  --limit <n>        Show at most <n> messages (default: %d).\n"
        "  --offset <n>       Start listing from the <n>-th message (1-based).\n"
        "\n"
        "Examples:\n"
        "  email-cli-ro list\n"
        "  email-cli-ro list --folder INBOX.Sent --limit 50\n",
        BATCH_DEFAULT_LIMIT
    );
}

static void help_show(void) {
    printf(
        "Usage: email-cli-ro show <uid>\n"
        "\n"
        "Displays the full content of the message identified by <uid>.\n"
        "\n"
        "  <uid>   Numeric IMAP UID shown by 'email-cli-ro list'\n"
        "\n"
        "The message is fetched from the server on first access and stored\n"
        "locally at ~/.local/share/email-cli/messages/<folder>/<uid>.eml.\n"
        "Subsequent reads are served from the local store.\n"
    );
}

static void help_folders(void) {
    printf(
        "Usage: email-cli-ro folders [options]\n"
        "\n"
        "Lists all available IMAP folders on the server.\n"
        "\n"
        "Options:\n"
        "  --tree    Render the folder hierarchy as a tree.\n"
        "\n"
        "Examples:\n"
        "  email-cli-ro folders\n"
        "  email-cli-ro folders --tree\n"
    );
}

static void help_sync(void) {
    printf(
        "Usage: email-cli-ro sync\n"
        "\n"
        "Downloads all messages in every IMAP folder to the local store.\n"
        "Messages already stored locally are skipped.\n"
        "\n"
        "Examples:\n"
        "  email-cli-ro sync\n"
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
    fprintf(stderr, "Run 'email-cli-ro help %s' for usage.\n", cmd);
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
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

    /* 2. Find command: first non-flag argument */
    const char *cmd = NULL;
    int cmd_idx = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) continue;
        cmd = argv[i]; cmd_idx = i; break;
    }

    /* --help anywhere: treat as "help <cmd>" */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            if (cmd && strcmp(cmd, "--help") != 0) {
                if (strcmp(cmd, "list")    == 0) { help_list();    return EXIT_SUCCESS; }
                if (strcmp(cmd, "show")    == 0) { help_show();    return EXIT_SUCCESS; }
                if (strcmp(cmd, "folders") == 0) { help_folders(); return EXIT_SUCCESS; }
                if (strcmp(cmd, "sync")    == 0) { help_sync();    return EXIT_SUCCESS; }
            }
            help_general();
            return EXIT_SUCCESS;
        }
    }

    if (cmd && strcmp(cmd, "help") == 0) {
        const char *topic = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) { topic = argv[i]; break; }
        if (topic) {
            if (strcmp(topic, "list")    == 0) { help_list();    return EXIT_SUCCESS; }
            if (strcmp(topic, "show")    == 0) { help_show();    return EXIT_SUCCESS; }
            if (strcmp(topic, "folders") == 0) { help_folders(); return EXIT_SUCCESS; }
            if (strcmp(topic, "sync")    == 0) { help_sync();    return EXIT_SUCCESS; }
            fprintf(stderr, "Unknown command '%s'.\n", topic);
            fprintf(stderr, "Run 'email-cli-ro help' for available commands.\n");
            return EXIT_FAILURE;
        }
        help_general();
        return EXIT_SUCCESS;
    }

    if (!cmd) {
        help_general();
        return EXIT_SUCCESS;
    }

    /* 3. Initialize logger */
    if (fs_mkdir_p(log_dir, 0700) != 0)
        fprintf(stderr, "Warning: Could not create log directory %s\n", log_dir);
    if (logger_init(log_file, LOG_DEBUG) != 0)
        fprintf(stderr, "Warning: Logging system failed to initialize.\n");
    logger_log(LOG_INFO, "--- email-cli-ro starting (cmd: %s) ---", cmd);

    /* 4. Load configuration — no wizard: must already exist */
    Config *cfg = config_load_from_store();
    if (!cfg) {
        fprintf(stderr,
                "Error: No configuration found.\n"
                "Run 'email-cli' once to complete the setup wizard.\n");
        logger_close();
        return EXIT_FAILURE;
    }

    /* 5. Initialize local store */
    if (local_store_init(cfg->host) != 0)
        logger_log(LOG_WARN, "Failed to initialize local store for %s", cfg->host);

    /* 6. Dispatch — batch mode only (pager = 0) */
    int result = -1;

    if (strcmp(cmd, "list") == 0) {
        EmailListOpts opts = {0, NULL, BATCH_DEFAULT_LIMIT, 0, 0};
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) {
                /* accepted as no-op: email-cli-ro is always batch mode */
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
                        opts.limit = (int)v;
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
        if (ok) result = email_service_list(cfg, &opts);

    } else if (strcmp(cmd, "show") == 0) {
        const char *uid_str = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue; /* no-op */
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
                result = email_service_read(cfg, uid, 0, BATCH_DEFAULT_LIMIT);
        }

    } else if (strcmp(cmd, "folders") == 0) {
        int tree = 0, ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) { /* no-op */
            } else if (strcmp(argv[i], "--tree") == 0)
                tree = 1;
            else { unknown_option("folders", argv[i]); ok = 0; }
        }
        if (ok) result = email_service_list_folders(cfg, tree);

    } else if (strcmp(cmd, "sync") == 0) {
        result = email_service_sync(cfg);

    } else {
        fprintf(stderr, "Unknown command '%s'.\n", cmd);
        fprintf(stderr, "Run 'email-cli-ro help' for available commands.\n");
    }

    /* 7. Cleanup */
    config_free(cfg);
    logger_log(LOG_INFO, "--- email-cli-ro session finished ---");
    logger_close();

    if (result >= 0)
        return EXIT_SUCCESS;
    fprintf(stderr, "\nFailed. Check logs in %s\n", log_file);
    return EXIT_FAILURE;
}
