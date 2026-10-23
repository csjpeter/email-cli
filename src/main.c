/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-24 */
/* Entry point of email-cli: the batch and interactive command line client. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#include "config_store.h"
#include "setup_wizard.h"
#include "email_service.h"
#include "platform/path.h"
#include "platform/terminal.h"
#include "raii.h"
#include "logger.h"
#include "local_store.h"
#include "fs_util.h"
#include "mail_account.h"
#include "help_gmail.h"
#include "mail_rules.h"
#include "when_expr.h"
#include "history_service.h"
#include <unistd.h>

/** Exit path marker: the failure has already been explained on stderr. */
#define RESULT_REPORTED (-2)

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
        "Reading:\n"
        "  list                        List messages in the configured mailbox\n"
        "  show <uid>                  Display the full content of a message\n"
        "  list-folders                List IMAP folders (IMAP only)\n"
        "  list-labels                 List Gmail labels with IDs (Gmail only)\n"
        "  list-attachments <uid>      List attachments in a message\n"
        "  save-attachment <uid>       Save a named attachment to disk\n"
        "\n"
        "Writing:\n"
        "  send                        Send a message non-interactively\n"
        "  reply <uid> [--all]         Reply (to all) to a message\n"
        "  mark-read <uid>             Mark a message as read\n"
        "  mark-unread <uid>           Mark a message as unread\n"
        "  mark-starred <uid>          Star (flag) a message\n"
        "  remove-starred <uid>        Remove star from a message\n"
        "  add-label <uid> <lbl>       Add a Gmail label to a message\n"
        "  remove-label <uid> <lbl>    Remove a Gmail label from a message\n"
        "  create-label <name>         Create a Gmail label (Gmail only)\n"
        "  delete-label <id>           Delete a Gmail label (Gmail only)\n"
        "  create-folder <name>        Create an IMAP folder (IMAP only)\n"
        "  delete-folder <name>        Delete an IMAP folder (IMAP only)\n"
        "  move <uid> <folder>         Move a message to a folder (Gmail: label)\n"
        "  mark-junk <uid>             Mark a message as junk/spam\n"
        "  mark-notjunk <uid>          Mark a message as not-junk (ham)\n"
        "  trash <uid>                 Move a message to the trash\n"
        "  archive <uid>               Archive a message\n"
        "  forward <uid> --to <addr>   Forward a message\n"
        "\n"
        "\n"
        "Finding messages (see 'email-cli help list' for details):\n"
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
        "  list-accounts               List all configured accounts\n"
        "  add-account                 Add a new account (runs setup wizard)\n"
        "  remove-account <email>      Remove an account (local data preserved)\n"
        "  config                      View or update configuration (incl. SMTP)\n"
        "  migrate-credentials         Re-encrypt (or decrypt) all stored passwords\n"
        "\n"
        "Rules:\n"
        "  rules list                  List sorting rules for the account\n"
        "  rules apply [--dry-run]     Apply rules to locally cached messages\n"
        "  rules add --name ...        Add a sorting rule\n"
        "  rules edit --name ...       Change a sorting rule in place\n"
        "  rules remove --name ...     Remove a sorting rule by name\n"
        "\n"
        "History:\n"
        "  history [object]            Recorded operations: who, when, old and new value\n"
        "  revert <id>                 Take back a recorded operation\n"
        "\n"
        "Options for every command:\n"
        "  -q, --quiet                 Do not ask before an irreversible operation\n"
        "\n"
        "Help:\n"
        "  help [command]              Show this help, or detailed help for a command\n"
        "  help gmail                  Step-by-step Gmail OAuth2 setup guide\n"
        "  --help, -h                  Show this help (also: <command> --help)\n"
        "  --version                   Print the version\n"
        "\n"
        "Run 'email-cli help <command>' for more information.\n"
        "For the interactive TUI use email-tui.\n"
        "For background sync and cron management use email-sync.\n"
    );
}

static void help_rules(void) {
    printf(
        "Usage: email-cli [<account>] rules <subcommand> [options]\n"
        "\n"
        "Manage and apply mail sorting rules for an account.\n"
        "\n"
        "Subcommands:\n"
        "  rules list                  List all sorting rules for the account\n"
        "  rules apply [--dry-run]     Apply rules to locally cached messages\n"
        "  rules add --name <name>     Add a new sorting rule\n"
        "  rules edit --name <name>    Change a sorting rule in place\n"
        "  rules remove --name <name>  Remove a sorting rule by name\n"
        "\n"
        "Rules are stored in:\n"
        "  ~/.config/email-cli/accounts/<account>/rules.ini\n"
        "\n"
        "Options:\n"
        "  --account <email>   Account to operate on (required if multiple configured)\n"
        "  --help, -h          Show help for this command\n"
        "\n"
        "Examples:\n"
        "  email-cli rules list\n"
        "  email-cli rules apply --dry-run\n"
        "  email-cli rules add --name \"GitHub\" --if-from \"*@github.com\" --add-label GitHub\n"
        "  email-cli rules remove --name \"GitHub\"\n"
        "\n"
        "Run 'email-cli help rules <subcommand>' for details on each subcommand.\n"
    );
}

static void help_rules_list(void) {
    printf(
        "Usage: email-cli [<account>] rules list\n"
        "\n"
        "--json: print JSON, the complete record, instead of the table.\n"
        "A script reads --json, not the table.  Errors stay text on stderr.\n"
        "\n"
        "List all sorting rules configured for the account.\n"
        "\n"
        "Rules are stored in:\n"
        "  ~/.config/email-cli/accounts/<account>/rules.ini\n"
        "\n"
        "Each rule matches incoming messages by From / Subject / To / Label\n"
        "(glob patterns) and applies label additions, removals, or folder moves.\n"
        "\n"
        "Options:\n"
        "  --account <email>   Account to inspect (required if multiple configured)\n"
        "  --help, -h          Show this help message\n"
        "\n"
        "Examples:\n"
        "  email-cli rules list\n"
        "  email-cli user@example.com rules list\n"
    );
}

static void help_rules_apply(void) {
    printf(
        "Usage: email-cli [<account>] rules apply [--dry-run] [--verbose]\n"
        "\n"
        "Apply mail sorting rules retroactively to all locally cached messages.\n"
        "Does not re-download messages or contact the server.\n"
        "\n"
        "Options:\n"
        "  --dry-run           Print what would change without writing anything\n"
        "  --verbose, -v       Show per-rule output for each matching message\n"
        "  --account <email>   Account to process (required if multiple configured)\n"
        "  --help, -h          Show this help message\n"
        "\n"
        "Examples:\n"
        "  email-cli rules apply\n"
        "  email-cli rules apply --dry-run\n"
        "  email-cli rules apply --verbose\n"
        "  email-cli user@example.com rules apply --dry-run --verbose\n"
    );
}

static void help_rules_add(void) {
    printf(
        "Usage: email-cli [<account>] rules add --name <name>\n"
        "         [--if-from <glob>] [--if-subject <glob>]\n"
        "         [--if-to <glob>] [--if-label <glob>]\n"
        "         [--add-label <label>]... [--remove-label <label>]...\n"
        "         [--move-folder <folder>]\n"
        "\n"
        "Add a new mail sorting rule for the account.\n"
        "\n"
        "Conditions (all omitted conditions match everything):\n"
        "  --if-from <glob>      Match sender address (glob pattern)\n"
        "  --if-subject <glob>   Match message subject (glob pattern)\n"
        "  --if-to <glob>        Match recipient address (glob pattern)\n"
        "  --if-label <glob>     Match an existing label (glob pattern)\n"
        "\n"
        "Actions:\n"
        "  --add-label <label>      Add this label to matching messages\n"
        "  --remove-label <label>   Remove this label from matching messages\n"
        "  --move-folder <folder>   Move to folder (IMAP only; ignored on Gmail)\n"
        "\n"
        "Options:\n"
        "  --name <name>       Rule name (required, must be unique)\n"
        "  --account <email>   Account to add rule to (required if multiple)\n"
        "  --help, -h          Show this help message\n"
        "\n"
        "Examples:\n"
        "  email-cli rules add --name \"GitHub\" --if-from \"*@github.com\"\n"
        "                      --add-label GitHub --remove-label INBOX\n"
        "  email-cli rules add --name \"Newsletters\" --if-subject \"*newsletter*\"\n"
        "                      --add-label Newsletter\n"
    );
}

static void help_rules_edit(void) {
    printf(
        "Usage: email-cli [<account>] rules edit --name <name>\n"
        "         [--new-name <name>]\n"
        "         [--if-from <glob>] [--if-subject <glob>]\n"
        "         [--if-to <glob>] [--if-label <glob>]\n"
        "         [--add-label <label>]... [--remove-label <label>]...\n"
        "         [--move-folder <folder>]\n"
        "\n"
        "Change the named sorting rule in place; its place in the list is kept.\n"
        "Only what you give is changed:\n"
        "  --new-name <name>        Rename the rule (the new name must be unused)\n"
        "  --if-from, --if-subject, --if-to, --if-label\n"
        "                           Replace the rule's whole condition by the ones given\n"
        "  --add-label <label>      Replace the labels to add (repeatable)\n"
        "  --remove-label <label>   Replace the labels to remove (repeatable)\n"
        "  --move-folder <folder>   Replace the destination folder (\"\" clears it)\n"
        "\n"
        "Options:\n"
        "  --name <name>       Exact name of the rule to edit (required)\n"
        "  --account <email>   Account of the rule (required if multiple)\n"
        "  --help, -h          Show this help message\n"
        "\n"
        "Examples:\n"
        "  email-cli rules edit --name \"GitHub\" --add-label Code\n"
        "  email-cli rules edit --name \"GitHub\" --new-name \"Git hosting\"\n"
    );
}

