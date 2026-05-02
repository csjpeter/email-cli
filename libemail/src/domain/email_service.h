#ifndef EMAIL_SERVICE_H
#define EMAIL_SERVICE_H

#include <stddef.h>
#include "config.h"

/**
 * @file email_service.h
 * @brief High-level service for listing and reading email messages.
 */

/**
 * @brief Options for email_service_list().
 *
 * all        0 = show UNSEEN messages only (default)
 *            1 = show ALL messages; unread ones are marked in the table
 * folder     NULL = use cfg->folder; non-NULL overrides for this call only
 * limit      Maximum number of rows to display per page (0 = no limit)
 * offset     1-based index of the first message to show (0 or 1 = start)
 * pager      1 = interactive pager (less-like); 0 = one-shot with hint line
 * action_uid    Output: UID of the message the user acted on (set when return
 *              value is 3/5/6 = reply/fwd/reply-all). Unused otherwise.
 * action_folder Output: physical IMAP folder of action_uid (may differ from
 *              the virtual/search folder the list was opened with).
 */
typedef struct {
    int         all;
    const char *folder;
    int         limit;
    int         offset;
    int         pager;
    char        action_uid[17];  /**< Output: UID for reply/fwd/reply-all */
    char        action_folder[512]; /**< Output: physical folder for action_uid */
} EmailListOpts;

/**
 * @brief Lists messages in a mailbox folder.
 *
 * In default mode (all=0) only UNSEEN messages are shown.
 * With all=1 all messages are shown; unread ones are prefixed with 'N'.
 * Unread messages always appear before read ones.
 * Prints a table of Status, UID, From, Subject and Date to stdout.
 *
 * @param cfg   Connection configuration.
 * @param opts  Listing options.
 * @return  0 = user quit normally,
 *          1 = user pressed Backspace (go to folder list, pager mode only),
 *          2 = user pressed 'c' (compose new message),
 *          3 = user pressed 'r' (reply; opts->action_uid holds the target UID),
 *          4 = background sync finished; caller should re-list to show new mail,
 *         -1 = error.
 */
int email_service_list(const Config *cfg, EmailListOpts *opts);

/**
 * @brief Lists all available IMAP folders.
 *
 * Issues IMAP LIST "" "*" and prints the results.
 * With tree=1 the hierarchy is rendered using box-drawing characters.
 *
 * @param cfg   Connection configuration.
 * @param tree  0 = flat list, 1 = tree view.
 * @return 0 on success, -1 on failure.
 */
int email_service_list_folders(const Config *cfg, int tree);

/**
 * @brief Interactive folder browser (TUI).
 *
 * Displays a full-screen cursor-driven folder list with tree/flat toggle.
 * In flat mode the browser opens at the level containing @p current_folder
 * so the user lands back where they came from.
 * Backspace navigates up one level in flat mode; at root it signals "go up"
 * by setting @p *go_up (if non-NULL) to 1 and returning NULL.
 *
 * @param cfg            Connection configuration.
 * @param current_folder Folder active before opening the browser; used to
 *                       pre-position the flat-mode view. May be NULL.
 * @param go_up          Optional output flag.  Set to 1 when Backspace is
 *                       pressed at the root level (caller should navigate to
 *                       the accounts screen).  Set to 0 on ESC/select.
 *                       If NULL the old behaviour is preserved: Backspace at
 *                       root returns the current folder unchanged.
 * @return Heap-allocated selected folder name, or NULL if the user quit or
 *         pressed Backspace at root.  Caller must free() the returned string.
 */
char *email_service_list_folders_interactive(const Config *cfg,
                                             const char *current_folder,
                                             int *go_up);

