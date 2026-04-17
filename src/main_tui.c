/**
 * @file main_tui.c
 * @brief Entry point for email-tui — interactive full-screen TUI email client.
 *
 * email-tui is the interactive client binary.  It presents a full-screen
 * cursor-driven account/folder/message navigator and supports composing,
 * replying to, and sending messages directly from the TUI.
 *
 * It does NOT support subcommands or batch/scriptable mode.
 * For scripting use email-cli; for syncing use email-sync.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include "config_store.h"
#include "setup_wizard.h"
#include "email_service.h"
#include "platform/terminal.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include "local_store.h"
#include "fs_util.h"
#include "smtp_adapter.h"
#include "compose_service.h"
#include "mime_util.h"
#include "html_render.h"
#include "mail_client.h"

/* Number of non-data lines printed around the message table */
#define LIST_HEADER_LINES 6

static int detect_page_size(void) {
    int rows = terminal_rows();
    if (rows > LIST_HEADER_LINES + 2)
        return rows - LIST_HEADER_LINES;
    return 20; /* safe fallback */
}

/* ── Help ────────────────────────────────────────────────────────────── */

static void help(void) {
    printf(
        "Usage: email-tui\n"
        "\n"
        "Full-screen interactive email client.\n"
        "\n"
        "  Starts with an account selector.  Use arrow keys to navigate,\n"
        "  Enter to open an account, Backspace to go back, ESC/q to quit.\n"
        "\n"
        "  Inside a message list:\n"
        "    c         Compose a new message\n"
        "    r         Reply to the selected message\n"
        "    Backspace Open the folder browser\n"
        "    ESC/q     Quit\n"
        "\n"
        "Options:\n"
        "  --help, -h   Show this help message\n"
        "\n"
        "For batch/scriptable operations use email-cli.\n"
        "For background sync use email-sync or 'email-cli cron setup'.\n"
    );
}

/* ── Compose / send helpers ──────────────────────────────────────────── */

/**
 * @brief Ensure SMTP is explicitly configured.
 *
 * If smtp_host is NULL (not configured — auto-derived from IMAP host at send
 * time, which may not work), the user is offered an inline SMTP configuration
 * wizard.  The updated config is saved automatically if the user confirms.
 * Never blocks the compose attempt: always returns 0.
 */
static void ensure_smtp_configured(Config *cfg) {
    if (cfg->gmail_mode) return; /* Gmail uses REST API, not SMTP */
    if (cfg->smtp_host) return; /* Already explicitly configured */

    printf("\n"
           "  SMTP (outgoing mail) is not explicitly configured.\n"
           "  Without it the system will guess settings from your IMAP host,\n"
           "  which often does not work (e.g. Gmail, Yahoo, Outlook).\n\n"
           "  Configure SMTP now? [Y/n] ");
    fflush(stdout);

    char ans[8] = "";
    if (!fgets(ans, sizeof(ans), stdin) || ans[0] == 'n' || ans[0] == 'N') {
        printf("  Skipping — send may fail if settings cannot be derived.\n\n");
        return;
    }

    if (setup_wizard_smtp(cfg) == 0) {
        if (config_save_to_store(cfg) == 0)
            printf("  SMTP settings saved.\n\n");
        else
            fprintf(stderr, "  Warning: SMTP configured but could not save to disk.\n\n");
    }
}

/**
 * @brief Build from address from config (SMTP user or IMAP user).
 * Returns a pointer into cfg — do NOT free.
 */
static const char *from_address(const Config *cfg) {
    return cfg->smtp_user ? cfg->smtp_user : cfg->user;
}

/**
 * @brief Interactive compose form — opens $EDITOR on a draft temp file.
 *
 * Writes a RFC 2822–style draft (editable headers + body) to a temp file,
 * launches $EDITOR (fallback: vim → vi), then reads the result back and sends.
 *
 * All header fields (From, To, Subject, In-Reply-To) are editable inside the
 * editor.  Abort if the user leaves To: empty or quits without saving.
 *
 * If @p prefill_body is non-NULL it is written into the draft (top-post
 * convention: user types above the quoted text).
 *
 * Returns 0 on successful send, -1 on abort or SMTP error.
 */