static void help_rules_remove(void) {
    printf(
        "Usage: email-cli [<account>] rules remove --name <name>\n"
        "\n"
        "Remove a mail sorting rule by name.\n"
        "\n"
        "Options:\n"
        "  --name <name>       Exact name of the rule to remove (required)\n"
        "  --account <email>   Account to remove rule from (required if multiple)\n"
        "  --help, -h          Show this help message\n"
        "\n"
        "Examples:\n"
        "  email-cli rules remove --name \"GitHub\"\n"
        "  email-cli user@example.com rules remove --name \"Newsletters\"\n"
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
        "Usage: email-cli [<account>] list [options]\n"
        "\n"
        "Lists messages in the configured mailbox.\n"
        "Shows unread (UNSEEN) messages by default; use --all for everything.\n"
        "\n"
        "Options:\n"
        "  --all                    Show all messages (not just unread).\n"
        "                           Unread messages are marked with 'N' and listed first.\n"
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
        "  IMAP only.  Gmail has the same thing as real labels: use\n"
        "  --folder UNREAD, --folder STARRED, ... ('list-labels' shows them).\n"
        "\n"
        "Content search (pass to --folder):\n"
        "  __search__:<scope>:<query>   scope 0=Subject 1=From 2=To 3=Body\n"
        "  Searches the locally cached messages, so it works offline.  Body\n"
        "  search runs on the decoded text: base64 / quoted-printable parts and\n"
        "  non-UTF-8 charsets are matched too, and HTML mail is searched as\n"
        "  rendered text.  Quote the argument -- it contains colons.\n"
        "\n"
        "Gmail notes:\n"
        "  Use 'email-cli list-labels' to see available labels and their IDs.\n"
        "  Predefined labels: INBOX, SENT, DRAFT, SPAM, TRASH, STARRED, IMPORTANT.\n"
        "\n"
        "Examples (IMAP):\n"
        "  email-cli list\n"
        "  email-cli list --all\n"
        "  email-cli list --all --offset 21\n"
        "  email-cli list --folder INBOX.Sent --limit 50\n"
        "  email-cli list --folder __unread__ --limit 20\n"
        "  email-cli list --all --from @shop.example --since 2024-01-01\n"
        "  email-cli list --all --folder __all__ --json\n"
        "  email-cli list --folder \"__search__:3:invoice\"   (search message bodies)\n"
        "\n"
        "Examples (Gmail):\n"
        "  email-cli list\n"
        "  email-cli list --label INBOX\n"
        "  email-cli list --label SENT\n"
        "  email-cli user@gmail.com list --label Label_42\n",
        BATCH_DEFAULT_LIMIT
    );
}

static void help_show(void) {
    printf(
        "Usage: email-cli show <uid> [--folder <name>] [--raw]\n"
        "\n"
        "--json: print JSON, the complete record, instead of the table.\n"
        "A script reads --json, not the table.  Errors stay text on stderr.\n"
        "\n"
        "Displays the full content of the message identified by <uid>.\n"
        "\n"
        "  <uid>             Numeric IMAP UID shown by 'email-cli list'\n"
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
        "\n"
        "Examples:\n"
        "  email-cli show 42\n"
        "  email-cli show 42 --folder \"Archive\"\n"
    );
}

static void help_folders(void) {
    printf(
        "Usage: email-cli list-folders [options]\n"
        "\n"
        "--json: print JSON, the complete record, instead of the table.\n"
        "A script reads --json, not the table.  Errors stay text on stderr.\n"
        "\n"
        "Lists all IMAP folders on the server. IMAP accounts only.\n"
        "Use 'list-labels' for Gmail accounts.\n"
        "\n"
        "Options:\n"
        "  --tree    Render the folder hierarchy as a tree.\n"
        "\n"
        "Examples:\n"
        "  email-cli list-folders\n"
        "  email-cli list-folders --tree\n"
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
        "  --attach <path>   Attach a file; repeat for several (max 8).\n"
        "                    The MIME type is inferred from the extension.\n"
        "\n"
        "SMTP settings must be configured (run 'email-cli config smtp').\n"
        "\n"
        "Examples:\n"
        "  email-cli send --to friend@example.com --subject \"Hello\" --body \"Hi there!\"\n"
    );
}

/**
 * Renew a Gmail account's OAuth2 authorization and persist the new token.
 *
 * Google revokes refresh tokens carrying Gmail scopes when the account
 * password changes, and until this existed the only way back was to delete the
 * account and add it again — which also threw away its local store.
 *
 * @param import  1 = take over a refresh token read from standard input
 *                (obtained on another machine) instead of asking Google;
 *                it is tested before it is saved.
 * @return 0 on success, RESULT_REPORTED when it failed for a reason already
 *         written to stderr.
 */
static int config_reauth_gmail(Config *cfg, GmailLoginMode mode, int import) {
    if (import) {
        char *old = cfg->gmail_refresh_token ? strdup(cfg->gmail_refresh_token) : NULL;
        if (gmail_login_import_token(cfg, stdin) != 0) {
            fprintf(stderr, "Error: expected one refresh token on standard input.\n");
            free(old);
            return RESULT_REPORTED;
        }
        if (email_service_check_credential(cfg) != 0) {
            free(cfg->gmail_refresh_token);
            cfg->gmail_refresh_token = old;
            fprintf(stderr, "The token was not accepted — the account is unchanged.\n");
            return RESULT_REPORTED;
        }
        free(old);
        if (config_save_to_store(cfg) != 0) {
            fprintf(stderr, "Error: the token works but could not be saved.\n");
            return RESULT_REPORTED;
        }
        printf("Gmail authorization taken over.\n");
        return 0;
    }
    if (gmail_login(cfg, mode, stdin) != 0) {
        fprintf(stderr, "Authorization was not completed — the account is unchanged.\n");
        return RESULT_REPORTED;
    }
    if (config_save_to_store(cfg) != 0) {
        fprintf(stderr,
                "Error: the authorization succeeded but the new token could not"
                " be saved.\n");
        return RESULT_REPORTED;
    }
    printf("Gmail authorization renewed.\n");
    return email_service_check_credential(cfg) == 0 ? 0 : RESULT_REPORTED;
}

static void help_config(void) {
    printf(
        "Usage: email-cli [<account>] config <subcommand>\n"
        "\n"
        "View or update configuration settings.\n"
        "\n"
        "Subcommands:\n"
        "  show      Print current configuration (passwords masked)\n"
        "  password  Change nothing but the password, then test it\n"
        "  reauth    Renew a Gmail account's authorization, then test it\n"
        "              --no-browser    only show the URL (SSH-forward the printed port)\n"
        "              --paste         no listener: paste the redirect address back\n"
        "              --import-token  read a refresh token from stdin (made elsewhere)\n"
        "  imap      Interactively configure IMAP (incoming mail) settings\n"
        "  smtp      Interactively configure SMTP (outgoing mail) settings\n"
        "\n"
        "A password changed at the provider is repaired with 'config password'.\n"
        "A Gmail account has no password here: access is granted by an OAuth2\n"
        "authorization, and changing the Google account password revokes it, so\n"
        "'config reauth' is what puts a Gmail account back to work.  Either\n"
        "subcommand sends you to the right one if you pick the other.\n"
        "\n"
        "SMTP settings are used by email-tui for composing and sending mail.\n"
        "Configuring them here writes to the shared config file.\n"
        "\n"
        "Examples:\n"
        "  email-cli config show\n"
        "  email-cli config password\n"
        "  email-cli config reauth\n"
        "  email-cli config imap\n"
        "  email-cli user@example.com config show\n"
    );
}

static void help_attachments(void) {
    printf(
        "Usage: email-cli list-attachments <uid>\n"
        "\n"
        "--json: print JSON, the complete record, instead of the table.\n"
        "A script reads --json, not the table.  Errors stay text on stderr.\n"
        "\n"
        "Lists all attachments in the message identified by <uid>.\n"
        "Prints one line per attachment: filename and decoded size.\n"
        "\n"
        "  <uid>   Numeric IMAP UID shown by 'email-cli list'\n"
        "\n"
        "Examples:\n"
        "  email-cli list-attachments 42\n"
    );
}

static void help_save_attachment(void) {
    printf(
        "Usage: email-cli save-attachment <uid> <filename> [dir]\n"
        "\n"
        "Saves the named attachment from message <uid> to disk.\n"
        "\n"
        "  <uid>       Numeric IMAP UID shown by 'email-cli list'\n"
        "  <filename>  Exact attachment filename shown by 'email-cli list-attachments'\n"
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
        "--json: print JSON, the complete record, instead of the table.\n"
        "A script reads --json, not the table.  Errors stay text on stderr.\n"
        "\n"
        "List all Gmail labels with their IDs. Gmail accounts only.\n"
        "Use 'list-folders' for IMAP accounts.\n"
        "\n"
        "Examples:\n"
        "  email-cli list-labels\n"
    );
}

static void help_create_label(void) {
    printf(
        "Usage: email-cli create-label <name>\n"
        "\n"
        "Create a new Gmail label. Gmail accounts only.\n"
        "\n"
        "  <name>  Display name for the new label\n"
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
        "Delete a Gmail label. Gmail accounts only.\n"
        "System labels (INBOX, TRASH, etc.) cannot be deleted.\n"
        "\n"
        "  <label-id>  Label ID (from list-labels)\n"
        "\n"
        "Examples:\n"
        "  email-cli delete-label Label_12345\n"
    );
}

static void help_create_folder(void) {
    printf(
        "Usage: email-cli create-folder <name>\n"
        "\n"
        "Create a new IMAP folder. IMAP accounts only.\n"
        "\n"
        "  <name>  Folder path / name\n"
        "\n"
        "Examples:\n"
        "  email-cli create-folder Work\n"
        "  email-cli create-folder Archive/2025\n"
    );
}

static void help_delete_folder(void) {
    printf(
        "Usage: email-cli delete-folder <name>\n"
        "\n"
        "Delete an IMAP folder. IMAP accounts only.\n"
        "System folders (INBOX, Trash, etc.) cannot be deleted.\n"
        "\n"
        "  <name>  Folder path / name\n"
        "\n"
        "Examples:\n"
        "  email-cli delete-folder OldWork\n"
    );
}