/**
 * @brief Interactive label browser for Gmail accounts (TUI).
 *
 * Displays labels in three sections:
 *   1. System labels (INBOX, Starred, Unread, Sent, Drafts)
 *   2. User-defined labels (alphabetical)
 *   3. Special (Archive, Spam, Trash)
 *
 * Each label shows a message count from the local .idx files.
 * Navigation: arrows, Enter to select, Backspace to go up.
 *
 * @param cfg            Connection configuration (must have gmail_mode=1).
 * @param current_label  Label active before opening; used for pre-positioning.
 * @param go_up          Output: set to 1 when Backspace pressed (go to accounts).
 * @return Heap-allocated selected label ID string, or NULL on ESC/quit.
 *         Caller must free().
 */
char *email_service_list_labels_interactive(const Config *cfg,
                                            const char *current_label,
                                            int *go_up);

/**
 * @brief Interactive accounts list screen (TUI).
 *
 * Shows all configured accounts in a navigable cursor list.
 * This is the top-level TUI entry point, sitting above the message list
 * in the navigation hierarchy.
 *
 * Keys:
 *   ↑/↓    — move cursor
 *   Enter   — open selected account: returns 1, *cfg_out set
 *   n       — add new account: returns 3, *cfg_out NULL
 *   d       — delete selected account: re-displays list
 *   e       — edit SMTP for selected account: returns 2, *cfg_out set
 *   ESC / q / Backspace — quit: returns 0
 *
 * @param cfg_out      Output: heap-allocated selected account Config (ret 1 or 2).
 *                     Caller must config_free() it.  NULL on ret 0 or 3.
 * @param cursor_inout Optional in/out cursor position.  On entry the cursor is
 *                     placed at this index (clamped to valid range).  On any
 *                     return the current cursor index is written back so the
 *                     caller can restore it on the next call.  Pass NULL to
 *                     always start at position 0.
 * @return 0 = quit, 1 = open account, 2 = edit SMTP, 3 = add new account.
 */
int email_service_account_interactive(Config **cfg_out, int *cursor_inout,
                                      const char *flash_msg);

/**
 * @brief Reads and displays one message identified by its IMAP UID.
 *
 * Checks the local cache first; fetches from the server on a cache miss.
 * The full RFC 2822 message is MIME-parsed and the plain-text body printed.
 *
 * @param cfg        Connection configuration.
 * @param uid        IMAP UID of the message.
 * @param pager      1 = interactive pager for long bodies; 0 = print all at once.
 * @param page_size  Lines per page when pager=1.
 * @return 0 on success, -1 on failure.
 */
int email_service_read(const Config *cfg, const char *uid, int pager, int page_size);

/**
 * @brief Downloads all messages in all folders to the local cache.
 *
 * Iterates over every folder returned by LIST, fetches each message that is
 * not yet in the full-message cache (BODY.PEEK[]), and saves it locally.
 * Messages already cached are skipped.  Progress is printed to stdout.
 * Attachments are stored as part of the raw RFC 2822 data (not extracted).
 *
 * @param cfg  Connection configuration.
 * @return 0 on success, -1 on error.
 */
int email_service_sync(const Config *cfg, int force_reconcile);

/**
 * @brief Sync all configured accounts, or only the one matching only_account.
 *
 * Iterates config_list_accounts() in alphabetical order.  For each account
 * (or the single matching account when only_account is non-NULL) it calls
 * local_store_init() then email_service_sync().
 *
 * @param only_account    Email address to restrict sync to, or NULL for all.
 * @param force_reconcile Non-zero to force a full reconcile for Gmail accounts
 *                        (bypass the incremental fast path).
 * @return 0 if every account synced successfully, -1 if any failed or the
 *         requested account was not found.
 */
int email_service_sync_all(const char *only_account, int force_reconcile);

/**
 * @brief Rebuild label .idx files from cached .hdr files for Gmail accounts.
 *
 * Does NOT contact the Gmail API.  Reads every .hdr file in the local
 * message store and reconstructs the label indexes from scratch.
 *
 * If @p only_account is non-NULL, only that account is processed.
 * IMAP accounts are skipped (they do not use label indexes).
 *
 * @param only_account  Email address to process, or NULL for all accounts.
 * @return 0 on success, -1 on error or account not found.
 */
