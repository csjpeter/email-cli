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
 * Supported commands: list, show, folders, attachments, save-attachment, help.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#include "config_store.h"
#include "email_service.h"
#include "platform/terminal.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include "local_store.h"
#include "fs_util.h"
#include "config.h"

#define BATCH_DEFAULT_LIMIT 100

/* ── Help pages ──────────────────────────────────────────────────────── */

static void help_general(void) {
    printf(
        "Usage: email-cli-ro [<account>] <command> [options]\n"
        "\n"
        "Read-only email CLI. All output is non-interactive (batch mode).\n"
        "Safe for use by AI agents: no send or write operations are available.\n"
        "\n"
        "  <account>  Email address of the account to use (e.g. user@example.com).\n"
        "             Required when multiple accounts are configured.\n"
        "             Alternative: --account <email>.\n"
        "\n"
        "Reading:\n"
        "  list                       List messages in the configured mailbox\n"
        "  show <uid>                 Display the full content of a message\n"
        "  list-folders               List available IMAP folders / Gmail labels\n"
        "  list-labels                List all labels (Gmail) or folders (IMAP)\n"
        "  list-attachments <uid>     List attachments in a message\n"
        "  save-attachment <uid> <filename> [dir]\n"
        "                             Save an attachment to disk\n"
        "\n"
        "\n"
        "Finding messages (see 'email-cli-ro help list' for details):\n"
        "  list --from <text>            Filter by sender substring\n"
        "  list --since/--before <date>  Filter by date (YYYY-MM-DD)\n"
        "  list --json                   Machine-readable output\n"
        "  list --all-accounts           Every configured account in turn\n"
        "  list --folder __unread__      Virtual folders: __unread__ __flagged__\n"
        "                                __answered__ __forwarded__ __junk__\n"
        "                                __phishing__ __all__\n"
        "  list --folder \"__search__:3:text\"  Search cached mail\n"
        "                                (scope 0=Subject 1=From 2=To 3=Body)\n"
        "  show <uid> --raw              Message source, undecoded\n"
        "Account management:\n"
        "  list-accounts              List all configured accounts\n"
        "\n"
        "Help:\n"
        "  help [command]             Show this help, or detailed help for a command\n"
        "\n"
        "Run 'email-cli-ro help <command>' for more information.\n"
        "For write operations (send, mark-read, add-label, etc.) use 'email-cli'.\n"
    );
}

/* Accept only YYYY-MM-DD for --since/--before: the manifest stores dates in
 * that form, so anything else would silently match nothing. */
static int valid_date_arg(const char *s) {
    if (!s) return 0;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return 0; }
        else if (s[i] < '0' || s[i] > '9') return 0;
    }
    return s[10] == '\0';
}

static void help_list(void) {
    printf(
        "Usage: email-cli-ro [<account>] list [options]\n"
        "\n"
        "Lists messages in the configured mailbox.\n"
        "Shows unread (UNSEEN) messages by default; use --all for everything.\n"
        "\n"
        "Options:\n"
        "  --all                    Show all messages (not just unread).\n"
        "  --folder <name>          IMAP: use <name> instead of the configured folder.\n"
        "  --label  <id-or-name>    Gmail: filter by label (alias for --folder).\n"
        "  --limit <n>              Show at most <n> messages (default: %d).\n"
        "  --offset <n>             Start listing from the <n>-th message (1-based).\n"
        "  --from <text>            Only messages whose From contains <text>\n"
        "                           (case-insensitive substring).\n"
        "  --since <YYYY-MM-DD>     Only messages on or after this date.\n"
        "  --before <YYYY-MM-DD>    Only messages strictly before this date.\n"
        "  --all-accounts           List every configured account in turn, each\n"
        "                           under an \"=== <account> ===\" heading.\n"
        "                           Cannot be combined with --json.\n"
        "  --json                   Emit JSON instead of the text table: one\n"
        "                           object per message, fields never truncated.\n"
        "                           stdout stays a single parseable document;\n"
        "                           notices go to stderr.\n"

        "\n"
        "Virtual folders (pass to --folder; they span every cached folder):\n"
        "  __unread__     Unread messages\n"
        "  __flagged__    Starred / flagged messages\n"
        "  __answered__   Messages that were replied to\n"
        "  __forwarded__  Messages that were forwarded\n"
        "  __junk__       Messages marked as junk / spam\n"
        "  __phishing__   Messages flagged as phishing\n"
        "  __all__        Every cached message, from every folder\n"
        "\n"
        "Content search (pass to --folder):\n"
        "  __search__:<scope>:<query>   scope 0=Subject 1=From 2=To 3=Body\n"
        "  Searches the locally cached messages, so it works offline.  Body\n"
        "  search runs on the decoded text: base64 / quoted-printable parts and\n"
        "  non-UTF-8 charsets are matched too, and HTML mail is searched as\n"
        "  rendered text.  Quote the argument -- it contains colons.\n"
        "\n"
        "Gmail notes:\n"
        "  Use 'email-cli-ro list-labels' to see available labels and their IDs.\n"
        "  Predefined labels: INBOX, SENT, DRAFT, SPAM, TRASH, STARRED, IMPORTANT.\n"
        "\n"
        "Examples (IMAP):\n"
        "  email-cli-ro list\n"
        "  email-cli-ro list --folder INBOX.Sent --limit 50\n"
        "  email-cli-ro list --folder __unread__ --limit 20\n"
        "  email-cli-ro list --all --from @shop.example --since 2024-01-01\n"
        "  email-cli-ro list --all --folder __all__ --json\n"
        "  email-cli-ro list --folder \"__search__:3:invoice\"   (search message bodies)\n"
        "\n"
        "Examples (Gmail):\n"
        "  email-cli-ro list\n"
        "  email-cli-ro list --label INBOX\n"
        "  email-cli-ro list --label SENT\n"
        "  email-cli-ro user@gmail.com list --label Label_42\n",
        BATCH_DEFAULT_LIMIT
    );
}