static void help_history(void) {
    printf(
        "Usage: email-cli [<account>] history [<object>] [--limit N]\n"
        "\n"
        "Show the recorded operations: who changed what, when, from which value to\n"
        "which.  Without <object> the recent changes are listed (yours; an\n"
        "administrator sees everyone's); with <object> every step of that object.\n"
        "\n"
        "  <object>    As the history names it: \"mail INBOX/000000000000002a\",\n"
        "              \"rule Newsletters\", \"account\", \"settings\", \"folder Work\"\n"
        "  --limit N   At most N steps (default 50)\n"
        "\n"
        "Each step ends in [revertable], [compensable], [not revertable], or says\n"
        "that it was reverted or is the revert of another step.\n"
        "Revert a step with: email-cli revert <id>\n"
        "\n"
        "Examples:\n"
        "  email-cli history\n"
        "  email-cli history \"mail INBOX/000000000000002a\"\n"
    );
}

static void help_revert(void) {
    printf(
        "Usage: email-cli revert <id> [-q]\n"
        "\n"
        "Take back the operation recorded as step <id> (see 'history').  The\n"
        "revert is a new step naming the step it reverts; nothing is rewritten, and\n"
        "the revert can itself be reverted.\n"
        "\n"
        "Refused when: the step is not revertable (a sent mail, a permanent delete,\n"
        "a secret); the state has changed since (the reason names what is there\n"
        "now, or is the server's own words); the mail is not in the local store; or\n"
        "the step is another user's and you are not an administrator.\n"
        "\n"
        "  -q, --quiet   Do not ask for confirmation\n"
        "\n"
        "Examples:\n"
        "  email-cli revert 12\n"
    );
}

static void help_move(void) {
    printf(
        "Usage: email-cli move <uid> <folder> [--folder <name>]\n"
        "\n"
        "Move a message to another folder.\n"
        "IMAP: copies it to <folder> and removes it from the source mailbox.\n"
        "Gmail: adds the label <folder> and removes the source label.\n"
        "\n"
        "  <uid>             UID of the message (decimal or 16-char hex)\n"
        "  <folder>          Destination folder (Gmail: label ID)\n"
        "  --folder <name>   Source mailbox (default: the account's folder)\n"
        "\n"
        "Examples:\n"
        "  email-cli move 12345 Archive\n"
        "  email-cli move 12345 Archive --folder INBOX\n"
    );
}

static void help_mark_junk(void) {
    printf(
        "Usage: email-cli mark-junk <uid>\n"
        "\n"
        "Mark a message as junk/spam.\n"
        "IMAP: sets $Junk and clears $NotJunk.\n"
        "Gmail: adds SPAM label and removes INBOX.\n"
        "\n"
        "  <uid>  UID of the message (decimal or 16-char hex)\n"
        "\n"
        "Examples:\n"
        "  email-cli mark-junk 12345\n"
    );
}

static void help_trash(void) {
    printf(
        "Usage: email-cli trash <uid> [--folder <folder>]\n"
        "       email-cli archive <uid> [--folder <folder>]\n"
        "\n"
        "trash: move a message to the trash (the TUI's 'D').\n"
        "  IMAP:  moves it to the trash folder (TRASH_FOLDER, default Trash).\n"
        "  Gmail: adds the TRASH label and removes the others.\n"
        "  A message that is already in the trash is deleted for good.\n"
        "\n"
        "archive: take a message out of the inbox (the TUI's 'a').\n"
        "  IMAP:  moves it to the archive folder (ARCHIVE_FOLDER, default Archive).\n"
        "  Gmail: removes its labels (INBOX included) and marks it read.\n"
        "\n"
        "  <uid>                Message UID (decimal or 16-char hex)\n"
        "  --folder <folder>    Folder (IMAP) or label (Gmail) the message is in;\n"
        "                       default: the account's folder\n"
        "\n"
        "Examples:\n"
        "  email-cli trash 12345\n"
        "  email-cli archive 12345 --folder INBOX\n"
    );
}

static void help_reply(void) {
    printf(
        "Usage: email-cli reply <uid> [--all] [--body <text>] [--folder <folder>]\n"
        "\n"
        "Reply to a message without interaction (the TUI's 'r' and 'R').\n"
        "The reply goes to the Reply-To or From address with the subject\n"
        "'Re: ...', In-Reply-To and References set, and the original quoted\n"
        "under your text. It is saved in Sent and the original is marked\n"
        "answered; that mark is local until the next sync.\n"
        "\n"
        "  <uid>                Message UID (decimal or 16-char hex)\n"
        "  --all                Reply to all: the original To and Cc (minus\n"
        "                       your own address) become the Cc\n"
        "  --body <text>        Your reply text (default: only the quote)\n"
        "  --folder <folder>    Folder (IMAP) or label (Gmail) the message is in;\n"
        "                       default: the account's folder\n"
        "\n"
        "Examples:\n"
        "  email-cli reply 12345 --body \"Thanks, done.\"\n"
        "  email-cli reply 12345 --all --body \"Agreed.\"\n"
    );
}

static void help_forward(void) {
    printf(
        "Usage: email-cli forward <uid> --to <addr> [--cc <addr>] [--body <text>]\n"
        "                         [--folder <folder>]\n"
        "\n"
        "Forwards a message (the TUI's 'F'): sends \"Fwd: <subject>\" with the\n"
        "original From, Date and text included, saves a copy in the Sent folder\n"
        "and flags the original $Forwarded.\n"
        "\n"
        "  <uid>              Message UID (decimal or 16-char hex)\n"
        "  --to <addr>        Recipient(s), comma separated (required)\n"
        "  --cc <addr>        Cc recipient(s)\n"
        "  --body <text>      A note placed above the forwarded message\n"
        "  --folder <folder>  Folder (IMAP) or label (Gmail) the message is in;\n"
        "                     default: the account's folder\n"
        "\n"
        "SMTP settings must be configured (run 'email-cli config smtp').\n"
        "\n"
        "Examples:\n"
        "  email-cli forward 12345 --to friend@example.org\n"
        "  email-cli forward 12345 --to a@example.org --body \"FYI\" --folder INBOX\n"
    );
}

static void help_mark_notjunk(void) {
    printf(
        "Usage: email-cli mark-notjunk <uid>\n"
        "\n"
        "Mark a message as not-junk (ham).\n"
        "IMAP: sets $NotJunk and clears $Junk.\n"
        "Gmail: removes SPAM label and adds INBOX.\n"
        "\n"
        "  <uid>  UID of the message (decimal or 16-char hex)\n"
        "\n"
        "Examples:\n"
        "  email-cli mark-notjunk 12345\n"
    );
}

static void help_list_accounts(void) {
    printf(
        "Usage: email-cli list-accounts\n"
        "\n"
        "--json: print JSON, the complete record, instead of the table.\n"
        "A script reads --json, not the table.  Errors stay text on stderr.\n"
        "\n"
        "List all configured accounts with their type and server.\n"
        "\n"
        "Examples:\n"
        "  email-cli list-accounts\n"
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
        "  <email>  Account email address shown by 'email-cli list-accounts'\n"
        "\n"
        "Examples:\n"
        "  email-cli remove-account user@example.com\n"
    );
}

static void help_migrate_credentials(void) {
    printf(
        "Usage: email-cli migrate-credentials\n"
        "\n"
        "Re-encrypt (or decrypt) the stored passwords of all accounts so that\n"
        "they match the current credential obfuscation setting.\n"
        "\n"
        "  obfuscation on   Stored passwords are written with the enc: prefix.\n"
        "  obfuscation off  Stored passwords are written as plaintext.\n"
        "\n"
        "The command takes no arguments and acts on every configured account.\n"
        "\n"
        "Examples:\n"
        "  email-cli migrate-credentials\n"
    );
}

