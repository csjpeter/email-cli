#ifndef EMAIL_SERVICE_H
#define EMAIL_SERVICE_H

#include "config.h"

/**
 * @file email_service.h
 * @brief High-level service for listing and reading email messages.
 */

/**
 * @brief Options for email_service_list().
 *
 * all    0 = show UNSEEN messages only (default)
 *        1 = show ALL messages; unread ones are marked in the table
 * folder NULL = use cfg->folder; non-NULL overrides for this call only
 * limit  Maximum number of rows to display per page (0 = no limit)
 * offset 1-based index of the first message to show (0 or 1 = start)
 * pager  1 = interactive pager (less-like); 0 = one-shot with hint line
 */
typedef struct {
    int         all;
    const char *folder;
    int         limit;
    int         offset;
    int         pager;
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
 *         -1 = error.
 */
int email_service_list(const Config *cfg, const EmailListOpts *opts);

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
 * Backspace navigates up one level; at root it is a no-op (use ESC to quit).
 *
 * @param cfg            Connection configuration.
 * @param current_folder Folder active before opening the browser; used to
 *                       pre-position the flat-mode view. May be NULL.
 * @return Heap-allocated selected folder name, or NULL if the user quit.
 *         Caller must free() the returned string.
 */
char *email_service_list_folders_interactive(const Config *cfg,
                                             const char *current_folder);

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
int email_service_read(const Config *cfg, int uid, int pager, int page_size);

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
int email_service_sync(const Config *cfg);

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
 * @brief Lists all attachments in a message identified by IMAP UID.
 *
 * Loads the message from local cache or fetches it from the server.
 * Prints one line per attachment: filename and decoded size.
 *
 * @param cfg  Connection configuration.
 * @param uid  IMAP UID of the message.
 * @return 0 on success, -1 on failure.
 */
int email_service_list_attachments(const Config *cfg, int uid);

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
int email_service_save_attachment(const Config *cfg, int uid,
                                  const char *name, const char *outdir);

#endif /* EMAIL_SERVICE_H */