static void help_show(void) {
    printf(
        "Usage: email-cli-ro show <uid> [--folder <name>] [--raw]\n"
        "\n"
        "Displays the full content of the message identified by <uid>.\n"
        "\n"
        "  <uid>             Numeric IMAP UID shown by 'email-cli-ro list'\n"
        "  --folder <name>   Folder/label containing the message.\n"
        "                    IMAP UIDs are unique only within a mailbox, so the same UID\n"
        "                    usually means a different message in each folder.  Omitted,\n"
        "                    the folder is resolved from the local store: a UID cached in\n"
        "                    one folder is shown from there; if several hold it the\n"
        "                    configured folder wins and the others are named on stderr;\n"
        "                    if none of them is the configured folder the command fails\n"
        "                    and lists the candidates.  Cross-folder listings print a\n"
        "                    Folder column to pass back here.\n"
        "  --label  <name>   Gmail: alias for --folder.\n"
        "  --raw             Print the message exactly as stored (RFC 2822 source):\n"
        "                    no MIME parsing, no transfer-encoding or charset\n"
        "                    decoding, no HTML rendering.  Use it to inspect the\n"
        "                    real headers when text looks mis-decoded.\n"
        "\n"
        "The message is fetched from the server on first access and stored\n"
        "locally under ~/.local/share/email-cli/accounts/<account>/store/.\n"
        "The exact path of a message is printed by 'show' as the File: line.\n"
        "Subsequent reads are served from the local store.\n"
    );
}

static void help_folders(void) {
    printf(
        "Usage: email-cli-ro list-folders [options]\n"
        "\n"
        "Lists all available IMAP folders on the server.\n"
        "\n"
        "Options:\n"
        "  --tree    Render the folder hierarchy as a tree.\n"
        "\n"
        "Examples:\n"
        "  email-cli-ro list-folders\n"
        "  email-cli-ro list-folders --tree\n"
    );
}

static void help_attachments(void) {
    printf(
        "Usage: email-cli-ro list-attachments <uid>\n"
        "\n"
        "Lists all attachments in the message identified by <uid>.\n"
        "Prints one line per attachment: filename and decoded size.\n"
        "\n"
        "  <uid>   Numeric IMAP UID shown by 'email-cli-ro list'\n"
        "\n"
        "Examples:\n"
        "  email-cli-ro list-attachments 42\n"
    );
}

static void help_save_attachment(void) {
    printf(
        "Usage: email-cli-ro save-attachment <uid> <filename> [dir]\n"
        "\n"
        "Saves the named attachment from message <uid> to disk.\n"
        "\n"
        "  <uid>       Numeric IMAP UID shown by 'email-cli-ro list'\n"
        "  <filename>  Exact attachment filename shown by 'email-cli-ro list-attachments'\n"
        "  [dir]       Destination directory (default: ~/Downloads or ~)\n"
        "\n"
        "Examples:\n"
        "  email-cli-ro save-attachment 42 report.pdf\n"
        "  email-cli-ro save-attachment 42 report.pdf /tmp\n"
    );
}

