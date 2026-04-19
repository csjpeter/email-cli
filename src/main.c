#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include "config_store.h"
#include "setup_wizard.h"
#include "email_service.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include "local_store.h"
#include "fs_util.h"
#include "smtp_adapter.h"
#include "compose_service.h"
#include "help_gmail.h"

/* Default limit for batch output */
#define BATCH_DEFAULT_LIMIT 100

/* ── Help pages ──────────────────────────────────────────────────────── */

static void help_general(void) {
    printf(
        "Usage: email-cli [<account>] <command> [options]\n"
        "\n"
        "Batch-mode email CLI with read and write operations.\n"
        "For the interactive TUI, use email-tui.\n"
        "\n"
        "  <account>  Email address of the account to use (e.g. user@example.com).\n"
        "             Required when multiple accounts are configured.\n"
        "             Alternative: --account <email>.\n"
        "\n"
        "Commands:\n"
        "  list                    List messages in the configured mailbox\n"
        "  show <uid>              Display the full content of a message by its UID\n"
        "  folders                 List available IMAP folders\n"
        "  attachments <uid>       List attachments in a message\n"
        "  save-attachment <uid>   Save a named attachment to disk\n"
        "  send                    Send a message non-interactively\n"
        "  mark-read <uid>         Mark a message as read\n"
        "  mark-unread <uid>       Mark a message as unread\n"
        "  mark-starred <uid>      Star (flag) a message\n"
        "  remove-starred <uid>    Remove star from a message\n"
        "  add-label <uid> <lbl>   Add a Gmail label to a message\n"
        "  remove-label <uid> <lbl> Remove a Gmail label from a message\n"
        "  list-labels             List all labels (Gmail) or folders (IMAP)\n"
        "  create-label <name>     Create a new label (Gmail) or folder (IMAP)\n"
        "  delete-label <id>       Delete a label (Gmail) or folder (IMAP)\n"
        "  show-accounts           List all configured accounts\n"
        "  add-account             Add a new account (runs setup wizard)\n"
        "  remove-account <email>  Remove an account (local data preserved)\n"
        "  config                  View or update configuration (incl. SMTP)\n"
        "  migrate-credentials     Re-encrypt (or decrypt) all stored passwords\n"
        "  help [command]          Show this help, or detailed help for a command\n"
        "  help gmail              Step-by-step Gmail OAuth2 setup guide\n"
        "\n"
        "Run 'email-cli help <command>' for more information.\n"
        "For the interactive TUI use email-tui.\n"
        "For background sync and cron management use email-sync.\n"
    );
}