int email_service_rebuild_indexes(const char *only_account);

/**
 * @brief Set verbose mode for sync and apply-rules operations.
 *
 * When verbose is non-zero, per-rule firing details are printed to stdout
 * during email_service_sync() and email_service_apply_rules().
 *
 * @param verbose  Non-zero to enable verbose output, 0 to disable.
 */
void email_service_set_verbose(int verbose);

/**
 * @brief Apply mail sorting rules retroactively to all locally cached messages.
 *
 * Loads the rules.ini for each configured account, iterates all cached
 * .hdr files, and applies matching rules (label add/remove).
 * Does not re-download messages or contact the server.
 *
 * @param only_account  Email address to process, or NULL for all accounts.
 * @param dry_run       Non-zero to print changes without writing them.
 * @param verbose       Non-zero to print per-rule firing details.
 * @return Number of messages modified (or would be modified), or -1 on error.
 */
int email_service_apply_rules(const char *only_account, int dry_run, int verbose);

/**
 * @brief Rebuild the contact suggestion cache from all locally cached headers.
 *
 * Iterates every configured account (or a single one if @p only_account is
 * set), scans all cached .hdr files, extracts From / To / Cc addresses, and
 * writes a fresh contacts.tsv.  Does not contact the server.
 *
 * @param only_account  Email address to process, or NULL for all accounts.
 * @return 0 on success, -1 on error or account not found.
 */
int email_service_rebuild_contacts(const char *only_account);

/**
 * @brief Installs a user crontab entry to run 'email-cli sync' periodically.
 *
 * Uses cfg->sync_interval (minutes) to set the cron frequency.
 * Does nothing if an entry already exists; prints guidance to remove it first.
 *
 * @param cfg  Connection configuration.  cfg->sync_interval must be > 0;
 *             the caller is responsible for setting a default before calling.
 * @return 0 on success or already-exists, -1 on error.
 */
int email_service_cron_setup(const Config *cfg);

/**
 * @brief Removes the 'email-cli sync' crontab entry added by cron_setup.
 *
 * @return 0 on success or not-found, -1 on error.
 */
int email_service_cron_remove(void);

/**
 * @brief Prints the current status of the 'email-cli sync' crontab entry.
 *
 * Shows whether an entry exists and what it looks like.
 * @return 0 always.
 */
int email_service_cron_status(void);

/**
 * @brief Load (or fetch) the raw RFC 2822 message for a given UID.
 *
 * Checks the local cache first; fetches from the server on a cache miss
 * and caches the result.  Returns a heap-allocated string the caller must
 * free(), or NULL on error.
 *
 * @param cfg  Connection configuration.
 * @param uid  IMAP UID of the message.
 * @return Heap-allocated raw message, or NULL on failure.
 */
char *email_service_fetch_raw(const Config *cfg, const char *uid);

/**
 * @brief Saves a sent message to the Sent folder on the IMAP server.
 *
 * Connects to the IMAP server, appends @p msg to the folder configured in
 * cfg->sent_folder (default: "Sent"), and marks it as \\Seen.
 * A non-fatal warning is printed on failure — the message was already sent.
 *
 * @param cfg      Connection configuration.
 * @param msg      Raw RFC 2822 message bytes (as returned by compose_build_message).
 * @param msg_len  Length of @p msg in bytes.
 * @return 0 on success, -1 on error.
 */
int email_service_save_sent(const Config *cfg, const char *msg, size_t msg_len);

/**
 * @brief Save a message to the local Drafts folder and queue for IMAP APPEND.
 *
 * Saves the message locally (no IMAP connection required) and records it in
 * pending_appends.tsv so the next sync uploads it to the server's Drafts folder.
 *
 * @param cfg      Account configuration.
 * @param msg      Raw RFC 2822 message bytes.
 * @param msg_len  Length in bytes.
 * @return 0 on success, -1 on error.
 */
int email_service_save_draft(const Config *cfg, const char *msg, size_t msg_len);