static int cmd_compose_interactive(Config *cfg,
                                   const char *prefill_to,
                                   const char *prefill_subject,
                                   const char *reply_to_msg_id,
                                   const char *prefill_body) {
    ensure_smtp_configured(cfg);

    /* 1. Create temp file */
    char tmppath[] = "/tmp/email-tui-XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) {
        perror("email-tui: mkstemp");
        return -1;
    }

    /* 2. Write draft (editable headers + body) */
    {
        FILE *f = fdopen(fd, "w");
        if (!f) { close(fd); unlink(tmppath); return -1; }
        const char *from = from_address(cfg);
        fprintf(f, "From: %s\n",    from ? from : "");
        fprintf(f, "To: %s\n",      prefill_to      ? prefill_to      : "");
        fprintf(f, "Subject: %s\n", prefill_subject ? prefill_subject : "");
        if (reply_to_msg_id && reply_to_msg_id[0])
            fprintf(f, "In-Reply-To: %s\n", reply_to_msg_id);
        fprintf(f, "\n");
        if (prefill_body && prefill_body[0])
            fprintf(f, "%s", prefill_body);
        fclose(f);
    }

    /* 3. Restore cursor, then launch $EDITOR */
    printf("\033[?25h");
    fflush(stdout);
    fflush(stderr);

    const char *editor = getenv("EDITOR");
    if (!editor || !editor[0]) editor = "vim";

    /* Build: EDITOR tmppath  (shell handles EDITOR with args, e.g. "nvim -u NONE") */
    {
        char cmd_buf[1024];
        snprintf(cmd_buf, sizeof(cmd_buf), "%s %s", editor, tmppath);
        int rc = system(cmd_buf);
        (void)rc; /* editor exit status not reliable across editors */
    }

    /* 4. Read back edited file */
    FILE *rf = fopen(tmppath, "r");
    unlink(tmppath);
    if (!rf) {
        fprintf(stderr, "Error: Could not read draft after editing.\n");
        return -1;
    }

    /* 5. Parse headers then body */
    char from_buf[512]   = "";
    char to_buf[512]     = "";
    char subj_buf[512]   = "";
    char msgid_buf[512]  = "";
    char *body = NULL;
    size_t body_len = 0;

    {
        char line[4096];
        int in_body = 0;
        while (fgets(line, sizeof(line), rf)) {
            /* strip trailing CRLF */
            size_t ll = strlen(line);
            while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
                line[--ll] = '\0';

            if (!in_body) {
                if (ll == 0) { in_body = 1; continue; } /* blank → body starts */
                if (strncasecmp(line, "From: ",       6)  == 0) { strncpy(from_buf,  line + 6,  sizeof(from_buf)  - 1); from_buf[sizeof(from_buf)-1]  = '\0'; }
                else if (strncasecmp(line, "To: ",    4)  == 0) { strncpy(to_buf,    line + 4,  sizeof(to_buf)    - 1); to_buf[sizeof(to_buf)-1]      = '\0'; }
                else if (strncasecmp(line, "Subject: ",9) == 0) { strncpy(subj_buf,  line + 9,  sizeof(subj_buf)  - 1); subj_buf[sizeof(subj_buf)-1]  = '\0'; }
                else if (strncasecmp(line, "In-Reply-To: ", 13) == 0) { strncpy(msgid_buf, line + 13, sizeof(msgid_buf) - 1); msgid_buf[sizeof(msgid_buf)-1] = '\0'; }
            } else {
                size_t add = ll + 1; /* line + \n */
                char *nb = realloc(body, body_len + add + 1);
                if (!nb) break;
                body = nb;
                memcpy(body + body_len, line, ll);
                body_len += ll;
                body[body_len++] = '\n';
                body[body_len]   = '\0';
            }
        }
        fclose(rf);
    }

    /* 6. Validate — abort if To: is empty */
    if (!to_buf[0]) {
        printf("  Aborted (To: is empty).\n");
        free(body);
        return -1;
    }

    /* 7. Build and send */
    {
        const char *from_send = from_buf[0] ? from_buf : from_address(cfg);
        const char *reply_id  = msgid_buf[0] ? msgid_buf : reply_to_msg_id;
        const char *body_str  = body ? body : "";
        ComposeParams p = {from_send, to_buf, subj_buf, body_str, reply_id};
        char *msg = NULL;
        size_t msg_len = 0;
        int rc = -1;
        if (compose_build_message(&p, &msg, &msg_len) != 0) {
            fprintf(stderr, "Error: Failed to build message.\n");
        } else {
            printf("  Sending...\n");
            fflush(stdout);
            if (cfg->gmail_mode) {
                /* Gmail: send via REST API; SENT label auto-added */
                RAII_MAIL MailClient *mc = mail_client_connect((Config *)cfg);
                rc = mc ? mail_client_append(mc, NULL, msg, msg_len) : -1;
                if (rc == 0)
                    printf("  Message sent.\n");
            } else {
                /* IMAP: send via SMTP, then save to Sent folder */
                rc = smtp_send(cfg, from_send, to_buf, msg, msg_len);
                if (rc == 0) {
                    printf("  Message sent.\n");
                    printf("  Saving to Sent folder...\n");
                    fflush(stdout);
                    if (email_service_save_sent(cfg, msg, msg_len) != 0)
                        fprintf(stderr, "  (Could not save to Sent folder — "
                                "check EMAIL_SENT_FOLDER in config.)\n");
                    else
                        printf("  Saved.\n");
                }
            }
            fflush(stdout);
            free(msg);
        }
        free(body);
        return rc;
    }
}