static void help_list(void) {
    printf(
        "Usage: email-cli list [options]\n"
        "\n"
        "Lists messages in the configured mailbox folder.\n"
        "Shows unread (UNSEEN) messages by default; use --all for everything.\n"
        "\n"
        "Options:\n"
        "  --all              Show all messages (not just unread).\n"
        "                     Unread messages are marked with 'N' and listed first.\n"
        "  --folder <name>    Use <name> instead of the configured folder.\n"
        "  --limit <n>        Show at most <n> messages (default: %d).\n"
        "  --offset <n>       Start listing from the <n>-th message (1-based).\n"
        "\n"
        "Examples:\n"
        "  email-cli list\n"
        "  email-cli list --all\n"
        "  email-cli list --all --offset 21\n"
        "  email-cli list --folder INBOX.Sent --limit 50\n",
        BATCH_DEFAULT_LIMIT
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

static void help_send(void) {
    printf(
        "Usage: email-cli send --to <addr> --subject <text> --body <text>\n"
        "\n"
        "Sends a message non-interactively (scriptable / batch mode).\n"
        "\n"
        "Options:\n"
        "  --to <addr>       Recipient email address (required)\n"
        "  --subject <text>  Subject line (required)\n"
        "  --body <text>     Message body text (required)\n"
        "\n"
        "SMTP settings must be configured (run 'email-cli config smtp').\n"
        "\n"
        "Examples:\n"
        "  email-cli send --to friend@example.com --subject \"Hello\" --body \"Hi there!\"\n"
    );
}

static void help_config(void) {
    printf(
        "Usage: email-cli [<account>] config <subcommand>\n"
        "\n"
        "View or update configuration settings.\n"
        "\n"
        "Subcommands:\n"
        "  show    Print current configuration (passwords masked)\n"
        "  imap    Interactively configure IMAP (incoming mail) settings\n"
        "  smtp    Interactively configure SMTP (outgoing mail) settings\n"
        "\n"
        "SMTP settings are used by email-tui for composing and sending mail.\n"
        "Configuring them here writes to the shared config file.\n"
        "\n"
        "Examples:\n"
        "  email-cli config show\n"
        "  email-cli config imap\n"
        "  email-cli user@example.com config show\n"
    );
}

static void help_attachments(void) {
    printf(
        "Usage: email-cli attachments <uid>\n"
        "\n"
        "Lists all attachments in the message identified by <uid>.\n"
        "Prints one line per attachment: filename and decoded size.\n"
        "\n"
        "  <uid>   Numeric IMAP UID shown by 'email-cli list'\n"
        "\n"
        "Examples:\n"
        "  email-cli attachments 42\n"
    );
}

static void help_save_attachment(void) {
    printf(
        "Usage: email-cli save-attachment <uid> <filename> [dir]\n"
        "\n"
        "Saves the named attachment from message <uid> to disk.\n"
        "\n"
        "  <uid>       Numeric IMAP UID shown by 'email-cli list'\n"
        "  <filename>  Exact attachment filename shown by 'email-cli attachments'\n"
        "  [dir]       Destination directory (default: ~/Downloads or ~)\n"
        "\n"
        "Examples:\n"
        "  email-cli save-attachment 42 report.pdf\n"
        "  email-cli save-attachment 42 report.pdf /tmp\n"
    );
}

static void help_mark_read(void) {
    printf(
        "Usage: email-cli mark-read <uid> [--folder <name>]\n"
        "       email-cli mark-unread <uid> [--folder <name>]\n"
        "\n"
        "Mark a message as read (removes the UNSEEN flag) or unread (adds it).\n"
        "\n"
        "  <uid>            Numeric IMAP UID shown by 'email-cli list'\n"
        "  --folder <name>  Folder/label containing the message (default: configured folder)\n"
        "\n"
        "Examples:\n"
        "  email-cli mark-read 42\n"
        "  email-cli mark-unread 42 --folder INBOX\n"
    );
}

static void help_mark_starred(void) {
    printf(
        "Usage: email-cli mark-starred <uid> [--folder <name>]\n"
        "       email-cli remove-starred <uid> [--folder <name>]\n"
        "\n"
        "Star (flag) or un-star a message.\n"
        "\n"
        "  <uid>            Numeric IMAP UID shown by 'email-cli list'\n"
        "  --folder <name>  Folder/label containing the message\n"
        "\n"
        "Examples:\n"
        "  email-cli mark-starred 42\n"
        "  email-cli remove-starred 42\n"
    );
}

static void help_add_label(void) {
    printf(
        "Usage: email-cli add-label <uid> <label>\n"
        "       email-cli remove-label <uid> <label>\n"
        "\n"
        "Add or remove a Gmail label on a message (Gmail only).\n"
        "\n"
        "  <uid>    Numeric message ID shown by 'email-cli list'\n"
        "  <label>  Gmail label ID (e.g. 'Label_12345' or 'STARRED')\n"
        "\n"
        "Examples:\n"
        "  email-cli add-label 1abc23 Work\n"
        "  email-cli remove-label 1abc23 Work\n"
    );
}

static void help_list_labels(void) {
    printf(
        "Usage: email-cli list-labels\n"
        "\n"
        "List all available labels (Gmail) or folders (IMAP).\n"
        "For Gmail, shows both the display name and the label ID.\n"
        "\n"
        "Examples:\n"
        "  email-cli list-labels\n"
    );
}

static void help_create_label(void) {
    printf(
        "Usage: email-cli create-label <name>\n"
        "\n"
        "Create a new Gmail label or IMAP folder.\n"
        "\n"
        "  <name>  Display name for the new label/folder\n"
        "\n"
        "Examples:\n"
        "  email-cli create-label Work\n"
        "  email-cli create-label \"My Projects\"\n"
    );
}

static void help_delete_label(void) {
    printf(
        "Usage: email-cli delete-label <label-id>\n"
        "\n"
        "Delete a Gmail label or IMAP folder.\n"
        "For Gmail, <label-id> is the label ID (from list-labels).\n"
        "For IMAP, <label-id> is the folder name.\n"
        "System labels (INBOX, TRASH, etc.) cannot be deleted.\n"
        "\n"
        "  <label-id>  Label ID (Gmail) or folder name (IMAP)\n"
        "\n"
        "Examples:\n"
        "  email-cli delete-label Label_12345\n"
        "  email-cli delete-label MyFolder\n"
    );
}

static void help_show_accounts(void) {
    printf(
        "Usage: email-cli show-accounts\n"
        "\n"
        "List all configured accounts with their type and server.\n"
        "\n"
        "Examples:\n"
        "  email-cli show-accounts\n"
    );
}

static void help_add_account(void) {
    printf(
        "Usage: email-cli add-account\n"
        "\n"
        "Run the interactive setup wizard to add a new account.\n"
        "Supports both IMAP and Gmail (OAuth2) accounts.\n"
        "\n"
        "Examples:\n"
        "  email-cli add-account\n"
    );
}

static void help_remove_account(void) {
    printf(
        "Usage: email-cli remove-account <email>\n"
        "\n"
        "Remove a configured account by email address.\n"
        "Local messages are NOT deleted — they are preserved on disk.\n"
        "\n"
        "  <email>  Account email address shown by 'email-cli show-accounts'\n"
        "\n"
        "Examples:\n"
        "  email-cli remove-account user@example.com\n"
    );
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int parse_uid(const char *s, char uid_out[17]) {
    if (!s || !*s) return -1;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v == 0 || v > 4294967295UL) return -1;
    snprintf(uid_out, 17, "%016lu", v);
    return 0;
}

static void unknown_option(const char *cmd, const char *opt) {
    fprintf(stderr, "Unknown option '%s' for '%s'.\n", opt, cmd);
    fprintf(stderr, "Run 'email-cli help %s' for usage.\n", cmd);
}

/* ── Entry point ─────────────────────────────────────────────────────── */

#ifndef EMAIL_CLI_VERSION
#define EMAIL_CLI_VERSION "unknown"
#endif

int main(int argc, char *argv[]) {
    /* 0. Set locale so wcwidth() and mbsrtowcs() work correctly for
     *    multi-byte UTF-8 characters (needed for column-width calculations). */
    setlocale(LC_ALL, "");

    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("email-cli %s\n", EMAIL_CLI_VERSION);
        return EXIT_SUCCESS;
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
        asprintf(&log_file, "%s/session.log", log_dir)        == -1) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return EXIT_FAILURE;
    }

    /* 2. Account + command detection.
     *    Supported forms:
     *      email-cli [<account>] <command> [options]
     *      email-cli --account <email> <command> [options]
     *    <account> is the first non-flag positional arg that contains '@'. */
    const char *account_arg = NULL;
    int account_arg_idx = -1;

    /* Pass A: scan for --account flag anywhere in args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--batch") == 0) continue;
        if (strcmp(argv[i], "--account") == 0 && i + 1 < argc) {
            account_arg = argv[++i]; continue;
        }
    }

    /* Pass B: if no --account flag, check whether first positional arg is an email */
    if (!account_arg) {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                if (strcmp(argv[i], "--account") == 0) i++; /* skip --account pair */
                continue;
            }
            /* First non-flag arg: if it looks like an email, treat as account */
            if (strchr(argv[i], '@')) {
                account_arg = argv[i];
                account_arg_idx = i;
            }
            break; /* stop after first non-flag arg */
        }
    }

    /* Command: first non-flag, non-account arg */
    const char *cmd = NULL;
    int cmd_idx = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--batch") == 0) continue;
        if (strcmp(argv[i], "--help") == 0) continue;
        if (strcmp(argv[i], "--account") == 0) { i++; continue; }
        if (i == account_arg_idx) continue; /* skip positional account */
        cmd = argv[i]; cmd_idx = i; break;
    }

    /* --help anywhere in the args: treat as "help <cmd>" */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--account") == 0) { i++; continue; }
        if (i == account_arg_idx) continue;
        if (strcmp(argv[i], "--help") == 0) {
            if (cmd && strcmp(cmd, "--help") != 0) {
                /* e.g. email-cli list --help */
                if (strcmp(cmd, "list")            == 0) { help_list();            return EXIT_SUCCESS; }
                if (strcmp(cmd, "show")            == 0) { help_show();            return EXIT_SUCCESS; }
                if (strcmp(cmd, "folders")         == 0) { help_folders();         return EXIT_SUCCESS; }
                if (strcmp(cmd, "attachments")     == 0) { help_attachments();     return EXIT_SUCCESS; }
                if (strcmp(cmd, "save-attachment") == 0) { help_save_attachment(); return EXIT_SUCCESS; }
                if (strcmp(cmd, "send")            == 0) { help_send();            return EXIT_SUCCESS; }
                if (strcmp(cmd, "config")          == 0) { help_config();          return EXIT_SUCCESS; }
                if (strcmp(cmd, "gmail")           == 0) { help_gmail();           return EXIT_SUCCESS; }
                if (strcmp(cmd, "mark-read")       == 0 ||
                    strcmp(cmd, "mark-unread")     == 0) { help_mark_read();       return EXIT_SUCCESS; }
                if (strcmp(cmd, "mark-starred")    == 0 ||
                    strcmp(cmd, "remove-starred")  == 0) { help_mark_starred();    return EXIT_SUCCESS; }
                if (strcmp(cmd, "add-label")       == 0 ||
                    strcmp(cmd, "remove-label")    == 0) { help_add_label();       return EXIT_SUCCESS; }
                if (strcmp(cmd, "list-labels")     == 0) { help_list_labels();     return EXIT_SUCCESS; }
                if (strcmp(cmd, "create-label")    == 0) { help_create_label();    return EXIT_SUCCESS; }
                if (strcmp(cmd, "delete-label")    == 0) { help_delete_label();    return EXIT_SUCCESS; }
                if (strcmp(cmd, "show-accounts")   == 0) { help_show_accounts();   return EXIT_SUCCESS; }
                if (strcmp(cmd, "add-account")     == 0) { help_add_account();     return EXIT_SUCCESS; }
                if (strcmp(cmd, "remove-account")  == 0) { help_remove_account();  return EXIT_SUCCESS; }
            }
            /* email-cli --help  or  email-cli help --help */
            help_general();
            return EXIT_SUCCESS;
        }
    }

    if (cmd && strcmp(cmd, "help") == 0) {
        /* First arg after the command is the topic */
        const char *topic = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            topic = argv[i]; break;
        }
        if (topic) {
            if (strcmp(topic, "list")            == 0) { help_list();            return EXIT_SUCCESS; }
            if (strcmp(topic, "show")            == 0) { help_show();            return EXIT_SUCCESS; }
            if (strcmp(topic, "folders")         == 0) { help_folders();         return EXIT_SUCCESS; }
            if (strcmp(topic, "attachments")     == 0) { help_attachments();     return EXIT_SUCCESS; }
            if (strcmp(topic, "save-attachment") == 0) { help_save_attachment(); return EXIT_SUCCESS; }
            if (strcmp(topic, "send")            == 0) { help_send();            return EXIT_SUCCESS; }
            if (strcmp(topic, "config")          == 0) { help_config();          return EXIT_SUCCESS; }
            if (strcmp(topic, "gmail")           == 0) { help_gmail();           return EXIT_SUCCESS; }
            if (strcmp(topic, "mark-read")       == 0 ||
                strcmp(topic, "mark-unread")     == 0) { help_mark_read();       return EXIT_SUCCESS; }
            if (strcmp(topic, "mark-starred")    == 0 ||
                strcmp(topic, "remove-starred")  == 0) { help_mark_starred();    return EXIT_SUCCESS; }
            if (strcmp(topic, "add-label")       == 0 ||
                strcmp(topic, "remove-label")    == 0) { help_add_label();       return EXIT_SUCCESS; }
            if (strcmp(topic, "list-labels")     == 0) { help_list_labels();     return EXIT_SUCCESS; }
            if (strcmp(topic, "create-label")    == 0) { help_create_label();    return EXIT_SUCCESS; }
            if (strcmp(topic, "delete-label")    == 0) { help_delete_label();    return EXIT_SUCCESS; }
            if (strcmp(topic, "show-accounts")   == 0) { help_show_accounts();   return EXIT_SUCCESS; }
            if (strcmp(topic, "add-account")     == 0) { help_add_account();     return EXIT_SUCCESS; }
            if (strcmp(topic, "remove-account")  == 0) { help_remove_account();  return EXIT_SUCCESS; }
            fprintf(stderr, "Unknown command '%s'.\n", topic);
            fprintf(stderr, "Run 'email-cli help' for available commands.\n");
            return EXIT_FAILURE;
        }
        help_general();
        return EXIT_SUCCESS;
    }

    /* No command: show help and exit. */
    if (!cmd) {
        help_general();
        return EXIT_SUCCESS;
    }

    /* 3. Initialize logger */
    if (fs_mkdir_p(log_dir, 0700) != 0)
        fprintf(stderr, "Warning: Could not create log directory %s\n", log_dir);
    if (logger_init(log_file, LOG_DEBUG) != 0)
        fprintf(stderr, "Warning: Logging system failed to initialize.\n");
    logger_log(LOG_INFO, "--- email-cli starting (cmd: %s) ---", cmd);

    /* Determine if this command needs a specific account config.
     * migrate-credentials operates on all accounts and handles its own loading. */
    int cmd_needs_cfg = !(cmd && (strcmp(cmd, "add-account")        == 0 ||
                                  strcmp(cmd, "show-accounts")      == 0 ||
                                  strcmp(cmd, "remove-account")     == 0 ||
                                  strcmp(cmd, "migrate-credentials") == 0));

    /* 4. Load configuration */
    Config *cfg = NULL;
    if (cmd_needs_cfg) {
        if (account_arg) {
            /* Specific account requested */
            cfg = config_load_account(account_arg);
            if (!cfg) {
                fprintf(stderr,
                        "Error: Account '%s' not found.\n"
                        "Run 'email-cli show-accounts' to list configured accounts.\n",
                        account_arg);
                logger_close();
                return EXIT_FAILURE;
            }
        } else {
            /* No account specified: use the only account or run wizard */
            int count = 0;
            AccountEntry *list = config_list_accounts(&count);
            if (count == 1) {
                cfg = list[0].cfg; list[0].cfg = NULL;
                config_free_account_list(list, count);
            } else if (count > 1) {
                fprintf(stderr, "Multiple accounts configured. Specify which to use:\n");
                for (int i = 0; i < count; i++)
                    fprintf(stderr, "  email-cli %s %s\n",
                            list[i].name ? list[i].name : "?", cmd ? cmd : "");
                fprintf(stderr, "Run 'email-cli show-accounts' for the full list.\n");
                config_free_account_list(list, count);
                logger_close();
                return EXIT_FAILURE;
            } else {
                config_free_account_list(list, count);
                /* No accounts: run setup wizard */
                logger_log(LOG_INFO, "No configuration found. Starting setup wizard.");
                cfg = setup_wizard_run();
                if (cfg) {
                    if (config_save_to_store(cfg) != 0) {
                        logger_log(LOG_ERROR, "Failed to save configuration.");
                        fprintf(stderr, "Error: Failed to save configuration to disk.\n");
                    } else {
                        printf("Configuration saved. Run 'email-cli sync' to download your mail.\n");
                    }
                } else {
                    logger_log(LOG_ERROR, "Configuration aborted by user.");
                    fprintf(stderr, "Configuration aborted. Exiting.\n");
                    logger_close();
                    return EXIT_FAILURE;
                }
            }
        }
    }

    /* 5. Initialize local store */
    if (cfg && local_store_init(cfg->host, cfg->user) != 0)
        logger_log(LOG_WARN, "Failed to initialize local store for %s", cfg->host);

    /* 6. Dispatch — batch mode only (no interactive TUI) */
    int result = -1;

    if (strcmp(cmd, "list") == 0) {
        EmailListOpts opts = {0, NULL, BATCH_DEFAULT_LIMIT, 0, 0, {0}};
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) {
                continue; /* accepted as no-op */
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
            char uid[17];
            if (parse_uid(uid_str, uid) != 0)
                fprintf(stderr,
                        "Error: UID must be a positive integer (got '%s').\n",
                        uid_str);
            else
                result = email_service_read(cfg, uid, 0, BATCH_DEFAULT_LIMIT);
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

    } else if (strcmp(cmd, "attachments") == 0) {
        const char *uid_str = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            uid_str = argv[i]; break;
        }
        if (!uid_str) {
            fprintf(stderr, "Error: 'attachments' requires a UID argument.\n");
            help_attachments();
        } else {
            char uid[17];
            if (parse_uid(uid_str, uid) != 0)
                fprintf(stderr,
                        "Error: UID must be a positive integer (got '%s').\n",
                        uid_str);
            else
                result = email_service_list_attachments(cfg, uid);
        }

    } else if (strcmp(cmd, "save-attachment") == 0) {
        const char *uid_str  = NULL;
        const char *filename = NULL;
        const char *outdir   = NULL;
        int argn = 0;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (argn == 0)      { uid_str  = argv[i]; argn++; }
            else if (argn == 1) { filename = argv[i]; argn++; }
            else if (argn == 2) { outdir   = argv[i]; argn++; }
        }
        if (!uid_str || !filename) {
            fprintf(stderr,
                    "Error: 'save-attachment' requires a UID and a filename.\n");
            help_save_attachment();
        } else {
            char uid[17];
            if (parse_uid(uid_str, uid) != 0)
                fprintf(stderr,
                        "Error: UID must be a positive integer (got '%s').\n",
                        uid_str);
            else
                result = email_service_save_attachment(cfg, uid, filename, outdir);
        }

    } else if (strcmp(cmd, "config") == 0) {
        /* Account already loaded globally above. */
        const char *subcmd = (argc > cmd_idx + 1) ? argv[cmd_idx + 1] : "";

        if (strcmp(subcmd, "show") == 0) {
            printf("\nemail-cli configuration");
            if (cfg->user) printf(" (%s)", cfg->user);
            printf(":\n\n");
            printf("  IMAP:\n");
            printf("    Host:     %s\n", cfg->host   ? cfg->host   : "(not set)");
            printf("    User:     %s\n", cfg->user   ? cfg->user   : "(not set)");
            printf("    Password: %s\n", cfg->pass   ? "****"      : "(not set)");
            printf("    Folder:   %s\n", cfg->folder ? cfg->folder : "INBOX");
            printf("\n  SMTP:\n");
            if (cfg->smtp_host) {
                printf("    Host:     %s\n", cfg->smtp_host);
                printf("    Port:     %d\n", cfg->smtp_port ? cfg->smtp_port : 587);
                printf("    User:     %s\n", cfg->smtp_user ? cfg->smtp_user : "(same as IMAP)");
                printf("    Password: %s\n", cfg->smtp_pass ? "****"         : "(same as IMAP)");
            } else {
                printf("    (not configured — will be derived from IMAP host)\n");
            }
            printf("\n");
            result = 0;

        } else if (strcmp(subcmd, "imap") == 0) {
            if (setup_wizard_imap(cfg) == 0) {
                if (config_save_to_store(cfg) == 0) {
                    printf("IMAP configuration saved.\n");
                    result = 0;
                } else {
                    fprintf(stderr, "Error: Could not save configuration.\n");
                }
            }

        } else if (strcmp(subcmd, "smtp") == 0) {
            if (setup_wizard_smtp(cfg) == 0) {
                if (config_save_to_store(cfg) == 0) {
                    printf("SMTP configuration saved.\n");
                    result = 0;
                } else {
                    fprintf(stderr, "Error: Could not save configuration.\n");
                }
            }

        } else {
            if (subcmd[0])
                fprintf(stderr, "Unknown config subcommand '%s'.\n", subcmd);
            help_config();
            result = subcmd[0] ? -1 : 0;
        }

    } else if (strcmp(cmd, "send") == 0) {
        const char *to = NULL, *subject = NULL, *body = NULL;
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (strcmp(argv[i], "--to") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --to requires an address.\n"); ok = 0;
                } else to = argv[++i];
            } else if (strcmp(argv[i], "--subject") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --subject requires text.\n"); ok = 0;
                } else subject = argv[++i];
            } else if (strcmp(argv[i], "--body") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --body requires text.\n"); ok = 0;
                } else body = argv[++i];
            } else {
                unknown_option("send", argv[i]); ok = 0;
            }
        }
        if (ok) {
            if (!to || !to[0] || !subject || !body) {
                fprintf(stderr, "Error: --to, --subject, and --body are all required.\n");
                help_send();
            } else {
                const char *from = cfg->smtp_user ? cfg->smtp_user : cfg->user;
                ComposeParams p = {from, to, subject, body, NULL};
                char *msg = NULL;
                size_t msg_len = 0;
                if (compose_build_message(&p, &msg, &msg_len) != 0) {
                    fprintf(stderr, "Error: Failed to build message.\n");
                } else {
                    result = smtp_send(cfg, from, to, msg, msg_len);
                    if (result == 0) {
                        printf("Message sent.\n");
                        if (email_service_save_sent(cfg, msg, msg_len) == 0)
                            printf("Saved.\n");
                        else
                            fprintf(stderr, "(Could not save to Sent folder — "
                                    "check EMAIL_SENT_FOLDER in config.)\n");
                    }
                    free(msg);
                }
            }
        }

    } else if (strcmp(cmd, "mark-read") == 0 || strcmp(cmd, "mark-unread") == 0) {
        const char *uid_str = NULL;
        const char *folder  = NULL;
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (strcmp(argv[i], "--folder") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --folder requires a folder name.\n"); ok = 0;
                } else folder = argv[++i];
            } else if (!uid_str) {
                uid_str = argv[i];
            } else {
                unknown_option(cmd, argv[i]); ok = 0;
            }
        }
        if (ok) {
            if (!uid_str) {
                fprintf(stderr, "Error: '%s' requires a UID argument.\n", cmd);
                help_mark_read();
            } else {
                char uid[17];
                if (parse_uid(uid_str, uid) != 0)
                    fprintf(stderr, "Error: UID must be a positive integer (got '%s').\n", uid_str);
                else {
                    int flag_add = (strcmp(cmd, "mark-unread") == 0) ? 1 : 0;
                    result = email_service_set_flag(cfg, uid, folder, MSG_FLAG_UNSEEN, flag_add);
                }
            }
        }

    } else if (strcmp(cmd, "mark-starred") == 0 || strcmp(cmd, "remove-starred") == 0) {
        const char *uid_str = NULL;
        const char *folder  = NULL;
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (strcmp(argv[i], "--folder") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --folder requires a folder name.\n"); ok = 0;
                } else folder = argv[++i];
            } else if (!uid_str) {
                uid_str = argv[i];
            } else {
                unknown_option(cmd, argv[i]); ok = 0;
            }
        }
        if (ok) {
            if (!uid_str) {
                fprintf(stderr, "Error: '%s' requires a UID argument.\n", cmd);
                help_mark_starred();
            } else {
                char uid[17];
                if (parse_uid(uid_str, uid) != 0)
                    fprintf(stderr, "Error: UID must be a positive integer (got '%s').\n", uid_str);
                else {
                    int flag_add = (strcmp(cmd, "mark-starred") == 0) ? 1 : 0;
                    result = email_service_set_flag(cfg, uid, folder, MSG_FLAG_FLAGGED, flag_add);
                }
            }
        }

    } else if (strcmp(cmd, "add-label") == 0 || strcmp(cmd, "remove-label") == 0) {
        const char *uid_str = NULL;
        const char *label   = NULL;
        int argn = 0;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (argn == 0)      { uid_str = argv[i]; argn++; }
            else if (argn == 1) { label   = argv[i]; argn++; }
        }
        if (!uid_str || !label) {
            fprintf(stderr, "Error: '%s' requires a UID and a label.\n", cmd);
            help_add_label();
        } else {
            char uid[17];
            if (parse_uid(uid_str, uid) != 0)
                fprintf(stderr, "Error: UID must be a positive integer (got '%s').\n", uid_str);
            else {
                int add = (strcmp(cmd, "add-label") == 0) ? 1 : 0;
                result = email_service_set_label(cfg, uid, label, add);
            }
        }

    } else if (strcmp(cmd, "list-labels") == 0) {
        result = email_service_list_labels(cfg);

    } else if (strcmp(cmd, "create-label") == 0) {
        const char *name = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            name = argv[i]; break;
        }
        if (!name) {
            fprintf(stderr, "Error: 'create-label' requires a label name.\n");
            help_create_label();
        } else {
            result = email_service_create_label(cfg, name);
        }

    } else if (strcmp(cmd, "delete-label") == 0) {
        const char *label_id = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            label_id = argv[i]; break;
        }
        if (!label_id) {
            fprintf(stderr, "Error: 'delete-label' requires a label ID.\n");
            help_delete_label();
        } else {
            result = email_service_delete_label(cfg, label_id);
        }

    } else if (strcmp(cmd, "show-accounts") == 0) {
        int count = 0;
        AccountEntry *accs = config_list_accounts(&count);
        if (count == 0) {
            printf("No accounts configured.\n");
            result = 0;
        } else {
            printf("%-40s  %-8s  %s\n", "Account", "Type", "Server");
            printf("%-40s  %-8s  %s\n",
                   "----------------------------------------",
                   "--------",
                   "----------------------------");
            for (int i = 0; i < count; i++) {
                const char *type   = (accs[i].cfg && accs[i].cfg->gmail_mode) ? "Gmail" : "IMAP";
                const char *server = accs[i].cfg ? (accs[i].cfg->host ? accs[i].cfg->host : "-") : "-";
                printf("%-40s  %-8s  %s\n",
                       accs[i].name ? accs[i].name : "?",
                       type, server);
            }
            config_free_account_list(accs, count);
            result = 0;
        }

    } else if (strcmp(cmd, "add-account") == 0) {
        Config *new_cfg = setup_wizard_run();
        if (new_cfg) {
            if (config_save_account(new_cfg) == 0) {
                printf("Account '%s' added.\n", new_cfg->user ? new_cfg->user : "?");
                result = 0;
            } else {
                fprintf(stderr, "Error: Failed to save account.\n");
            }
            config_free(new_cfg);
        } else {
            fprintf(stderr, "Account setup cancelled.\n");
            result = -1;
        }

    } else if (strcmp(cmd, "remove-account") == 0) {
        const char *account_name = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            account_name = argv[i]; break;
        }
        if (!account_name) {
            fprintf(stderr, "Error: 'remove-account' requires an account email.\n");
            help_remove_account();
        } else {
            /* Verify account exists */
            int acc_count = 0;
            AccountEntry *accs = config_list_accounts(&acc_count);
            int found = 0;
            for (int i = 0; i < acc_count && !found; i++)
                if (accs[i].name && strcmp(accs[i].name, account_name) == 0) found = 1;
            config_free_account_list(accs, acc_count);

            if (!found) {
                fprintf(stderr, "Error: Account '%s' not found.\n", account_name);
            } else {
                config_delete_account(account_name);

                const char *data_base = platform_data_dir();
                printf("Account '%s' removed.\n", account_name);
                printf("\n");
                printf("Local messages have been PRESERVED and are NOT deleted.\n");
                if (data_base) {
                    printf("Local data directory: %s/email-cli/accounts/%s/\n",
                           data_base, account_name);
                    printf("To delete local messages manually:\n");
                    printf("  rm -rf %s/email-cli/accounts/%s/\n",
                           data_base, account_name);
                }
                result = 0;
            }
        }

    } else if (strcmp(cmd, "migrate-credentials") == 0) {
        int obfus = app_settings_get_obfuscation();
        printf("Migrating credentials (%s)...\n",
               obfus ? "encrypting stored passwords" : "removing encryption");
        int rc = config_migrate_credentials();
        if (rc == 0) {
            printf("Done. All account credentials are now %s.\n",
                   obfus ? "obfuscated (enc: prefix)" : "stored as plaintext");
            result = 0;
        } else {
            fprintf(stderr, "Error: one or more accounts could not be migrated.\n");
        }

    } else {
        fprintf(stderr, "Unknown command '%s'.\n", cmd);
        fprintf(stderr, "Run 'email-cli help' for available commands.\n");
    }

    /* 7. Cleanup */
    config_free(cfg);
    logger_log(LOG_INFO, "--- email-cli session finished ---");
    logger_close();

    if (result >= 0)
        return EXIT_SUCCESS;
    fprintf(stderr, "\nFailed. Check logs in %s\n", log_file);
    return EXIT_FAILURE;
}