static void help_help(void) {
    printf(
        "Usage: email-cli help [<command>]\n"
        "\n"
        "Show the general help, or the detailed help page of one command.\n"
        "'email-cli <command> --help' shows the same page as 'email-cli help <command>'.\n"
        "\n"
        "  <command>  A command word, e.g. list, send or rules apply\n"
        "\n"
        "Examples:\n"
        "  email-cli help\n"
        "  email-cli help list\n"
        "  email-cli help rules apply\n"
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
    fprintf(stderr, "Run 'email-cli help %s' for usage.\n", cmd);
}


/* ── Confirmation of the steps that cannot be taken back (EMAIL-226) ──── */

static int g_quiet;   /* -q / --quiet: no confirmation, no prompt */

/**
 * @brief Asks whether to go on with an irreversible operation; the default is
 *        No.  With -q the question is not asked and the answer is yes.
 * @param what  What is about to happen, one line.
 * @return 1 to go on, 0 when the answer is anything but "y" or "yes".
 */
static int confirm_irreversible(const char *what) {
    if (g_quiet) return 1;
    printf("%s\nThis cannot be undone and is not revertable. Continue? [y/N] ", what);
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) {
        printf("\n");
        return 0;
    }
    for (char *p = line; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strncmp(line, "y\n", 2) == 0 || strncmp(line, "yes\n", 4) == 0 ||
           strcmp(line, "y") == 0 || strcmp(line, "yes") == 0;
}

/** The confirmation of a revert (itself revertable): default No, -q skips. */
static int confirm_irreversible_or_revert(const char *what) {
    if (g_quiet) return 1;
    printf("%s\nContinue? [y/N] ", what);
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) {
        printf("\n");
        return 0;
    }
    for (char *p = line; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strncmp(line, "y\n", 2) == 0 || strncmp(line, "yes\n", 4) == 0;
}

/** Writes the answer "declined" and returns the result code for it. */
static int declined(void) {
    printf("Not done.\n");
    return RESULT_REPORTED;
}

/* ── history and revert ───────────────────────────────────────────────── */

static int history_colour(void) {
    return isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
}

/** Prints the steps of @p h (newest first) that belong in the view: those of
 *  @p object, or the recent changes when it is NULL. */
static int print_history(const OpHistory *h, const char *object, int limit) {
    const int recent = object == NULL;
    const int colour = history_colour();
    int shown = 0;
    if (colour) printf("\033[1m");
    printf("%-5s %-20s %-8s %-9s %s\n", "ID", "WHEN", "WHO", "OP", "WHAT");
    if (colour) printf("\033[0m");
    for (int i = 0; i < h->count && shown < limit; i++) {   /* newest first */
        const OpStep *s = &h->steps[i];
        if (object && strcmp(s->object, object) != 0) continue;
        if (!history_shown(s, recent)) continue;
        char desc[512];
        history_describe(s, desc, sizeof(desc));
        const char *state;
        char tail[64];
        if (s->reverts) {
            snprintf(tail, sizeof(tail), "revert of %lu", s->reverts);
            state = tail;
        } else if (s->kind == OPH_IRREVERSIBLE) {
            state = "not revertable";
        } else if (op_history_reverted_by(h, s->id)) {
            snprintf(tail, sizeof(tail), "reverted by %lu",
                     op_history_reverted_by(h, s->id));
            state = tail;
        } else if (s->kind == OPH_COMPENSABLE) {
            state = "compensable";
        } else {
            state = "revertable";
        }
        const int dim = s->kind == OPH_IRREVERSIBLE;
        if (dim && colour) printf("\033[2m");
        printf("%-5lu %-20.20s %-8.8s %-9.9s %s: %s [%s]\n", s->id, s->time,
               s->who, s->op, s->object, desc, state);
        if (dim && colour) printf("\033[0m");
        shown++;
    }
    if (shown == 0) printf("No operations recorded.\n");
    return 0;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

#ifndef EMAIL_CLI_VERSION
#define EMAIL_CLI_VERSION "unknown"
#endif

int main(int argc, char *argv[]) {
    /* 0. Set locale so wcwidth() and mbsrtowcs() work correctly for
     *    multi-byte UTF-8 characters (needed for column-width calculations). */
    setlocale(LC_ALL, "");

    /* TERM unset or dumb: no colour or erase codes in the output (EMAIL-108). */
    terminal_plain_output_if_needed();

    if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("email-cli %s\n", EMAIL_CLI_VERSION);
        return EXIT_SUCCESS;
    }

    /* -q / --quiet: no confirmation of irreversible operations (EMAIL-226).
     * Removed from argv so that no command has to know it. */
    {
        int w = 1;
        for (int r = 1; r < argc; r++) {
            if (strcmp(argv[r], "-q") == 0 || strcmp(argv[r], "--quiet") == 0) {
                g_quiet = 1;
                continue;
            }
            argv[w++] = argv[r];
        }
        argc = w;
        argv[argc] = NULL;
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
    int cmd_json = 0;   /* --json given (EMAIL-122) */
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
                if (strcmp(cmd, "list-folders")         == 0) { help_folders();         return EXIT_SUCCESS; }
                if (strcmp(cmd, "list-attachments")     == 0) { help_attachments();     return EXIT_SUCCESS; }
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
                if (strcmp(cmd, "create-folder")   == 0) { help_create_folder();   return EXIT_SUCCESS; }
                if (strcmp(cmd, "delete-folder")   == 0) { help_delete_folder();   return EXIT_SUCCESS; }
                if (strcmp(cmd, "move")            == 0) { help_move();            return EXIT_SUCCESS; }
                if (strcmp(cmd, "mark-junk")       == 0) { help_mark_junk();       return EXIT_SUCCESS; }
                if (strcmp(cmd, "trash")           == 0 ||
                    strcmp(cmd, "archive")         == 0) { help_trash();          return EXIT_SUCCESS; }
                if (strcmp(cmd, "reply")           == 0) { help_reply();           return EXIT_SUCCESS; }
                if (strcmp(cmd, "forward")         == 0) { help_forward();         return EXIT_SUCCESS; }
                if (strcmp(cmd, "mark-notjunk")    == 0) { help_mark_notjunk();    return EXIT_SUCCESS; }
                if (strcmp(cmd, "list-accounts")   == 0) { help_list_accounts();   return EXIT_SUCCESS; }
                if (strcmp(cmd, "add-account")     == 0) { help_add_account();     return EXIT_SUCCESS; }
                if (strcmp(cmd, "remove-account")  == 0) { help_remove_account();  return EXIT_SUCCESS; }
                if (strcmp(cmd, "rules")           == 0) { help_rules();           return EXIT_SUCCESS; }
                if (strcmp(cmd, "history")         == 0) { help_history();         return EXIT_SUCCESS; }
                if (strcmp(cmd, "revert")          == 0) { help_revert();          return EXIT_SUCCESS; }
                if (strcmp(cmd, "migrate-credentials") == 0) { help_migrate_credentials(); return EXIT_SUCCESS; }
                if (strcmp(cmd, "help")            == 0) { help_help();            return EXIT_SUCCESS; }
            }
            /* email-cli --help  or  email-cli help --help */
            help_general();
            return EXIT_SUCCESS;
        }
    }

    if (cmd && strcmp(cmd, "help") == 0) {
        /* First arg after the command is the topic */
        const char *topic = NULL;
        /* Collect up to two topic words for multi-word topics like "rules apply" */
        const char *topic2 = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (!topic) { topic = argv[i]; continue; }
            topic2 = argv[i]; break;
        }
        if (topic) {
            /* Multi-word topics */
            if (strcmp(topic, "rules") == 0 && topic2) {
                if (strcmp(topic2, "apply")  == 0) { help_rules_apply();  return EXIT_SUCCESS; }
                if (strcmp(topic2, "add")    == 0) { help_rules_add();    return EXIT_SUCCESS; }
                if (strcmp(topic2, "edit")   == 0) { help_rules_edit();   return EXIT_SUCCESS; }
                if (strcmp(topic2, "remove") == 0) { help_rules_remove(); return EXIT_SUCCESS; }
                if (strcmp(topic2, "list")   == 0) { help_rules_list();   return EXIT_SUCCESS; }
            }
            if (strcmp(topic, "list")            == 0) { help_list();            return EXIT_SUCCESS; }
            if (strcmp(topic, "show")            == 0) { help_show();            return EXIT_SUCCESS; }
            if (strcmp(topic, "list-folders")         == 0) { help_folders();         return EXIT_SUCCESS; }
            if (strcmp(topic, "list-attachments")     == 0) { help_attachments();     return EXIT_SUCCESS; }
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
            if (strcmp(topic, "create-folder")   == 0) { help_create_folder();   return EXIT_SUCCESS; }
            if (strcmp(topic, "delete-folder")   == 0) { help_delete_folder();   return EXIT_SUCCESS; }
            if (strcmp(topic, "move")            == 0) { help_move();            return EXIT_SUCCESS; }
            if (strcmp(topic, "mark-junk")       == 0) { help_mark_junk();       return EXIT_SUCCESS; }
            if (strcmp(topic, "trash")           == 0 ||
                strcmp(topic, "archive")         == 0) { help_trash();          return EXIT_SUCCESS; }
            if (strcmp(topic, "reply")           == 0) { help_reply();           return EXIT_SUCCESS; }
            if (strcmp(topic, "forward")         == 0) { help_forward();         return EXIT_SUCCESS; }
            if (strcmp(topic, "mark-notjunk")    == 0) { help_mark_notjunk();    return EXIT_SUCCESS; }
            if (strcmp(topic, "list-accounts")   == 0) { help_list_accounts();   return EXIT_SUCCESS; }
            if (strcmp(topic, "add-account")     == 0) { help_add_account();     return EXIT_SUCCESS; }
            if (strcmp(topic, "remove-account")  == 0) { help_remove_account();  return EXIT_SUCCESS; }
            if (strcmp(topic, "rules")           == 0) { help_rules();           return EXIT_SUCCESS; }
            if (strcmp(topic, "history")         == 0) { help_history();         return EXIT_SUCCESS; }
            if (strcmp(topic, "revert")          == 0) { help_revert();          return EXIT_SUCCESS; }
            if (strcmp(topic, "migrate-credentials") == 0) { help_migrate_credentials(); return EXIT_SUCCESS; }
            if (strcmp(topic, "help")            == 0) { help_help();            return EXIT_SUCCESS; }
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
    emc_log_open(log_file, 1);   /* libemail-client logs into the same file */
    logger_log(LOG_INFO, "--- email-cli starting (cmd: %s) ---", cmd);

    /* Determine if this command needs a specific account config.
     * migrate-credentials operates on all accounts and handles its own loading. */
    int cmd_needs_cfg = !(cmd && (strcmp(cmd, "add-account")        == 0 ||
                                  strcmp(cmd, "list-accounts")      == 0 ||
                                  strcmp(cmd, "remove-account")     == 0 ||
                                  strcmp(cmd, "migrate-credentials") == 0 ||
                                  strcmp(cmd, "history")            == 0 ||
                                  strcmp(cmd, "revert")             == 0 ||
                                  strcmp(cmd, "rules")              == 0));

    /* 4. Load configuration */
    /* --all-accounts iterates over every configured account, so the usual
     * "which account?" resolution below must be skipped for it. */
    int all_accounts = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--all-accounts") == 0) all_accounts = 1;

    Config *cfg = NULL;
    if (cmd_needs_cfg && !all_accounts) {
        if (account_arg) {
            /* Specific account requested */
            cfg = config_load_account(account_arg);
            if (!cfg) {
                fprintf(stderr,
                        "Error: Account '%s' not found.\n"
                        "Run 'email-cli list-accounts' to list configured accounts.\n",
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
                fprintf(stderr, "Run 'email-cli list-accounts' for the full list.\n");
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
                        /* The wizard used to promise a connection check and
                         * never make one.  Make it, so a mistyped password is
                         * found now and not at the first sync. */
                        email_service_check_credential(cfg);
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
    /* result: >= 0 succeeded; -1 failed and the log file may explain why;
     * RESULT_REPORTED failed for a reason already written to stderr, so the
     * "check the logs" pointer would only mislead. */
    int result = -1;

    /* EMAIL-122: --json on the read commands that offer it */
    {
        int want_json = 0;
        for (int i = cmd_idx + 1; i < argc; i++)
            if (strcmp(argv[i], "--json") == 0) want_json = 1;
        if (want_json && (strcmp(cmd, "show") == 0 || strcmp(cmd, "list-folders") == 0 ||
                          strcmp(cmd, "list-attachments") == 0 || strcmp(cmd, "list-labels") == 0))
            email_service_set_json(1);
        cmd_json = want_json;
    }

    if (strcmp(cmd, "list") == 0) {
        EmailListOpts opts = {0};
        opts.limit = BATCH_DEFAULT_LIMIT;
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) {
                continue; /* accepted as no-op */
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
                        "email-cli", "email-cli");
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
        /* UID is the first non --batch, non-option arg after "show" */
        const char *uid_str = NULL;
        const char *folder  = NULL;
        int raw_mode = 0;
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0 || strcmp(argv[i], "--json") == 0) {
                continue;
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
        } else if (raw_mode && cmd_json) {
            fprintf(stderr, "Error: --raw cannot be combined with --json.\n");
            result = -1;
        } else if (!uid_str) {
            fprintf(stderr, "Error: 'show' requires a UID argument.\n");
            help_show();
        } else {
            char uid[17];
            if (parse_uid(uid_str, uid) != 0)
                fprintf(stderr,
                        "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n",
                        uid_str);
            else
                result = raw_mode
                         ? email_service_read_raw(cfg, folder, uid)
                         : email_service_read(cfg, folder, uid, 0, BATCH_DEFAULT_LIMIT);
        }

    } else if (strcmp(cmd, "list-folders") == 0) {
        int tree = 0, ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0 || strcmp(argv[i], "--json") == 0) continue;
            if (strcmp(argv[i], "--tree") == 0)
                tree = 1;
            else { unknown_option("list-folders", argv[i]); ok = 0; }
        }
        if (ok) result = email_service_list_folders(cfg, tree);

    } else if (strcmp(cmd, "list-attachments") == 0) {
        const char *uid_str = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0 || strcmp(argv[i], "--json") == 0) continue;
            uid_str = argv[i]; break;
        }
        if (!uid_str) {
            fprintf(stderr, "Error: 'list-attachments' requires a UID argument.\n");
            help_attachments();
        } else {
            char uid[17];
            if (parse_uid(uid_str, uid) != 0)
                fprintf(stderr,
                        "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n",
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
                        "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n",
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

            if (cfg->gmail_mode) {
                /* A Gmail account uses none of the IMAP or SMTP fields.
                 * Printing them anyway is what told the reporter their
                 * account was password-authenticated and sent them into the
                 * IMAP wizard, where nothing they did could help. */
                printf("  Account type: Gmail (Gmail API over OAuth2)\n");
                printf("    Email:         %s\n", cfg->user ? cfg->user : "(not set)");
                printf("    Password:      none — Gmail accounts do not use one\n");
                printf("    Authorization: %s\n",
                       (cfg->gmail_refresh_token && cfg->gmail_refresh_token[0])
                           ? "stored (refresh token)"
                           : "missing — run 'email-cli config reauth'");
                printf("    OAuth2 client: %s\n",
                       (cfg->gmail_client_id && cfg->gmail_client_id[0])
                           ? "from this account's config"
                           : "built into this build");
                printf("    Default label: %s\n", cfg->folder ? cfg->folder : "INBOX");
                printf("\n"
                       "  A changed Google account password revokes the authorization.\n"
                       "  'email-cli config reauth' renews it.\n");
            } else {
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
            }
            printf("\n");
            result = 0;

        } else if (strcmp(subcmd, "password") == 0) {
            if (cfg->gmail_mode) {
                /* A Gmail account has no password here.  Saying so and then
                 * doing nothing is what sent the reporter down the IMAP
                 * wizard, so explain and carry out the repair they meant. */
                printf("This is a Gmail account: it has no password here.\n"
                       "Access is granted by an OAuth2 authorization you give\n"
                       "Google, and changing the Google account password revokes\n"
                       "it. Renewing that authorization now — 'config reauth'\n"
                       "does exactly this.\n");
                result = config_reauth_gmail(cfg, GMAIL_LOGIN_BROWSER, 0);
            } else {
                int rc = setup_wizard_password(cfg);
                if (rc < 0) {
                    fprintf(stderr, "Cancelled — the password was left unchanged.\n");
                    result = RESULT_REPORTED;
                } else if (rc == 0) {
                    /* Nothing to save; still worth telling the user whether
                     * the stored password works, since that is why they came. */
                    result = email_service_check_credential(cfg) == 0
                           ? 0 : RESULT_REPORTED;
                } else if (config_save_to_store(cfg) != 0) {
                    fprintf(stderr, "Error: Could not save configuration.\n");
                    result = RESULT_REPORTED;
                } else {
                    printf("Password saved.\n");
                    result = email_service_check_credential(cfg) == 0
                           ? 0 : RESULT_REPORTED;
                }
            }

        } else if (strcmp(subcmd, "reauth") == 0) {
            if (!cfg->gmail_mode) {
                fprintf(stderr,
                        "This is an IMAP account: there is no authorization to renew.\n"
                        "Run 'email-cli config password' to change its password.\n");
                result = RESULT_REPORTED;
            } else {
                GmailLoginMode lm = GMAIL_LOGIN_BROWSER;
                int import = 0, bad_opt = 0;
                for (int ai = cmd_idx + 2; ai < argc; ai++) {
                    if (strcmp(argv[ai], "--paste") == 0) lm = GMAIL_LOGIN_PASTE;
                    else if (strcmp(argv[ai], "--no-browser") == 0) lm = GMAIL_LOGIN_LOOPBACK;
                    else if (strcmp(argv[ai], "--import-token") == 0) import = 1;
                    else {
                        fprintf(stderr, "Unknown option for 'config reauth': %s\n"
                                        "Run 'email-cli help config'.\n", argv[ai]);
                        bad_opt = 1;
                    }
                }
                result = bad_opt ? RESULT_REPORTED : config_reauth_gmail(cfg, lm, import);
            }

        } else if (strcmp(subcmd, "imap") == 0) {
            if (cfg->gmail_mode) {
                /* It used to run, accept a new password, write EMAIL_HOST and
                 * EMAIL_PASS into the config and report "IMAP configuration
                 * saved" — while GMAIL_MODE=1 stayed and sync kept using the
                 * API.  A subcommand that reports success and changes nothing
                 * that matters is worse than one that refuses. */
                fprintf(stderr,
                        "This is a Gmail account: it does not use IMAP, so there are no\n"
                        "IMAP settings to change. Mail is fetched through the Gmail API\n"
                        "with an OAuth2 authorization.\n"
                        "  'email-cli config reauth' renews that authorization.\n"
                        "  'email-cli config show' lists what this account actually uses.\n");
                result = RESULT_REPORTED;
            } else if (setup_wizard_imap(cfg) == 0) {
                if (config_save_to_store(cfg) == 0) {
                    printf("IMAP configuration saved.\n");
                    result = 0;
                } else {
                    fprintf(stderr, "Error: Could not save configuration.\n");
                }
            } else {
                /* A cancelled wizard is not a malfunction: say what happened
                 * instead of sending the user to the log file. */
                fprintf(stderr, "Cancelled — the configuration was left unchanged.\n");
                result = RESULT_REPORTED;
            }

        } else if (strcmp(subcmd, "smtp") == 0) {
            if (cfg->gmail_mode) {
                fprintf(stderr,
                        "This is a Gmail account: outgoing mail goes through the Gmail\n"
                        "API, not SMTP, so there are no SMTP settings to change.\n"
                        "  'email-cli config show' lists what this account actually uses.\n");
                result = RESULT_REPORTED;
            } else if (setup_wizard_smtp(cfg) == 0) {
                if (config_save_to_store(cfg) == 0) {
                    printf("SMTP configuration saved.\n");
                    result = 0;
                } else {
                    fprintf(stderr, "Error: Could not save configuration.\n");
                }
            } else {
                fprintf(stderr, "Cancelled — the configuration was left unchanged.\n");
                result = RESULT_REPORTED;
            }

        } else {
            if (subcmd[0])
                fprintf(stderr, "Unknown config subcommand '%s'.\n", subcmd);
            help_config();
            result = subcmd[0] ? -1 : 0;
        }
        /* (config subcommands end here) */

    } else if (strcmp(cmd, "send") == 0) {
        const char *to = NULL, *subject = NULL, *body = NULL;
        /* US-84: --attach <path> (may appear multiple times) */
        const char *attach_arr[8];
        int attach_count = 0;
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
            } else if (strcmp(argv[i], "--attach") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --attach requires a file path.\n"); ok = 0;
                } else if (attach_count < 8) {
                    attach_arr[attach_count++] = argv[++i];
                } else {
                    fprintf(stderr, "Error: maximum 8 attachments allowed.\n"); ok = 0;
                }
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
                char q[600];
                snprintf(q, sizeof(q), "Send the message to %s.", to);
                if (!confirm_irreversible(q)) {
                    result = declined();
                    goto send_done;
                }
                emc_message p = {.from = from, .to = to, .subject = subject,
                                 .body = body,
                                 .attachments = attach_count > 0 ? attach_arr : NULL,
                                 .attachment_count = attach_count};
                char *msg = NULL;
                size_t msg_len = 0;
                if (mail_compose(&p, &msg, &msg_len) != 0) {
                    fprintf(stderr, "Error: Failed to build message.\n");
                } else {
                    result = mail_send_wire(cfg, from, to, msg, msg_len);
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
        send_done: ;

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
                    fprintf(stderr, "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n", uid_str);
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
                    fprintf(stderr, "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n", uid_str);
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
                fprintf(stderr, "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n", uid_str);
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
            char q[512];
            snprintf(q, sizeof(q), "Delete the label '%s'.", label_id);
            if (!cfg->gmail_mode || confirm_irreversible(q)) result = email_service_delete_label(cfg, label_id);
            else result = declined();
        }

    } else if (strcmp(cmd, "create-folder") == 0) {
        const char *name = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            name = argv[i]; break;
        }
        if (!name) {
            fprintf(stderr, "Error: 'create-folder' requires a folder name.\n");
            help_create_folder();
        } else {
            result = email_service_create_folder(cfg, name);
        }

    } else if (strcmp(cmd, "history") == 0) {
        const char *object = NULL;
        int limit = 50, ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (strcmp(argv[i], "--account") == 0) { i++; continue; }
            if (i == account_arg_idx) continue;
            if (strcmp(argv[i], "--limit") == 0) {
                if (i + 1 >= argc || atoi(argv[i + 1]) <= 0) {
                    fprintf(stderr, "Error: --limit requires a positive number.\n"); ok = 0;
                } else limit = atoi(argv[++i]);
            } else if (argv[i][0] != '-' && !object) {
                object = argv[i];
            } else {
                unknown_option("history", argv[i]); ok = 0;
            }
        }
        if (ok) {
            OpHistory h = {0};
            if (history_load(account_arg, NULL, &h) != 0) {
                fprintf(stderr, "Error: the history could not be read.\n");
            } else {
                result = print_history(&h, object, limit);
                op_history_free(&h);
            }
        }

    } else if (strcmp(cmd, "revert") == 0) {
        const char *id_str = NULL;
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (strcmp(argv[i], "--account") == 0) { i++; continue; }
            if (i == account_arg_idx) continue;
            if (argv[i][0] != '-' && !id_str) id_str = argv[i];
            else { unknown_option("revert", argv[i]); ok = 0; }
        }
        if (ok) {
            char *end = NULL;
            unsigned long id = id_str ? strtoul(id_str, &end, 10) : 0;
            if (!id_str || *end != '\0' || id == 0) {
                fprintf(stderr, "Error: 'revert' requires a step number.\n");
                help_revert();
            } else {
                OpHistory h = {0};
                const OpStep *st = NULL;
                char q[600] = "";
                if (history_load(NULL, NULL, &h) == 0 && (st = op_history_step(&h, id))) {
                    char desc[512];
                    history_describe(st, desc, sizeof(desc));
                    snprintf(q, sizeof(q), "Revert step %lu: %s %s", id, st->object, desc);
                }
                if (!st) {
                    fprintf(stderr, "Error: no step %lu in the history.\n", id);
                    result = RESULT_REPORTED;
                } else if (!confirm_irreversible_or_revert(q)) {
                    result = declined();
                } else {
                    char refusal[512] = "";
                    if (history_revert(id, refusal, sizeof(refusal)) == 0) {
                        printf("Step %lu reverted.\n", id);
                        result = 0;
                    } else {
                        fprintf(stderr, "Refused: %s\n", refusal);
                        result = RESULT_REPORTED;
                    }
                }
                op_history_free(&h);
            }
        }

    } else if (strcmp(cmd, "delete-folder") == 0) {
        const char *name = NULL;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            name = argv[i]; break;
        }
        if (!name) {
            fprintf(stderr, "Error: 'delete-folder' requires a folder name.\n");
            help_delete_folder();
        } else {
            char q[512];
            snprintf(q, sizeof(q), "Delete the folder '%s' with every message in it.", name);
            if (cfg->gmail_mode || confirm_irreversible(q)) result = email_service_delete_folder(cfg, name);
            else result = declined();
        }

    } else if (strcmp(cmd, "move") == 0) {
        const char *uid_str = NULL;
        const char *target  = NULL;
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
            } else if (!target) {
                target = argv[i];
            } else {
                unknown_option(cmd, argv[i]); ok = 0;
            }
        }
        if (ok) {
            if (!uid_str || !target) {
                fprintf(stderr, "Error: 'move' requires a UID and a destination folder.\n");
                help_move();
            } else {
                char uid[17];
                if (parse_uid(uid_str, uid) != 0)
                    fprintf(stderr, "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n", uid_str);
                else
                    result = email_service_move(cfg, uid, folder, target);
            }
        }

    } else if (strcmp(cmd, "mark-junk") == 0 || strcmp(cmd, "mark-notjunk") == 0) {
        const char *uid_str = NULL;
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (!uid_str) { uid_str = argv[i]; }
            else { unknown_option(cmd, argv[i]); ok = 0; }
        }
        if (ok) {
            if (!uid_str) {
                fprintf(stderr, "Error: '%s' requires a UID argument.\n", cmd);
                if (strcmp(cmd, "mark-junk") == 0) help_mark_junk();
                else help_mark_notjunk();
            } else {
                char uid[17];
                if (parse_uid(uid_str, uid) != 0)
                    fprintf(stderr, "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n", uid_str);
                else if (strcmp(cmd, "mark-junk") == 0)
                    result = email_service_mark_junk(cfg, uid);
                else
                    result = email_service_mark_notjunk(cfg, uid);
            }
        }

    } else if (strcmp(cmd, "trash") == 0 || strcmp(cmd, "archive") == 0) {
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
                help_trash();
            } else {
                char uid[17];
                if (parse_uid(uid_str, uid) != 0)
                    fprintf(stderr, "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n", uid_str);
                else if (strcmp(cmd, "trash") == 0) {
                    char q[512];
                    snprintf(q, sizeof(q), "Delete message %s for good.", uid_str);
                    if (!email_service_trash_is_permanent(cfg, folder) ||
                        confirm_irreversible(q))
                        result = email_service_trash(cfg, uid, folder);
                    else
                        result = declined();
                }
                else
                    result = email_service_archive(cfg, uid, folder);
            }
        }

    } else if (strcmp(cmd, "reply") == 0) {
        const char *uid_str = NULL, *folder = NULL, *body = NULL;
        int all = 0, ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (strcmp(argv[i], "--all") == 0) {
                all = 1;
            } else if (strcmp(argv[i], "--folder") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --folder requires a folder name.\n"); ok = 0;
                } else folder = argv[++i];
            } else if (strcmp(argv[i], "--body") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --body requires text.\n"); ok = 0;
                } else body = argv[++i];
            } else if (!uid_str) {
                uid_str = argv[i];
            } else {
                unknown_option("reply", argv[i]); ok = 0;
            }
        }
        if (ok) {
            if (!uid_str) {
                fprintf(stderr, "Error: 'reply' requires a UID argument.\n");
                help_reply();
            } else {
                char uid[17];
                if (parse_uid(uid_str, uid) != 0)
                    fprintf(stderr, "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n", uid_str);
                else if (confirm_irreversible("Send the reply."))
                    result = email_service_reply(cfg, uid, folder, all, body);
                else
                    result = declined();
            }
        }

    } else if (strcmp(cmd, "forward") == 0) {
        const char *uid_str = NULL, *folder = NULL;
        const char *to = NULL, *cc = NULL, *note = NULL;
        int ok = 1;
        for (int i = cmd_idx + 1; i < argc && ok; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            const char *opt = argv[i];
            const char **dst = NULL;
            const char *what = NULL;
            if      (strcmp(opt, "--to")     == 0) { dst = &to;     what = "an address"; }
            else if (strcmp(opt, "--cc")     == 0) { dst = &cc;     what = "an address"; }
            else if (strcmp(opt, "--body")   == 0) { dst = &note;   what = "text"; }
            else if (strcmp(opt, "--folder") == 0) { dst = &folder; what = "a folder name"; }
            if (dst) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: %s requires %s.\n", opt, what); ok = 0;
                } else *dst = argv[++i];
            } else if (!uid_str && opt[0] != '-') {
                uid_str = opt;
            } else {
                unknown_option(cmd, opt); ok = 0;
            }
        }
        if (ok) {
            if (!uid_str) {
                fprintf(stderr, "Error: 'forward' requires a UID argument.\n");
                help_forward();
            } else if (!to || !to[0]) {
                fprintf(stderr, "Error: --to is required.\n");
                help_forward();
            } else {
                char uid[17];
                if (parse_uid(uid_str, uid) != 0)
                    fprintf(stderr, "Error: invalid UID '%s' (expected decimal integer or 16-char hex).\n", uid_str);
                else if (confirm_irreversible("Send the forwarded message."))
                    result = email_service_forward(cfg, uid, folder, to, cc, note);
                else
                    result = declined();
            }
        }

    } else if (strcmp(cmd, "list-accounts") == 0) {
        int count = 0;
        AccountEntry *accs = cmd_json ? NULL : config_list_accounts(&count);
        if (cmd_json) {
            result = email_service_list_accounts_json();
        } else if (count == 0) {
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
                email_service_check_credential(new_cfg);
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

            char q[512];
            snprintf(q, sizeof(q), "Remove the account '%s' (its settings and credentials).",
                     account_name);
            if (!found) {
                fprintf(stderr, "Error: Account '%s' not found.\n", account_name);
            } else if (!confirm_irreversible(q)) {
                result = declined();
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

    } else if (strcmp(cmd, "rules") == 0) {
        /* Determine subcommand: the first non-flag arg after "rules" */
        const char *subcmd = NULL;
        int subcmd_idx = -1;
        for (int i = cmd_idx + 1; i < argc; i++) {
            if (strcmp(argv[i], "--batch") == 0) continue;
            if (strcmp(argv[i], "--help")  == 0 || strcmp(argv[i], "-h") == 0) {
                help_rules_list();
                return EXIT_SUCCESS;
            }
            if (argv[i][0] != '-') { subcmd = argv[i]; subcmd_idx = i; break; }
        }

        if (!subcmd || strcmp(subcmd, "list") == 0) {
            /* rules list: show rules for the configured account(s) */
            for (int i = subcmd_idx + 1; i < argc; i++) {
                if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                    help_rules_list(); return EXIT_SUCCESS;
                }
            }
            if (cmd_json) {
                if (email_service_rules_json(account_arg) == 0) {
                    fprintf(stderr, "Error: Account '%s' not found.\n",
                            account_arg ? account_arg : "");
                    result = -1;
                } else {
                    result = 0;
                }
            } else {
            int acc_count = 0;
            AccountEntry *accounts = config_list_accounts(&acc_count);

            int printed = 0;
            for (int i = 0; i < acc_count; i++) {
                if (account_arg && account_arg[0] &&
                    strcmp(accounts[i].name, account_arg) != 0)
                    continue;

                MailRules *rules = mail_rules_load(accounts[i].name);

                if (!rules || rules->count == 0) {
                    printf("No rules configured for %s.\n", accounts[i].name);
                    printf("  (Create ~/.config/email-cli/accounts/%s/rules.ini"
                           " or use 'email-import-rules'.)\n", accounts[i].name);
                    mail_rules_free(rules);
                    printed++;
                    continue;
                }

                printf("Rules for %s (%d rule%s):\n\n",
                       accounts[i].name, rules->count,
                       rules->count == 1 ? "" : "s");

                for (int r = 0; r < rules->count; r++) {
                    const MailRule *rule = &rules->rules[r];
                    printf("  %d. %s\n", r + 1,
                           rule->name && rule->name[0] ? rule->name : "(unnamed)");

                    /* mail_rules_load() builds `when` from the flat if-* fields,
                     * so a rule with any condition has one. */
                    if (rule->when && rule->when[0])
                        printf("       when         = %s\n", rule->when);

                    for (int j = 0; j < rule->then_add_count; j++)
                        printf("       then-add-label    = %s\n", rule->then_add_label[j]);
                    for (int j = 0; j < rule->then_rm_count; j++)
                        printf("       then-remove-label = %s\n", rule->then_rm_label[j]);
                    if (rule->then_move_folder)
                        printf("       then-move-folder  = %s\n", rule->then_move_folder);
                    printf("\n");
                }

                mail_rules_free(rules);
                printed++;
            }

            config_free_account_list(accounts, acc_count);

            if (!printed) {
                fprintf(stderr, "Error: Account '%s' not found.\n",
                        account_arg ? account_arg : "");
                result = -1;
            } else {
                result = 0;
            }
            }

        } else if (strcmp(subcmd, "apply") == 0) {
            /* rules apply [--dry-run] [--verbose] */
            int dry_run = 0, verbose = 0;
            for (int i = subcmd_idx + 1; i < argc; i++) {
                if (strcmp(argv[i], "--dry-run")  == 0) { dry_run = 1; continue; }
                if (strcmp(argv[i], "--verbose")  == 0 ||
                    strcmp(argv[i], "-v")          == 0) { verbose = 1; continue; }
                if (strcmp(argv[i], "--help")     == 0 ||
                    strcmp(argv[i], "-h")          == 0) { help_rules_apply(); return EXIT_SUCCESS; }
                if (strcmp(argv[i], "--batch")    == 0) continue;
                /* Skip account arg which is already parsed */
                if (strcmp(argv[i], "--account")  == 0) { i++; continue; }
            }
            int rc = email_service_apply_rules(account_arg, dry_run, verbose);
            result = rc >= 0 ? 0 : -1;

        } else if (strcmp(subcmd, "add") == 0) {
            /* rules add --name <name> [conditions] [actions] */
            const char *rule_name  = NULL;
            const char *if_from    = NULL;
            const char *if_subject = NULL;
            const char *if_to      = NULL;
            const char *if_label   = NULL;
            const char *move_folder = NULL;
            const char *add_labels[MAIL_RULE_MAX_LABELS];
            int   add_label_count = 0;
            const char *rm_labels[MAIL_RULE_MAX_LABELS];
            int   rm_label_count = 0;

            for (int i = subcmd_idx + 1; i < argc; i++) {
                if (strcmp(argv[i], "--help")    == 0 ||
                    strcmp(argv[i], "-h")         == 0) { help_rules_add(); return EXIT_SUCCESS; }
                if (strcmp(argv[i], "--batch")   == 0) continue;
                if (strcmp(argv[i], "--account") == 0) { i++; continue; }
                if (strcmp(argv[i], "--name")         == 0 && i+1 < argc) { rule_name  = argv[++i]; continue; }
                if (strcmp(argv[i], "--if-from")      == 0 && i+1 < argc) { if_from    = argv[++i]; continue; }
                if (strcmp(argv[i], "--if-subject")   == 0 && i+1 < argc) { if_subject = argv[++i]; continue; }
                if (strcmp(argv[i], "--if-to")        == 0 && i+1 < argc) { if_to      = argv[++i]; continue; }
                if (strcmp(argv[i], "--if-label")     == 0 && i+1 < argc) { if_label   = argv[++i]; continue; }
                if (strcmp(argv[i], "--move-folder")  == 0 && i+1 < argc) { move_folder = argv[++i]; continue; }
                if (strcmp(argv[i], "--add-label")    == 0 && i+1 < argc) {
                    if (add_label_count < MAIL_RULE_MAX_LABELS)
                        add_labels[add_label_count++] = argv[++i];
                    else i++;
                    continue;
                }
                if (strcmp(argv[i], "--remove-label") == 0 && i+1 < argc) {
                    if (rm_label_count < MAIL_RULE_MAX_LABELS)
                        rm_labels[rm_label_count++] = argv[++i];
                    else i++;
                    continue;
                }
                fprintf(stderr, "Unknown option '%s' for 'rules add'.\n", argv[i]);
                fprintf(stderr, "Run 'email-cli help rules add' for usage.\n");
                result = -1;
                goto rules_done;
            }

            if (!rule_name || !rule_name[0]) {
                fprintf(stderr, "Error: --name is required for 'rules add'.\n");
                fprintf(stderr, "Run 'email-cli help rules add' for usage.\n");
                result = -1;
                goto rules_done;
            }

            /* Resolve target account name into a local copy */
            char target_acct_buf[256] = {0};
            if (account_arg && account_arg[0]) {
                strncpy(target_acct_buf, account_arg, sizeof(target_acct_buf) - 1);
            } else {
                int acc_count = 0;
                AccountEntry *accs = config_list_accounts(&acc_count);
                if (acc_count == 1) {
                    strncpy(target_acct_buf, accs[0].name, sizeof(target_acct_buf) - 1);
                } else if (acc_count > 1) {
                    fprintf(stderr, "Multiple accounts configured. Use --account.\n");
                    config_free_account_list(accs, acc_count);
                    result = -1;
                    goto rules_done;
                } else {
                    fprintf(stderr, "No accounts configured.\n");
                    config_free_account_list(accs, acc_count);
                    result = -1;
                    goto rules_done;
                }
                config_free_account_list(accs, acc_count);
            }

            {
                MailRules *rules = mail_rules_load(target_acct_buf);
                if (!rules) {
                    rules = calloc(1, sizeof(MailRules));
                    if (!rules) {
                        fprintf(stderr, "Error: out of memory.\n");
                        result = -1;
                        goto rules_done;
                    }
                }
                /* Check for duplicate name */
                int dup = 0;
                for (int r = 0; r < rules->count; r++) {
                    if (rules->rules[r].name &&
                        strcmp(rules->rules[r].name, rule_name) == 0) { dup = 1; break; }
                }
                if (dup) {
                    fprintf(stderr, "Error: A rule named \"%s\" already exists.\n", rule_name);
                    mail_rules_free(rules);
                    result = -1;
                    goto rules_done;
                }
                /* Grow array if needed */
                if (rules->count >= rules->cap) {
                    int nc = rules->cap ? rules->cap * 2 : 8;
                    MailRule *tmp = realloc(rules->rules, (size_t)nc * sizeof(MailRule));
                    if (!tmp) {
                        fprintf(stderr, "Error: out of memory.\n");
                        mail_rules_free(rules);
                        result = -1;
                        goto rules_done;
                    }
                    rules->rules = tmp;
                    rules->cap   = nc;
                }
                MailRule *nr = &rules->rules[rules->count++];
                memset(nr, 0, sizeof(*nr));
                nr->name       = strdup(rule_name);
                nr->if_from    = if_from    ? strdup(if_from)    : NULL;
                nr->if_subject = if_subject ? strdup(if_subject) : NULL;
                nr->if_to      = if_to      ? strdup(if_to)      : NULL;
                nr->if_label   = if_label   ? strdup(if_label)   : NULL;
                nr->when = when_from_flat(if_from, if_subject, if_to, if_label,
                                          NULL, NULL, NULL, NULL, 0, 0);
                nr->then_move_folder = move_folder ? strdup(move_folder) : NULL;
                for (int j = 0; j < add_label_count; j++)
                    nr->then_add_label[nr->then_add_count++] = strdup(add_labels[j]);
                for (int j = 0; j < rm_label_count; j++)
                    nr->then_rm_label[nr->then_rm_count++] = strdup(rm_labels[j]);

                if (mail_rules_save(target_acct_buf, rules) == 0) {
                    printf("Rule added: \"%s\"\n", rule_name);
                    result = 0;
                } else {
                    fprintf(stderr, "Error: failed to save rules.\n");
                    result = -1;
                }
                mail_rules_free(rules);
            }

        } else if (strcmp(subcmd, "remove") == 0) {
            /* rules remove --name <name> */
            const char *rule_name = NULL;
            for (int i = subcmd_idx + 1; i < argc; i++) {
                if (strcmp(argv[i], "--help")    == 0 ||
                    strcmp(argv[i], "-h")         == 0) { help_rules_remove(); return EXIT_SUCCESS; }
                if (strcmp(argv[i], "--batch")   == 0) continue;
                if (strcmp(argv[i], "--account") == 0) { i++; continue; }
                if (strcmp(argv[i], "--name")    == 0 && i+1 < argc) { rule_name = argv[++i]; continue; }
                fprintf(stderr, "Unknown option '%s' for 'rules remove'.\n", argv[i]);
                fprintf(stderr, "Run 'email-cli help rules remove' for usage.\n");
                result = -1;
                goto rules_done;
            }

            if (!rule_name || !rule_name[0]) {
                fprintf(stderr, "Error: --name is required for 'rules remove'.\n");
                fprintf(stderr, "Run 'email-cli help rules remove' for usage.\n");
                result = -1;
                goto rules_done;
            }

            /* Resolve target account name into a local copy */
            char rem_acct_buf[256] = {0};
            if (account_arg && account_arg[0]) {
                strncpy(rem_acct_buf, account_arg, sizeof(rem_acct_buf) - 1);
            } else {
                int acc_count = 0;
                AccountEntry *accs = config_list_accounts(&acc_count);
                if (acc_count == 1) {
                    strncpy(rem_acct_buf, accs[0].name, sizeof(rem_acct_buf) - 1);
                } else if (acc_count > 1) {
                    fprintf(stderr, "Multiple accounts configured. Use --account.\n");
                    config_free_account_list(accs, acc_count);
                    result = -1;
                    goto rules_done;
                } else {
                    fprintf(stderr, "No accounts configured.\n");
                    config_free_account_list(accs, acc_count);
                    result = -1;
                    goto rules_done;
                }
                config_free_account_list(accs, acc_count);
            }

            {
                MailRules *rules = mail_rules_load(rem_acct_buf);
                if (!rules || rules->count == 0) {
                    fprintf(stderr, "No rules found for %s.\n", rem_acct_buf);
                    mail_rules_free(rules);
                    result = -1;
                    goto rules_done;
                }
                int found = -1;
                for (int r = 0; r < rules->count; r++) {
                    if (rules->rules[r].name &&
                        strcmp(rules->rules[r].name, rule_name) == 0) {
                        found = r; break;
                    }
                }
                if (found < 0) {
                    fprintf(stderr, "Error: No rule named \"%s\" found.\n", rule_name);
                    mail_rules_free(rules);
                    result = -1;
                    goto rules_done;
                }
                {
                    MailRule *r = &rules->rules[found];
                    free(r->name); free(r->when);
                    free(r->if_from); free(r->if_not_from);
                    free(r->if_subject); free(r->if_not_subject);
                    free(r->if_to); free(r->if_not_to);
                    free(r->if_label); free(r->if_body);
                    free(r->then_move_folder); free(r->then_forward_to);
                    for (int j = 0; j < r->then_add_count; j++) free(r->then_add_label[j]);
                    for (int j = 0; j < r->then_rm_count;  j++) free(r->then_rm_label[j]);
                }
                for (int r = found; r < rules->count - 1; r++)
                    rules->rules[r] = rules->rules[r + 1];
                rules->count--;
                if (mail_rules_save(rem_acct_buf, rules) == 0) {
                    printf("Rule removed: \"%s\"\n", rule_name);
                    result = 0;
                } else {
                    fprintf(stderr, "Error: failed to save rules.\n");
                    result = -1;
                }
                mail_rules_free(rules);
            }

        } else if (strcmp(subcmd, "edit") == 0) {
            /* rules edit --name <name> [--new-name N] [conditions] [actions] */
            const char *rule_name = NULL, *new_name = NULL;
            const char *if_from = NULL, *if_subject = NULL, *if_to = NULL, *if_label = NULL;
            const char *move_folder = NULL;
            const char *add_labels[MAIL_RULE_MAX_LABELS];
            const char *rm_labels[MAIL_RULE_MAX_LABELS];
            int add_label_count = 0, rm_label_count = 0;
            int set_cond = 0, set_add = 0, set_rm = 0, set_folder = 0;

            for (int i = subcmd_idx + 1; i < argc; i++) {
                if (strcmp(argv[i], "--help")    == 0 ||
                    strcmp(argv[i], "-h")         == 0) { help_rules_edit(); return EXIT_SUCCESS; }
                if (strcmp(argv[i], "--batch")   == 0) continue;
                if (strcmp(argv[i], "--account") == 0) { i++; continue; }
                if (strcmp(argv[i], "--name")         == 0 && i+1 < argc) { rule_name  = argv[++i]; continue; }
                if (strcmp(argv[i], "--new-name")     == 0 && i+1 < argc) { new_name   = argv[++i]; continue; }
                if (strcmp(argv[i], "--if-from")      == 0 && i+1 < argc) { if_from    = argv[++i]; set_cond = 1; continue; }
                if (strcmp(argv[i], "--if-subject")   == 0 && i+1 < argc) { if_subject = argv[++i]; set_cond = 1; continue; }
                if (strcmp(argv[i], "--if-to")        == 0 && i+1 < argc) { if_to      = argv[++i]; set_cond = 1; continue; }
                if (strcmp(argv[i], "--if-label")     == 0 && i+1 < argc) { if_label   = argv[++i]; set_cond = 1; continue; }
                if (strcmp(argv[i], "--move-folder")  == 0 && i+1 < argc) { move_folder = argv[++i]; set_folder = 1; continue; }
                if (strcmp(argv[i], "--add-label")    == 0 && i+1 < argc) {
                    set_add = 1;
                    if (add_label_count < MAIL_RULE_MAX_LABELS) add_labels[add_label_count++] = argv[++i];
                    else i++;
                    continue;
                }
                if (strcmp(argv[i], "--remove-label") == 0 && i+1 < argc) {
                    set_rm = 1;
                    if (rm_label_count < MAIL_RULE_MAX_LABELS) rm_labels[rm_label_count++] = argv[++i];
                    else i++;
                    continue;
                }
                fprintf(stderr, "Unknown option '%s' for 'rules edit'.\n", argv[i]);
                fprintf(stderr, "Run 'email-cli help rules edit' for usage.\n");
                result = -1;
                goto rules_done;
            }

            if (!rule_name || !rule_name[0]) {
                fprintf(stderr, "Error: --name is required for 'rules edit'.\n");
                fprintf(stderr, "Run 'email-cli help rules edit' for usage.\n");
                result = -1;
                goto rules_done;
            }
            if (!new_name && !set_cond && !set_add && !set_rm && !set_folder) {
                fprintf(stderr, "Error: nothing to change for 'rules edit'.\n");
                fprintf(stderr, "Run 'email-cli help rules edit' for usage.\n");
                result = -1;
                goto rules_done;
            }
            if (new_name && !new_name[0]) {
                fprintf(stderr, "Error: --new-name must not be empty.\n");
                result = -1;
                goto rules_done;
            }

            char edit_acct_buf[256] = {0};
            if (account_arg && account_arg[0]) {
                strncpy(edit_acct_buf, account_arg, sizeof(edit_acct_buf) - 1);
            } else {
                int acc_count = 0;
                AccountEntry *accs = config_list_accounts(&acc_count);
                if (acc_count == 1) {
                    strncpy(edit_acct_buf, accs[0].name, sizeof(edit_acct_buf) - 1);
                } else if (acc_count > 1) {
                    fprintf(stderr, "Multiple accounts configured. Use --account.\n");
                    config_free_account_list(accs, acc_count);
                    result = -1;
                    goto rules_done;
                } else {
                    fprintf(stderr, "No accounts configured.\n");
                    config_free_account_list(accs, acc_count);
                    result = -1;
                    goto rules_done;
                }
                config_free_account_list(accs, acc_count);
            }

            {
                MailRules *rules = mail_rules_load(edit_acct_buf);
                if (!rules || rules->count == 0) {
                    fprintf(stderr, "No rules found for %s.\n", edit_acct_buf);
                    mail_rules_free(rules);
                    result = -1;
                    goto rules_done;
                }
                int found = -1;
                for (int r = 0; r < rules->count; r++) {
                    if (rules->rules[r].name &&
                        strcmp(rules->rules[r].name, rule_name) == 0) { found = r; break; }
                }
                if (found < 0) {
                    fprintf(stderr, "Error: No rule named \"%s\" found.\n", rule_name);
                    mail_rules_free(rules);
                    result = -1;
                    goto rules_done;
                }
                if (new_name && strcmp(new_name, rule_name) != 0) {
                    for (int r = 0; r < rules->count; r++) {
                        if (rules->rules[r].name &&
                            strcmp(rules->rules[r].name, new_name) == 0) {
                            fprintf(stderr, "Error: A rule named \"%s\" already exists.\n", new_name);
                            mail_rules_free(rules);
                            result = -1;
                            goto rules_done;
                        }
                    }
                }
                MailRule *er = &rules->rules[found];
                if (new_name) { free(er->name); er->name = strdup(new_name); }
                if (set_cond) {
                    /* The given conditions replace the whole condition. */
                    free(er->when);
                    free(er->if_from);        free(er->if_subject);
                    free(er->if_to);          free(er->if_label);
                    free(er->if_not_from);    free(er->if_not_subject);
                    free(er->if_not_to);      free(er->if_body);
                    er->if_not_from = er->if_not_subject = er->if_not_to = er->if_body = NULL;
                    er->if_age_gt = er->if_age_lt = 0;
                    er->if_from    = if_from    ? strdup(if_from)    : NULL;
                    er->if_subject = if_subject ? strdup(if_subject) : NULL;
                    er->if_to      = if_to      ? strdup(if_to)      : NULL;
                    er->if_label   = if_label   ? strdup(if_label)   : NULL;
                    er->when = when_from_flat(if_from, if_subject, if_to, if_label,
                                              NULL, NULL, NULL, NULL, 0, 0);
                }
                if (set_add) {
                    for (int j = 0; j < er->then_add_count; j++) { free(er->then_add_label[j]); er->then_add_label[j] = NULL; }
                    er->then_add_count = 0;
                    for (int j = 0; j < add_label_count; j++)
                        er->then_add_label[er->then_add_count++] = strdup(add_labels[j]);
                }
                if (set_rm) {
                    for (int j = 0; j < er->then_rm_count; j++) { free(er->then_rm_label[j]); er->then_rm_label[j] = NULL; }
                    er->then_rm_count = 0;
                    for (int j = 0; j < rm_label_count; j++)
                        er->then_rm_label[er->then_rm_count++] = strdup(rm_labels[j]);
                }
                if (set_folder) {
                    free(er->then_move_folder);
                    er->then_move_folder = move_folder[0] ? strdup(move_folder) : NULL;
                }
                if (mail_rules_save(edit_acct_buf, rules) == 0) {
                    printf("Rule updated: \"%s\"\n", new_name ? new_name : rule_name);
                    result = 0;
                } else {
                    fprintf(stderr, "Error: failed to save rules.\n");
                    result = -1;
                }
                mail_rules_free(rules);
            }

        } else {
            fprintf(stderr, "Unknown 'rules' subcommand '%s'.\n", subcmd);
            fprintf(stderr, "Available: rules list, rules apply, rules add, rules edit, rules remove\n");
            result = -1;
        }
        rules_done: ;

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
    if (result != RESULT_REPORTED)
        fprintf(stderr, "\nFailed. Check logs in %s\n", log_file);
    return EXIT_FAILURE;
}