/**
 * @brief Set or clear a flag on a message (batch mode — synchronous push).
 *
 * Updates local manifest, Gmail label indexes (gmail_mode), pending queue,
 * then synchronously connects and pushes to server.
 * If server connection fails the pending queue ensures retry on next sync.
 *
 * @param cfg        Account configuration.
 * @param uid        16-char padded UID string.
 * @param folder     Folder/label the message is in (NULL = cfg->folder).
 * @param flag_bit   MSG_FLAG_UNSEEN, MSG_FLAG_FLAGGED, or MSG_FLAG_DONE.
 * @param add        1 = set the flag, 0 = clear it.
 * @return 0 on success, -1 on error.
 */
int email_service_set_flag(const Config *cfg, const char *uid,
                           const char *folder, int flag_bit, int add);

/**
 * @brief Add or remove a label/folder on a message (Gmail) or copy/move (IMAP).
 *
 * For Gmail: calls mail_client_modify_label.
 * For IMAP: not supported (returns -1 with message).
 *
 * @param cfg    Account configuration.
 * @param uid    16-char padded UID string.
 * @param label  Label ID (Gmail) or folder name (IMAP).
 * @param add    1 = add label, 0 = remove label.
 * @return 0 on success, -1 on error.
 */
int email_service_set_label(const Config *cfg, const char *uid,
                            const char *label, int add);

/**
 * @brief Print all available labels/folders to stdout.
 * Gmail: lists all labels with their IDs.
 * IMAP: lists all folders.
 * @return 0 on success, -1 on error.
 */
int email_service_list_labels(const Config *cfg);

/**
 * @brief Create a new Gmail label. Fails on IMAP accounts.
 * @param name  Display name for the new label.
 * @return 0 on success, -1 on error.
 */
int email_service_create_label(const Config *cfg, const char *name);

/**
 * @brief Delete a Gmail label. Fails on IMAP accounts.
 * @param label_id  Label ID (from list-labels).
 * @return 0 on success, -1 on error.
 */
int email_service_delete_label(const Config *cfg, const char *label_id);

/**
 * @brief Mark a message as junk/spam ($Junk / Gmail SPAM).
 * @return 0 on success, -1 on error.
 */
int email_service_mark_junk(const Config *cfg, const char *uid);

/**
 * @brief Mark a message as not-junk/ham ($NotJunk / remove Gmail SPAM).
 * @return 0 on success, -1 on error.
 */
int email_service_mark_notjunk(const Config *cfg, const char *uid);

/**
 * @brief Create a new IMAP folder. Fails on Gmail accounts.
 * @param name  Folder path / name.
 * @return 0 on success, -1 on error.
 */
int email_service_create_folder(const Config *cfg, const char *name);

/**
 * @brief Delete an IMAP folder. Fails on Gmail accounts.
 * @param name  Folder path / name.
 * @return 0 on success, -1 on error.
 */
int email_service_delete_folder(const Config *cfg, const char *name);

/**
 * @brief Lists all attachments in a message identified by IMAP UID.
 *
 * Loads the message from local cache or fetches it from the server.
 * Prints one line per attachment: filename and decoded size.
 *
 * @param cfg  Connection configuration.
 * @param uid  IMAP UID of the message.
 * @return 0 on success, -1 on failure.
 */
int email_service_list_attachments(const Config *cfg, const char *uid);

/**
 * @brief Saves one named attachment from a message to a directory.
 *
 * Loads the message, finds the attachment by filename, and writes the
 * decoded content to @p outdir/@p name.
 *
 * @param cfg     Connection configuration.
 * @param uid     IMAP UID of the message.
 * @param name    Exact filename of the attachment to save.
 * @param outdir  Destination directory path, or NULL to use ~/Downloads
 *                (falling back to ~).
 * @return 0 on success, -1 on failure.
 */
int email_service_save_attachment(const Config *cfg, const char *uid,
                                  const char *name, const char *outdir);

#endif /* EMAIL_SERVICE_H */