static void help_list_labels(void) {
    printf(
        "Usage: email-cli-ro list-labels\n"
        "\n"
        "List all available labels (Gmail) or folders (IMAP).\n"
        "For Gmail, shows both the display name and the label ID.\n"
        "\n"
        "Examples:\n"
        "  email-cli-ro list-labels\n"
    );
}

static void help_list_accounts(void) {
    printf(
        "Usage: email-cli-ro list-accounts\n"
        "\n"
        "List all configured accounts with their type and server.\n"
        "\n"
        "Examples:\n"
        "  email-cli-ro list-accounts\n"
    );
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int parse_uid(const char *s, char uid_out[17]) {
    if (!s || !*s) return -1;
    /* Accept 16-character hex strings directly (Gmail message IDs shown by list). */
    if (strlen(s) == 16) {
        int all_hex = 1;
        for (int i = 0; i < 16; i++) {
            if (!isxdigit((unsigned char)s[i])) { all_hex = 0; break; }
        }
        if (all_hex) {
            memcpy(uid_out, s, 16);
            uid_out[16] = '\0';
            return 0;
        }
    }
    /* Accept positive decimal integers (IMAP UIDs). */
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v == 0 || v > 4294967295UL) return -1;
    snprintf(uid_out, 17, "%016lu", v);
    return 0;
}

static void unknown_option(const char *cmd, const char *opt) {
    fprintf(stderr, "Unknown option '%s' for '%s'.\n", opt, cmd);
    fprintf(stderr, "Run 'email-cli-ro help %s' for usage.\n", cmd);
}

/* ── Entry point ─────────────────────────────────────────────────────── */