/**
 * @brief Reply to a message identified by UID in the given folder.
 * Loads the message, extracts reply metadata, opens compose form.
 */
static int cmd_reply(Config *cfg, const char *uid, const char *folder) {
    ensure_smtp_configured(cfg);

    /* Load raw message from the actual current folder */
    char *raw = NULL;
    if (local_msg_exists(folder, uid))
        raw = local_msg_load(folder, uid);
    else {
        /* Temporarily override cfg->folder so fetch uses the right mailbox */
        char *saved_folder = cfg->folder;
        cfg->folder = (char *)folder;
        raw = email_service_fetch_raw(cfg, uid);
        cfg->folder = saved_folder;
    }

    if (!raw) {
        fprintf(stderr, "Error: Could not load message UID %s.\n", uid);
        return -1;
    }

    char *reply_to = NULL, *subject = NULL, *msg_id = NULL;
    int rc = compose_extract_reply_meta(raw, &reply_to, &subject, &msg_id);
    if (rc != 0) {
        free(raw);
        fprintf(stderr, "Error: Could not parse message headers.\n");
        return -1;
    }

    /* Extract body text for quoting (try HTML first, then plain text) */
    char *body_text = NULL;
    {
        char *html_raw = mime_get_html_part(raw);
        if (html_raw) {
            body_text = html_render(html_raw, 72, 0);
            free(html_raw);
        }
        if (!body_text)
            body_text = mime_get_text_body(raw);
    }

    /* Build attribution + "> "-prefixed quoted block */
    char *quoted = NULL;
    {
        char *from_raw = mime_get_header(raw, "From");
        char *from_dec = from_raw ? mime_decode_words(from_raw) : NULL;
        free(from_raw);
        char *date_raw = mime_get_header(raw, "Date");
        char *date_fmt = date_raw ? mime_format_date(date_raw) : NULL;
        free(date_raw);

        /* "On <date>, <from> wrote:\n> line1\n> line2\n..." */
        size_t qcap = 4096 + (body_text ? strlen(body_text) * 2 : 0);
        quoted = malloc(qcap);
        if (quoted) {
            int off = snprintf(quoted, qcap, "On %s, %s wrote:\n",
                               date_fmt ? date_fmt : "?",
                               from_dec ? from_dec : "?");
            if (body_text) {
                const char *p = body_text;
                while (*p && off < (int)qcap - 4) {
                    const char *nl = strchr(p, '\n');
                    size_t len = nl ? (size_t)(nl - p) : strlen(p);
                    size_t advance = len + (nl ? 1 : 0);  /* bytes to skip incl. \n */
                    if (len > 0 && p[len - 1] == '\r') len--;  /* strip CR from output */
                    if (off + 2 + (int)len + 1 < (int)qcap) {
                        quoted[off++] = '>';
                        quoted[off++] = ' ';
                        memcpy(quoted + off, p, len);
                        off += (int)len;
                        quoted[off++] = '\n';
                        quoted[off]   = '\0';
                    }
                    p += advance;
                    if (!nl) break;
                }
            }
        }
        free(from_dec);
        free(date_fmt);
    }
    free(body_text);
    free(raw);

    rc = cmd_compose_interactive(cfg, reply_to, subject, msg_id, quoted);
    free(quoted);
    free(reply_to);
    free(subject);
    free(msg_id);
    return rc;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

#ifndef EMAIL_CLI_VERSION
#define EMAIL_CLI_VERSION "unknown"
#endif

int main(int argc, char *argv[]) {
    /* 0. Set locale so wcwidth() and mbsrtowcs() work correctly for
     *    multi-byte UTF-8 characters (needed for column-width calculations). */
    setlocale(LC_ALL, "");

    /* Handle --help / -h before anything else */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("email-tui %s\n", EMAIL_CLI_VERSION);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            help();
            return EXIT_SUCCESS;
        }
        /* Unknown option */
        if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option '%s'.\nRun 'email-tui --help' for usage.\n",
                    argv[i]);
            return EXIT_FAILURE;
        }
        /* Unexpected positional argument */
        fprintf(stderr, "email-tui does not accept arguments.\n"
                        "Run 'email-tui --help' for usage.\n");
        return EXIT_FAILURE;
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
        asprintf(&log_file, "%s/tui-session.log", log_dir)   == -1) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return EXIT_FAILURE;
    }

    /* 2. Initialize logger */
    if (fs_mkdir_p(log_dir, 0700) != 0)
        fprintf(stderr, "Warning: Could not create log directory %s\n", log_dir);
    if (logger_init(log_file, LOG_DEBUG) != 0)
        fprintf(stderr, "Warning: Logging system failed to initialize.\n");
    logger_log(LOG_INFO, "--- email-tui starting ---");

    /* 3. Load configuration */
    Config *cfg = config_load_from_store();
    if (!cfg) {
        logger_log(LOG_INFO, "No configuration found. Starting setup wizard.");
        cfg = setup_wizard_run();
        if (cfg) {
            if (config_save_to_store(cfg) != 0) {
                logger_log(LOG_ERROR, "Failed to save configuration.");
                fprintf(stderr, "Error: Failed to save configuration to disk.\n");
            } else {
                printf("Configuration saved. Run 'email-sync' to download your mail.\n");
            }
        } else {
            logger_log(LOG_ERROR, "Configuration aborted by user.");
            fprintf(stderr, "Configuration aborted. Exiting.\n");
            logger_close();
            return EXIT_FAILURE;
        }
    }

    /* 4. Initialize local store for the first/default account */
    if (local_store_init(cfg->host, cfg->user) != 0)
        logger_log(LOG_WARN, "Failed to initialize local store for %s", cfg->host);

    int page_size = detect_page_size();

    /* 5. Interactive TUI — navigation hierarchy:
     *   Accounts screen  (top, skipped on warm start if last_account is set)
     *     └─ Folder browser  (Enter on account; also Backspace from message list)
     *           └─ Message list  (Enter on folder)
     *   Backspace at folder-root → back to Accounts
     *   ESC anywhere → quit
     */
    int result = 0;
    int account_cursor = 0;  /* persists across re-entries to accounts screen */

    /* Restore account cursor position from last session. */
    {
        char *last = ui_pref_get_str("last_account");
        if (last) {
            int acount = 0;
            AccountEntry *alist = config_list_accounts(&acount);
            if (alist) {
                for (int i = 0; i < acount; i++) {
                    if (alist[i].cfg && alist[i].cfg->user &&
                        strcmp(alist[i].cfg->user, last) == 0) {
                        account_cursor = i;
                        break;
                    }
                }
                config_free_account_list(alist, acount);
            }
            free(last);
        }
    }

    for (;;) {  /* outer: accounts screen */
        Config *sel_cfg = NULL;

        {
            int acc = email_service_account_interactive(&sel_cfg, &account_cursor);
            if (acc == 0) break;  /* ESC/quit */
            if (acc == 3) {
                /* 'n' → add new account via wizard */
                Config *new_cfg = setup_wizard_run();
                if (new_cfg) {
                    if (config_save_account(new_cfg) != 0)
                        fprintf(stderr, "Warning: Failed to save new account.\n");
                    config_free(new_cfg);
                }
                continue;  /* re-display accounts screen */
            }
            if (!sel_cfg) continue;
            if (acc == 4) {
                /* 'i' → edit IMAP settings for selected account */
                if (setup_wizard_imap(sel_cfg) == 0)
                    config_save_account(sel_cfg);
                config_free(sel_cfg);
                continue;  /* re-display accounts screen with updated info */
            }
            if (acc == 2) {
                /* 'e' → edit SMTP settings for selected account */
                if (setup_wizard_smtp(sel_cfg) == 0)
                    config_save_account(sel_cfg);
                config_free(sel_cfg);
                continue;  /* re-display accounts screen with updated info */
            }
            /* acc == 1: Enter → open account */
        }

        logger_log(LOG_INFO, "Switching to account: user=%s host=%s",
                   sel_cfg->user ? sel_cfg->user : "(null)",
                   sel_cfg->host ? sel_cfg->host : "(null)");
        local_store_init(sel_cfg->host, sel_cfg->user);

        /* Remember this as the last-used account for next startup */
        if (sel_cfg->user)
            ui_pref_set_str("last_account", sel_cfg->user);

        /* Per-account folder cursor: restore last used folder for this account */
        char fc_key[256];
        snprintf(fc_key, sizeof(fc_key), "folder_cursor_%s",
                 sel_cfg->user ? sel_cfg->user : "default");
        char *saved_folder = ui_pref_get_str(fc_key);
        const char *init_folder = (saved_folder && saved_folder[0])
                                  ? saved_folder
                                  : (sel_cfg->folder ? sel_cfg->folder : "INBOX");

        /* Open folder browser first — user picks which folder to enter */
        int go_up = 0;
        char *tui_folder = email_service_list_folders_interactive(
                               sel_cfg, init_folder, &go_up);
        free(saved_folder);
        if (tui_folder)
            ui_pref_set_str(fc_key, tui_folder);  /* persist selected folder */

        if (go_up) {
            /* Backspace at folder root → back to accounts screen */
            config_free(sel_cfg);
            continue;
        }
        if (!tui_folder) {
            /* ESC from folder browser → quit */
            config_free(sel_cfg);
            break;
        }

        int back_to_accounts = 0;
        for (;;) {  /* inner: message list + folder browser */
            EmailListOpts opts = {0, tui_folder, page_size, 0, 1, {0}};
            int ret = email_service_list(sel_cfg, &opts);
            if (ret == 1) {
                /* Backspace from message list → folder browser */
                char *sel = email_service_list_folders_interactive(
                                sel_cfg, tui_folder, &go_up);
                free(tui_folder);
                tui_folder = sel;
                if (tui_folder)
                    ui_pref_set_str(fc_key, tui_folder);  /* persist new folder */
                if (go_up) {
                    /* Backspace at folder root → back to accounts */
                    back_to_accounts = 1;
                    break;
                }
                if (!tui_folder) break;  /* ESC from folder browser → quit */
            } else if (ret == 2) {
                /* 'c' → compose new message */
                cmd_compose_interactive(sel_cfg, NULL, NULL, NULL, NULL);
                /* Pause so the user can read the send result before the
                 * list re-renders and clears the screen. */
                printf("\n  [Press any key to return to inbox]\n");
                fflush(stdout);
                {
                    TermRawState *_r = terminal_raw_enter();
                    char _c; ssize_t _n = read(STDIN_FILENO, &_c, 1); (void)_n;
                    terminal_raw_exit(&_r);
                }
            } else if (ret == 3) {
                /* 'r' → reply to current message */
                cmd_reply(sel_cfg, opts.action_uid, tui_folder);
                printf("\n  [Press any key to return to inbox]\n");
                fflush(stdout);
                {
                    TermRawState *_r = terminal_raw_enter();
                    char _c; ssize_t _n = read(STDIN_FILENO, &_c, 1); (void)_n;
                    terminal_raw_exit(&_r);
                }
            } else if (ret == 4) {
                /* background sync finished → re-list to show new messages */
                continue;
            } else {
                result = (ret >= 0) ? 0 : -1;
                break;
            }
        }
        free(tui_folder);
        config_free(sel_cfg);
        if (!back_to_accounts) break;  /* ESC/quit → exit outer loop too */
    }

    /* 6. Cleanup */
    config_free(cfg);
    logger_log(LOG_INFO, "--- email-tui session finished ---");
    logger_close();

    return result >= 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