#ifndef EMAIL_CLI_VERSION
#define EMAIL_CLI_VERSION "unknown"
#endif

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("email-cli-ro %s\n", EMAIL_CLI_VERSION);
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

    /* 2. Account + command detection (mirrors main.c logic).
     *    Supported forms:
     *      email-cli-ro [<account>] <command> [options]
     *      email-cli-ro --account <email> <command> [options]  */
    const char *account_arg = NULL;
    int account_arg_idx = -1;

    /* Pass A: scan for --account flag anywhere in args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--account") == 0 && i + 1 < argc) {
            account_arg = argv[++i]; continue;
        }
    }

    /* Pass B: if no --account flag, check whether first positional arg is an email */
    if (!account_arg) {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                if (strcmp(argv[i], "--account") == 0) i++;
                continue;
            }
            if (strchr(argv[i], '@')) {
                account_arg = argv[i];
                account_arg_idx = i;
            }
            break;
        }
    }

    /* Command: first non-flag, non-account arg */
    const char *cmd = NULL;
    int cmd_idx = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) continue;
        if (strcmp(argv[i], "--account") == 0) { i++; continue; }
        if (strcmp(argv[i], "--batch") == 0) continue; /* no-op: always batch */
        if (i == account_arg_idx) continue;
        cmd = argv[i]; cmd_idx = i; break;
    }

    /* --help anywhere: treat as "help <cmd>" */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            if (cmd && strcmp(cmd, "--help") != 0) {
                if (strcmp(cmd, "list")            == 0) { help_list();            return EXIT_SUCCESS; }
                if (strcmp(cmd, "show")            == 0) { help_show();            return EXIT_SUCCESS; }
                if (strcmp(cmd, "list-folders")         == 0) { help_folders();         return EXIT_SUCCESS; }
                if (strcmp(cmd, "list-attachments")     == 0) { help_attachments();     return EXIT_SUCCESS; }
                if (strcmp(cmd, "save-attachment") == 0) { help_save_attachment(); return EXIT_SUCCESS; }
                if (strcmp(cmd, "list-labels")     == 0) { help_list_labels();     return EXIT_SUCCESS; }
                if (strcmp(cmd, "list-accounts")   == 0) { help_list_accounts();   return EXIT_SUCCESS; }
            }
            help_general();
            return EXIT_SUCCESS;
        }
    }

    if (cmd && strcmp(cmd, "help") == 0) {
        const char *topic = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) { topic = argv[i]; break; }
        if (topic) {
            if (strcmp(topic, "list")            == 0) { help_list();            return EXIT_SUCCESS; }
            if (strcmp(topic, "show")            == 0) { help_show();            return EXIT_SUCCESS; }
            if (strcmp(topic, "list-folders")         == 0) { help_folders();         return EXIT_SUCCESS; }
            if (strcmp(topic, "list-attachments")     == 0) { help_attachments();     return EXIT_SUCCESS; }
            if (strcmp(topic, "save-attachment") == 0) { help_save_attachment(); return EXIT_SUCCESS; }
            if (strcmp(topic, "list-labels")     == 0) { help_list_labels();     return EXIT_SUCCESS; }
            if (strcmp(topic, "list-accounts")   == 0) { help_list_accounts();   return EXIT_SUCCESS; }
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

    /* --all-accounts iterates over every configured account, so the usual
     * "which account?" resolution below must be skipped for it. */
    int all_accounts = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--all-accounts") == 0) all_accounts = 1;

    /* 4. Load configuration — no wizard: must already exist */
    Config *cfg = NULL;
    if (strcmp(cmd, "list-accounts") != 0 && !all_accounts) {
        if (account_arg) {
            cfg = config_load_account(account_arg);
            if (!cfg) {
                fprintf(stderr,
                        "Error: Account '%s' not found.\n"
                        "Run 'email-cli-ro list-accounts' to list configured accounts.\n",
                        account_arg);
                logger_close();
                return EXIT_FAILURE;
            }
        } else {
            int count = 0;
            AccountEntry *list = config_list_accounts(&count);
            if (count == 1) {
                cfg = list[0].cfg; list[0].cfg = NULL;
                config_free_account_list(list, count);
            } else if (count > 1) {
                fprintf(stderr, "Multiple accounts configured. Specify which to use:\n");
                for (int i = 0; i < count; i++)
                    fprintf(stderr, "  email-cli-ro %s %s\n",
                            list[i].name ? list[i].name : "?", cmd ? cmd : "");
                fprintf(stderr, "Run 'email-cli-ro list-accounts' for the full list.\n");
                config_free_account_list(list, count);
                logger_close();
                return EXIT_FAILURE;
            } else {
                config_free_account_list(list, count);
                fprintf(stderr,
                        "Error: No configuration found.\n"
                        "Run 'email-cli' once to complete the setup wizard.\n");
                logger_close();
                return EXIT_FAILURE;
            }
        }
    }

    /* 5. Initialize local store */
    if (cfg && local_store_init(cfg->host, cfg->user) != 0)
        logger_log(LOG_WARN, "Failed to initialize local store for %s", cfg->host);

    /* 6. Dispatch — batch mode only (pager = 0) */
    int result = -1;

    if (strcmp(cmd, "list") == 0) {
        EmailListOpts opts = {0};
        opts.limit = BATCH_DEFAULT_LIMIT;
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) {
                /* accepted as no-op: email-cli-ro is always batch mode */
            } else if (strcmp(argv[i], "--all") == 0) {
                opts.all = 1;
            } else if (strcmp(argv[i], "--folder") == 0 ||
                       strcmp(argv[i], "--label")  == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: %s requires a name.\n", argv[i]);
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
            } else if (strcmp(argv[i], "--all-accounts") == 0) {
                /* handled before account resolution; accepted here as a no-op */
            } else if (strcmp(argv[i], "--json") == 0) {
                opts.json = 1;
            } else if (strcmp(argv[i], "--from") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --from requires a substring.\n");
                    ok = 0;
                } else {
                    opts.filter_from = argv[++i];
                }
            } else if (strcmp(argv[i], "--since") == 0 ||
                       strcmp(argv[i], "--before") == 0) {
                int is_since = (strcmp(argv[i], "--since") == 0);
                const char *name = argv[i];
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: %s requires a date (YYYY-MM-DD).\n", name);
                    ok = 0;
                } else {
                    const char *v = argv[++i];
                    if (!valid_date_arg(v)) {
                        fprintf(stderr,
                                "Error: %s must be a date in YYYY-MM-DD form (got '%s').\n",
                                name, v);
                        ok = 0;
                    } else if (is_since) {
                        opts.filter_since = v;
                    } else {
                        opts.filter_before = v;
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
        if (all_accounts) {
            /* Run the same listing for every configured account.  JSON mode is
             * refused rather than emitting several documents back to back:
             * stdout must stay one parseable value. */
            if (opts.json) {
                fprintf(stderr,
                        "Error: --all-accounts cannot be combined with --json yet.\n"
                        "Run one account at a time, e.g.:\n"
                        "  for a in $(%s list-accounts --batch); do %s \"$a\" list --json; done\n",
                        "email-cli-ro", "email-cli-ro");
                result = -1;
            } else if (!ok) {
                /* option error already reported */
            } else {
                int acc_count = 0;
                AccountEntry *accs = config_list_accounts(&acc_count);
                if (!accs || acc_count == 0) {
                    fprintf(stderr, "Error: No accounts configured.\n");
                    result = -1;
                } else {
                    result = 0;
                    for (int ai = 0; ai < acc_count; ai++) {
                        if (!accs[ai].cfg) continue;
                        printf("=== %s ===\n", accs[ai].name ? accs[ai].name : "?");
                        if (local_store_init(accs[ai].cfg->host, accs[ai].cfg->user) != 0) {
                            fprintf(stderr, "Warning: local store unavailable for %s\n",
                                    accs[ai].name ? accs[ai].name : "?");
                            continue;
                        }
                        EmailListOpts aopts = opts;
                        if (email_service_list(accs[ai].cfg, &aopts) < 0) result = -1;
                        printf("\n");
                    }
                }
                config_free_account_list(accs, acc_count);
            }
        } else if (ok) {
            result = email_service_list(cfg, &opts);
        }

    } else if (strcmp(cmd, "show") == 0) {
        const char *uid_str = NULL;
        const char *folder  = NULL;
        int raw_mode = 0;
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) {
                continue; /* no-op */
            } else if (strcmp(argv[i], "--raw") == 0) {
                raw_mode = 1;
            } else if (strcmp(argv[i], "--folder") == 0 ||
                       strcmp(argv[i], "--label")  == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: %s requires a name.\n", argv[i]);
                    ok = 0;
                } else {
                    folder = argv[++i];
                }
            } else if (!uid_str) {
                uid_str = argv[i];
            } else {
                unknown_option("show", argv[i]);
                ok = 0;
            }
        }
        if (!ok) {
            /* error already printed above */
        } else if (!uid_str) {
            fprintf(stderr, "Error: 'show' requires a UID argument.\n");
            help_show();
        } else {
            char uid[17];
            if (parse_uid(uid_str, uid) != 0)
                fprintf(stderr,
                        "Error: UID must be a positive integer (got '%s').\n",
                        uid_str);
            else
                result = raw_mode
                         ? email_service_read_raw(cfg, folder, uid)
                         : email_service_read(cfg, folder, uid, 0, BATCH_DEFAULT_LIMIT);
        }

    } else if (strcmp(cmd, "list-folders") == 0) {
        int tree = 0, ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) { /* no-op */
            } else if (strcmp(argv[i], "--tree") == 0)
                tree = 1;
            else { unknown_option("list-folders", argv[i]); ok = 0; }
        }
        if (ok) result = email_service_list_folders(cfg, tree);

    } else if (strcmp(cmd, "list-attachments") == 0) {
        const char *uid_str = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            uid_str = argv[i]; break;
        }
        if (!uid_str) {
            fprintf(stderr, "Error: 'list-attachments' requires a UID argument.\n");
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

    } else if (strcmp(cmd, "list-labels") == 0) {
        result = email_service_list_labels(cfg);

    } else if (strcmp(cmd, "list-accounts") == 0) {
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

    } else {
        /* Check if the command is a write-only command blocked in ro mode */
        static const char *ro_blocked[] = {
            "mark-read", "mark-unread", "mark-starred", "remove-starred",
            "add-label", "remove-label", "create-label", "delete-label",
            "create-folder", "delete-folder",
            "mark-junk", "mark-notjunk",
            "add-account", "remove-account", NULL
        };
        int blocked = 0;
        for (int i = 0; ro_blocked[i]; i++) {
            if (strcmp(cmd, ro_blocked[i]) == 0) {
                fprintf(stderr, "Error: '%s' is not available in read-only mode (email-cli-ro).\n", cmd);
                fprintf(stderr, "Use 'email-cli' for write operations.\n");
                config_free(cfg);
                logger_log(LOG_INFO, "--- email-cli-ro session finished ---");
                logger_close();
                return EXIT_FAILURE;
            }
        }
        if (!blocked) {
            fprintf(stderr, "Unknown command '%s'.\n", cmd);
            fprintf(stderr, "Run 'email-cli-ro help' for available commands.\n");
        }
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
