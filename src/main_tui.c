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
#include "help_gmail.h"
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
#include "input_line.h"

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
        "For background sync use email-sync (incl. 'email-sync cron setup').\n"
    );
}

/* ── Pre-compose dialog ──────────────────────────────────────────────── */

#define CD_MAX_CONTACTS   512
#define CD_MAX_MATCH        5
#define CD_NFIELDS          4
#define CD_FIELD_TO         0
#define CD_FIELD_CC         1
#define CD_FIELD_BCC        2
#define CD_FIELD_SUBJ       3
#define CD_FIELD_BUFSZ    512

typedef struct { char addr[256]; char name[128]; } CDContact;
typedef struct {
    char to[CD_FIELD_BUFSZ];
    char cc[CD_FIELD_BUFSZ];
    char bcc[CD_FIELD_BUFSZ];
    char subj[CD_FIELD_BUFSZ];
} CDResult;

/** Load contacts from accounts/<user>/contacts.tsv into pre-allocated array.
 *  Format per line: address\tdisplay_name\tfrequency
 *  Returns number of contacts loaded (0 on error or empty file). */
static int cd_contacts_load(const Config *cfg, CDContact *contacts, int maxn) {
    if (!cfg || !cfg->user || !contacts || maxn <= 0) return 0;
    const char *data_base = platform_data_dir();
    if (!data_base) return 0;
    char path[1024];
    snprintf(path, sizeof(path), "%s/email-cli/accounts/%s/contacts.tsv",
             data_base, cfg->user);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int count = 0;
    char line[512];
    while (count < maxn && fgets(line, sizeof(line), f)) {
        /* strip trailing newline */
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
            line[--ll] = '\0';
        if (!ll) continue;
        /* parse: addr\tname\tfreq  (name and freq are optional) */
        char *tab1 = strchr(line, '\t');
        if (!tab1) {
            int alen0 = (int)strlen(line);
            if (alen0 >= (int)sizeof(contacts[count].addr))
                alen0 = (int)sizeof(contacts[count].addr) - 1;
            memcpy(contacts[count].addr, line, (size_t)alen0);
            contacts[count].addr[alen0] = '\0';
            contacts[count].name[0] = '\0';
        } else {
            int alen = (int)(tab1 - line);
            if (alen >= (int)sizeof(contacts[count].addr))
                alen = (int)sizeof(contacts[count].addr) - 1;
            memcpy(contacts[count].addr, line, (size_t)alen);
            contacts[count].addr[alen] = '\0';
            char *tab2 = strchr(tab1+1, '\t');
            int nlen;
            if (tab2)
                nlen = (int)(tab2 - (tab1+1));
            else
                nlen = (int)strlen(tab1+1);
            if (nlen >= (int)sizeof(contacts[count].name))
                nlen = (int)sizeof(contacts[count].name) - 1;
            memcpy(contacts[count].name, tab1+1, (size_t)nlen);
            contacts[count].name[nlen] = '\0';
        }
        if (contacts[count].addr[0]) count++;
    }
    fclose(f);
    return count;
}

/** Case-insensitive substring match against addr and name.
 *  Fills indices[] with up to max matching indices; returns count. */
static int cd_contacts_match(const CDContact *contacts, int ncontacts,
                              const char *query,
                              int *indices, int max) {
    if (!query || !query[0] || ncontacts <= 0 || max <= 0) return 0;
    int found = 0;
    for (int i = 0; i < ncontacts && found < max; i++) {
        if (strcasestr(contacts[i].addr, query) ||
            (contacts[i].name[0] && strcasestr(contacts[i].name, query))) {
            indices[found++] = i;
        }
    }
    return found;
}

/** Extract the current token (text after last ',' or ';') from buf[0..cur).
 *  Strips leading spaces. Writes into tok[toksz]. */
static void cd_current_token(const char *buf, int cur, char *tok, int toksz) {
    if (toksz <= 0) return;
    tok[0] = '\0';
    if (cur <= 0) return;
    /* find last separator */
    int sep = -1;
    for (int i = cur - 1; i >= 0; i--) {
        if (buf[i] == ',' || buf[i] == ';') { sep = i; break; }
    }
    const char *start = buf + sep + 1;
    /* skip leading spaces */
    while (*start == ' ') start++;
    int len = (int)(buf + cur - start);
    if (len <= 0) { tok[0] = '\0'; return; }
    if (len >= toksz) len = toksz - 1;
    memcpy(tok, start, (size_t)len);
    tok[len] = '\0';
}

/** Replace the current token in buf with addr; update *cur.
 *  buf is CD_FIELD_BUFSZ bytes. */
static void cd_complete(char *buf, int *cur, const char *addr) {
    /* find start of current token */
    int sep = -1;
    for (int i = *cur - 1; i >= 0; i--) {
        if (buf[i] == ',' || buf[i] == ';') { sep = i; break; }
    }
    /* token starts at sep+1, skip spaces */
    int tok_start = sep + 1;
    while (tok_start < *cur && buf[tok_start] == ' ') tok_start++;

    int addr_len = (int)strlen(addr);
    int rest_len = (int)strlen(buf + *cur);
    /* new buffer: buf[0..tok_start) + addr + buf[*cur..] */
    int new_len = tok_start + addr_len + rest_len;
    if (new_len >= CD_FIELD_BUFSZ - 1) {
        addr_len = CD_FIELD_BUFSZ - 1 - tok_start - rest_len;
        if (addr_len < 0) addr_len = 0;
        new_len = CD_FIELD_BUFSZ - 1;
    }
    /* shift the rest */
    memmove(buf + tok_start + addr_len, buf + *cur, (size_t)(rest_len + 1));
    memcpy(buf + tok_start, addr, (size_t)addr_len);
    buf[new_len] = '\0';
    *cur = tok_start + addr_len;
}

/** Render the dialog to the terminal. Returns 1-based row of active field. */
static int cd_render(const char *title,
                     char bufs[CD_NFIELDS][CD_FIELD_BUFSZ],
                     int cursors[CD_NFIELDS],
                     int active,
                     const CDContact *contacts,
                     const int *match_idx,
                     int match_count) {
    int tcols = terminal_cols();
    if (tcols < 20) tcols = 80;

    /* Row 1: title bar */
    printf("\033[H");  /* cursor to top-left */
    printf("\033[7m");  /* reverse video */
    char title_line[256];
    snprintf(title_line, sizeof(title_line), "  %s", title);
    int tlen = (int)strlen(title_line);
    printf("%s", title_line);
    for (int i = tlen; i < tcols; i++) putchar(' ');
    printf("\033[0m");

    /* Row 2: blank */
    printf("\033[2;1H\033[K");

    int row = 3;
    int active_row = 3;

    static const char *labels[CD_NFIELDS] = {
        "To:      ",
        "Cc:      ",
        "Bcc:     ",
        "Subject: "
    };

    for (int f = 0; f < CD_NFIELDS; f++) {
        printf("\033[%d;1H\033[K", row);
        if (f == active) {
            printf("\033[1m");  /* bold for active */
            active_row = row;
        } else {
            printf("\033[2m");  /* dim for inactive */
        }
        /* indent + label (9 chars) + space = 2 + 9 + 2 = 13 chars before value */
        printf("  %s  ", labels[f]);
        printf("\033[0m");

        /* Print field value — truncate to fit terminal width */
        const char *val = bufs[f];
        int val_len = (int)strlen(val);
        int avail = tcols - 13;
        if (avail < 1) avail = 1;

        /* Compute display start for scrolling: keep cursor visible */
        int disp_start = 0;
        if (cursors[f] > avail - 1)
            disp_start = cursors[f] - (avail - 1);

        int print_len = val_len - disp_start;
        if (print_len > avail) print_len = avail;
        if (print_len > 0)
            fwrite(val + disp_start, 1, (size_t)print_len, stdout);
        row++;

        /* Suggestions for address fields */
        if (f == active && f < CD_FIELD_SUBJ && match_count > 0) {
            for (int m = 0; m < match_count; m++) {
                printf("\033[%d;1H\033[K", row);
                printf("             \033[36m\xe2\x96\xb6 %-40s  %s\033[0m",
                       contacts[match_idx[m]].addr,
                       contacts[match_idx[m]].name);
                row++;
            }
        }
    }

    /* Blank row after fields */
    printf("\033[%d;1H\033[K", row);
    row++;

    /* Help row */
    printf("\033[%d;1H\033[K", row);
    printf("\033[2m  Tab=next  Shift-Tab=prev  ;=add addr  Enter=OK  ESC=cancel\033[0m");
    row++;

    /* Clear any stale rows below */
    printf("\033[%d;1H\033[J", row);

    fflush(stdout);
    return active_row;
}

/** Pre-compose dialog: collect To, Cc, Bcc, Subject before opening editor.
 *  Returns 1 if confirmed (Enter on Subject), 0 if cancelled (ESC). */
static int compose_dialog(const Config *cfg,
                           const char *title,
                           const char *prefill_to,
                           const char *prefill_cc,
                           const char *prefill_subject,
                           CDResult *out) {
    /* Clear screen */
    printf("\033[2J\033[H");
    fflush(stdout);

    /* Load contacts */
    CDContact *contacts = calloc((size_t)CD_MAX_CONTACTS, sizeof(CDContact));
    if (!contacts) return 0;
    int ncontacts = cd_contacts_load(cfg, contacts, CD_MAX_CONTACTS);

    /* Initialize field buffers */
    char bufs[CD_NFIELDS][CD_FIELD_BUFSZ];
    memset(bufs, 0, sizeof(bufs));
    if (prefill_to && prefill_to[0])
        strncpy(bufs[CD_FIELD_TO], prefill_to, CD_FIELD_BUFSZ - 1);
    if (prefill_cc && prefill_cc[0])
        strncpy(bufs[CD_FIELD_CC], prefill_cc, CD_FIELD_BUFSZ - 1);
    if (prefill_subject && prefill_subject[0])
        strncpy(bufs[CD_FIELD_SUBJ], prefill_subject, CD_FIELD_BUFSZ - 1);

    /* Set cursors to end of pre-filled text */
    int cursors[CD_NFIELDS];
    for (int i = 0; i < CD_NFIELDS; i++)
        cursors[i] = (int)strlen(bufs[i]);

    /* If To is pre-filled, start on CC; otherwise start on TO */
    int active = (prefill_to && prefill_to[0]) ? CD_FIELD_CC : CD_FIELD_TO;

    int result = 0;

    {
        RAII_TERM_RAW TermRawState *raw = terminal_raw_enter();
        if (!raw) { free(contacts); return 0; }

        /* Autocomplete state */
        int match_idx[CD_MAX_MATCH];
        int match_count = 0;

        for (;;) {
            /* Recompute autocomplete for active address field */
            match_count = 0;
            if (active < CD_FIELD_SUBJ) {
                char tok[256];
                cd_current_token(bufs[active], cursors[active], tok, sizeof(tok));
                if (tok[0])
                    match_count = cd_contacts_match(contacts, ncontacts, tok,
                                                    match_idx, CD_MAX_MATCH);
            }

            int active_row = cd_render(title, bufs, cursors, active,
                                       contacts, match_idx, match_count);

            /* Position cursor on active field */
            int tcols = terminal_cols();
            if (tcols < 20) tcols = 80;
            int avail = tcols - 13;
            if (avail < 1) avail = 1;
            int disp_start = 0;
            if (cursors[active] > avail - 1)
                disp_start = cursors[active] - (avail - 1);
            int cursor_col = 13 + (cursors[active] - disp_start) + 1; /* 1-based */
            printf("\033[%d;%dH", active_row, cursor_col);
            fflush(stdout);

            TermKey key = terminal_read_key();

            if (key == TERM_KEY_ESC || key == TERM_KEY_QUIT) {
                result = 0;
                goto dialog_done;
            }

            if (key == TERM_KEY_ENTER) {
                if (active == CD_FIELD_SUBJ) {
                    result = 1;
                    goto dialog_done;
                }
                /* Move to next field */
                active = (active + 1) % CD_NFIELDS;
                continue;
            }

            if (key == TERM_KEY_TAB) {
                if (match_count > 0 && active < CD_FIELD_SUBJ) {
                    cd_complete(bufs[active], &cursors[active],
                                contacts[match_idx[0]].addr);
                } else {
                    active = (active + 1) % CD_NFIELDS;
                }
                continue;
            }

            if (key == TERM_KEY_SHIFT_TAB) {
                active = (active - 1 + CD_NFIELDS) % CD_NFIELDS;
                continue;
            }

            if (key == TERM_KEY_BACK) {
                /* Delete UTF-8 char before cursor */
                if (cursors[active] > 0) {
                    int i = cursors[active] - 1;
                    /* skip UTF-8 continuation bytes */
                    while (i > 0 && ((unsigned char)bufs[active][i] & 0xC0) == 0x80)
                        i--;
                    int rest = (int)strlen(bufs[active]) - cursors[active];
                    memmove(bufs[active] + i, bufs[active] + cursors[active],
                            (size_t)(rest + 1));
                    cursors[active] = i;
                }
                continue;
            }

            if (key == TERM_KEY_DELETE) {
                /* Delete UTF-8 char at cursor */
                int len = (int)strlen(bufs[active]);
                if (cursors[active] < len) {
                    int i = cursors[active] + 1;
                    while (i < len && ((unsigned char)bufs[active][i] & 0xC0) == 0x80)
                        i++;
                    memmove(bufs[active] + cursors[active],
                            bufs[active] + i,
                            (size_t)(len - i + 1));
                }
                continue;
            }

            if (key == TERM_KEY_LEFT) {
                if (cursors[active] > 0) {
                    cursors[active]--;
                    while (cursors[active] > 0 &&
                           ((unsigned char)bufs[active][cursors[active]] & 0xC0) == 0x80)
                        cursors[active]--;
                }
                continue;
            }

            if (key == TERM_KEY_RIGHT) {
                int len = (int)strlen(bufs[active]);
                if (cursors[active] < len) {
                    cursors[active]++;
                    while (cursors[active] < len &&
                           ((unsigned char)bufs[active][cursors[active]] & 0xC0) == 0x80)
                        cursors[active]++;
                }
                continue;
            }

            if (key == TERM_KEY_HOME) {
                cursors[active] = 0;
                continue;
            }

            if (key == TERM_KEY_END) {
                cursors[active] = (int)strlen(bufs[active]);
                continue;
            }

            if (key == TERM_KEY_IGNORE) {
                int ch = terminal_last_printable();
                if (ch == ';' && active < CD_FIELD_SUBJ) {
                    /* Trim trailing spaces then append "; " */
                    int len = (int)strlen(bufs[active]);
                    while (len > 0 && bufs[active][len-1] == ' ')
                        bufs[active][--len] = '\0';
                    if (len + 2 < CD_FIELD_BUFSZ - 1) {
                        bufs[active][len]   = ';';
                        bufs[active][len+1] = ' ';
                        bufs[active][len+2] = '\0';
                        cursors[active] = len + 2;
                    }
                } else {
                    /* Insert UTF-8 sequence at cursor */
                    const char *seq = terminal_last_utf8();
                    if (seq && seq[0]) {
                        int slen = (int)strlen(seq);
                        int cur_len = (int)strlen(bufs[active]);
                        if (cur_len + slen < CD_FIELD_BUFSZ - 1) {
                            memmove(bufs[active] + cursors[active] + slen,
                                    bufs[active] + cursors[active],
                                    (size_t)(cur_len - cursors[active] + 1));
                            memcpy(bufs[active] + cursors[active], seq, (size_t)slen);
                            cursors[active] += slen;
                        }
                    }
                }
                continue;
            }
        }

dialog_done:;
    } /* RAII_TERM_RAW scope ends */

    /* Clear screen */
    printf("\033[2J\033[H");
    fflush(stdout);

    if (result) {
        memcpy(out->to,   bufs[CD_FIELD_TO],   CD_FIELD_BUFSZ);
        memcpy(out->cc,   bufs[CD_FIELD_CC],   CD_FIELD_BUFSZ);
        memcpy(out->bcc,  bufs[CD_FIELD_BCC],  CD_FIELD_BUFSZ);
        memcpy(out->subj, bufs[CD_FIELD_SUBJ], CD_FIELD_BUFSZ);
    }

    free(contacts);
    return result;
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
                                   const char *prefill_cc,
                                   const char *prefill_bcc,
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
        if (prefill_cc && prefill_cc[0])
            fprintf(f, "Cc: %s\n", prefill_cc);
        if (prefill_bcc && prefill_bcc[0])
            fprintf(f, "Bcc: %s\n", prefill_bcc);
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
    char cc_buf[512]     = "";
    char bcc_buf[512]    = "";
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
                else if (strncasecmp(line, "Cc: ",    4)  == 0) { strncpy(cc_buf,    line + 4,  sizeof(cc_buf)    - 1); cc_buf[sizeof(cc_buf)-1]      = '\0'; }
                else if (strncasecmp(line, "Bcc: ",   5)  == 0) { strncpy(bcc_buf,   line + 5,  sizeof(bcc_buf)   - 1); bcc_buf[sizeof(bcc_buf)-1]    = '\0'; }
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
        ComposeParams p = {from_send, to_buf,
                           cc_buf[0]  ? cc_buf  : NULL,
                           bcc_buf[0] ? bcc_buf : NULL,
                           subj_buf, body_str, reply_id};
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

    CDResult dlg = {0};
    if (!compose_dialog(cfg, "Reply", reply_to, NULL, subject, &dlg)) {
        rc = -1;
    } else {
        rc = cmd_compose_interactive(cfg, dlg.to, dlg.cc, dlg.bcc, dlg.subj, msg_id, quoted);
    }
    free(quoted);
    free(reply_to);
    free(subject);
    free(msg_id);
    return rc;
}

/**
 * @brief Forward a message identified by UID in the given folder.
 * Shows pre-compose dialog with prefilled subject, opens editor for body.
 */
static int cmd_forward(Config *cfg, const char *uid, const char *folder) {
    ensure_smtp_configured(cfg);

    /* Load raw message */
    char *raw = NULL;
    if (local_msg_exists(folder, uid))
        raw = local_msg_load(folder, uid);
    else {
        char *saved_folder = cfg->folder;
        cfg->folder = (char *)folder;
        raw = email_service_fetch_raw(cfg, uid);
        cfg->folder = saved_folder;
    }

    if (!raw) {
        fprintf(stderr, "Error: Could not load message UID %s.\n", uid);
        return -1;
    }

    /* Build "Fwd: " subject */
    char *subj_raw = mime_get_header(raw, "Subject");
    char *subj_dec = subj_raw ? mime_decode_words(subj_raw) : strdup("");
    free(subj_raw);
    char fwd_subject[512];
    snprintf(fwd_subject, sizeof(fwd_subject), "Fwd: %s", subj_dec ? subj_dec : "");
    free(subj_dec);

    /* Build quoted body (attribution without "> " prefix — just inline) */
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

    char *quoted = NULL;
    {
        char *from_raw = mime_get_header(raw, "From");
        char *from_dec = from_raw ? mime_decode_words(from_raw) : NULL;
        free(from_raw);
        char *date_raw = mime_get_header(raw, "Date");
        char *date_fmt = date_raw ? mime_format_date(date_raw) : NULL;
        free(date_raw);

        size_t qcap = 4096 + (body_text ? strlen(body_text) * 2 : 0);
        quoted = malloc(qcap);
        if (quoted) {
            int off = snprintf(quoted, qcap,
                               "\n\n--- Forwarded message ---\nFrom: %s\nDate: %s\n\n",
                               from_dec ? from_dec : "?",
                               date_fmt ? date_fmt : "?");
            if (body_text && off > 0) {
                int rest = (int)strlen(body_text);
                if (off + rest < (int)qcap - 1) {
                    memcpy(quoted + off, body_text, (size_t)rest);
                    off += rest;
                    quoted[off] = '\0';
                }
            }
        }
        free(from_dec);
        free(date_fmt);
    }
    free(body_text);
    free(raw);

    /* Show dialog with empty To, prefilled Subject */
    CDResult dlg = {0};
    if (!compose_dialog(cfg, "Forward", NULL, NULL, fwd_subject, &dlg)) {
        free(quoted);
        return -1;
    }
    int rc = cmd_compose_interactive(cfg, dlg.to, dlg.cc, dlg.bcc, dlg.subj, NULL, quoted);
    free(quoted);
    return rc;
}

/**
 * @brief Reply-all to a message identified by UID in the given folder.
 * Extracts From → To, original To+Cc (minus own addr) → Cc.
 */
static int cmd_reply_all(Config *cfg, const char *uid, const char *folder) {
    ensure_smtp_configured(cfg);

    /* Load raw message */
    char *raw = NULL;
    if (local_msg_exists(folder, uid))
        raw = local_msg_load(folder, uid);
    else {
        char *saved_folder = cfg->folder;
        cfg->folder = (char *)folder;
        raw = email_service_fetch_raw(cfg, uid);
        cfg->folder = saved_folder;
    }

    if (!raw) {
        fprintf(stderr, "Error: Could not load message UID %s.\n", uid);
        return -1;
    }

    /* Extract reply metadata (from, subject, msg_id) */
    char *reply_to = NULL, *subject = NULL, *msg_id = NULL;
    int rc = compose_extract_reply_meta(raw, &reply_to, &subject, &msg_id);
    if (rc != 0) {
        free(raw);
        fprintf(stderr, "Error: Could not parse message headers.\n");
        return -1;
    }

    /* Extract original To + Cc, remove own address, join with "; " */
    char cc_buf[1024] = "";
    {
        const char *own = from_address(cfg);
        char *orig_to  = mime_get_header(raw, "To");
        char *orig_cc  = mime_get_header(raw, "Cc");
        char *dec_to   = orig_to ? mime_decode_words(orig_to)  : NULL;
        char *dec_cc   = orig_cc ? mime_decode_words(orig_cc)  : NULL;
        free(orig_to); free(orig_cc);

        /* Combine dec_to and dec_cc separated by ", " */
        char combined[2048] = "";
        if (dec_to && dec_to[0]) {
            strncpy(combined, dec_to, sizeof(combined) - 1);
        }
        if (dec_cc && dec_cc[0]) {
            if (combined[0]) {
                strncat(combined, ", ", sizeof(combined) - strlen(combined) - 1);
            }
            strncat(combined, dec_cc, sizeof(combined) - strlen(combined) - 1);
        }
        free(dec_to); free(dec_cc);

        /* Build cc_buf: split on commas, skip own address */
        int cc_len = 0;
        char *p = combined;
        while (p && *p) {
            while (*p == ' ') p++;
            char *sep = strchr(p, ',');
            int elen;
            if (sep) {
                elen = (int)(sep - p);
            } else {
                elen = (int)strlen(p);
            }
            /* Trim trailing spaces */
            while (elen > 0 && p[elen-1] == ' ') elen--;
            if (elen > 0 && !(own && strcasestr(p, own ? own : ""))) {
                if (cc_len + elen + 3 < (int)sizeof(cc_buf) - 1) {
                    if (cc_len > 0) {
                        cc_buf[cc_len++] = ';';
                        cc_buf[cc_len++] = ' ';
                    }
                    memcpy(cc_buf + cc_len, p, (size_t)elen);
                    cc_len += elen;
                    cc_buf[cc_len] = '\0';
                }
            }
            if (!sep) break;
            p = sep + 1;
        }
    }

    /* Extract body for quoting */
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

    char *quoted = NULL;
    {
        char *from_raw = mime_get_header(raw, "From");
        char *from_dec = from_raw ? mime_decode_words(from_raw) : NULL;
        free(from_raw);
        char *date_raw = mime_get_header(raw, "Date");
        char *date_fmt = date_raw ? mime_format_date(date_raw) : NULL;
        free(date_raw);

        size_t qcap = 4096 + (body_text ? strlen(body_text) * 2 : 0);
        quoted = malloc(qcap);
        if (quoted) {
            int off = snprintf(quoted, qcap, "On %s, %s wrote:\n",
                               date_fmt ? date_fmt : "?",
                               from_dec ? from_dec : "?");
            if (body_text) {
                const char *bp = body_text;
                while (*bp && off < (int)qcap - 4) {
                    const char *nl = strchr(bp, '\n');
                    size_t len = nl ? (size_t)(nl - bp) : strlen(bp);
                    size_t advance = len + (nl ? 1 : 0);
                    if (len > 0 && bp[len-1] == '\r') len--;
                    if (off + 2 + (int)len + 1 < (int)qcap) {
                        quoted[off++] = '>';
                        quoted[off++] = ' ';
                        memcpy(quoted + off, bp, len);
                        off += (int)len;
                        quoted[off++] = '\n';
                        quoted[off]   = '\0';
                    }
                    bp += advance;
                    if (!nl) break;
                }
            }
        }
        free(from_dec);
        free(date_fmt);
    }
    free(body_text);
    free(raw);

    CDResult dlg = {0};
    if (!compose_dialog(cfg, "Reply All", reply_to, cc_buf[0] ? cc_buf : NULL, subject, &dlg)) {
        rc = -1;
    } else {
        rc = cmd_compose_interactive(cfg, dlg.to, dlg.cc, dlg.bcc, dlg.subj, msg_id, quoted);
    }
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
        /* Gmail OAuth credential check */
        if (sel_cfg->gmail_mode &&
            !sel_cfg->gmail_refresh_token &&
            (!sel_cfg->gmail_client_id || !sel_cfg->gmail_client_id[0])) {
            fprintf(stderr,
                "\n  This Gmail account has no OAuth2 credentials configured.\n"
                "  Add GMAIL_CLIENT_ID and GMAIL_CLIENT_SECRET to:\n"
                "    ~/.config/email-cli/accounts/%s/config.ini\n"
                "  Run 'email-cli help gmail' for the setup guide.\n\n"
                "  Press any key to return to accounts...\n",
                sel_cfg->user ? sel_cfg->user : "(unknown)");
            fflush(stderr);
            {
                TermRawState *_r = terminal_raw_enter();
                char _c; ssize_t _n = read(STDIN_FILENO, &_c, 1); (void)_n;
                terminal_raw_exit(&_r);
            }
            config_free(sel_cfg);
            continue;
        }

        const char *init_folder = (saved_folder && saved_folder[0])
                                  ? saved_folder
                                  : (sel_cfg->folder ? sel_cfg->folder : "INBOX");

        /* Open folder/label browser — user picks which folder/label to enter */
        int go_up = 0;
        char *tui_folder = sel_cfg->gmail_mode
            ? email_service_list_labels_interactive(sel_cfg, init_folder, &go_up)
            : email_service_list_folders_interactive(sel_cfg, init_folder, &go_up);
        free(saved_folder);
        if (tui_folder)
            ui_pref_set_str(fc_key, tui_folder);  /* persist selected folder */

        if (!tui_folder && !go_up) {
            /* Connection error or empty folder list — show actionable advice */
            fprintf(stderr, "\n  Connection failed.\n\n");
            if (sel_cfg->gmail_mode) {
                fprintf(stderr,
                    "  Possible causes:\n"
                    "  - OAuth2 credentials missing or expired\n"
                    "  - Network issue\n\n"
                    "  To fix: check GMAIL_CLIENT_ID and GMAIL_REFRESH_TOKEN in\n"
                    "    ~/.config/email-cli/accounts/%s/config.ini\n"
                    "  Run 'email-cli help gmail' for the setup guide.\n",
                    sel_cfg->user ? sel_cfg->user : "(unknown)");
            } else if (sel_cfg->host &&
                       (strstr(sel_cfg->host, "gmail.com") ||
                        strstr(sel_cfg->host, "google.com") ||
                        strstr(sel_cfg->host, "googlemail.com"))) {
                fprintf(stderr,
                    "  Gmail is not supported via IMAP in email-cli.\n\n"
                    "  To fix: delete this account ('d' on accounts screen) and\n"
                    "  re-add it ('n') as Gmail type (option 2), which uses the\n"
                    "  Gmail API with OAuth2 instead of IMAP.\n\n"
                    "  Run 'email-cli help gmail' for the setup guide.\n");
            } else {
                fprintf(stderr,
                    "  Possible causes:\n"
                    "  - Wrong password or username\n"
                    "  - Server requires app-specific password (e.g. Gmail with 2FA)\n"
                    "  - Server address or port is incorrect\n"
                    "  - Network issue\n\n"
                    "  To fix: press 'i' on accounts screen to edit IMAP settings,\n"
                    "  or delete and re-add the account with 'd' then 'n'.\n");
            }
            fprintf(stderr, "\n  Press any key to return to accounts...\n");
            fflush(stderr);
            {
                TermRawState *_r = terminal_raw_enter();
                char _c; ssize_t _n = read(STDIN_FILENO, &_c, 1); (void)_n;
                terminal_raw_exit(&_r);
            }
            config_free(sel_cfg);
            continue;
        }
        if (go_up) {
            /* Backspace at folder root → back to accounts screen */
            config_free(sel_cfg);
            continue;
        }

        int back_to_accounts = 0;
        for (;;) {  /* inner: message list + folder browser */
            EmailListOpts opts = {0, tui_folder, page_size, 0, 1, {0}};
            int ret = email_service_list(sel_cfg, &opts);
            if (ret == 1) {
                /* Backspace from message list → folder/label browser */
                char *sel = sel_cfg->gmail_mode
                    ? email_service_list_labels_interactive(sel_cfg, tui_folder, &go_up)
                    : email_service_list_folders_interactive(sel_cfg, tui_folder, &go_up);
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
                /* 'c' → compose new message via pre-compose dialog */
                {
                    CDResult dlg = {0};
                    if (compose_dialog(sel_cfg, "New Message", NULL, NULL, NULL, &dlg)) {
                        cmd_compose_interactive(sel_cfg, dlg.to, dlg.cc, dlg.bcc,
                                                dlg.subj, NULL, NULL);
                    }
                }
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
            } else if (ret == 5) {
                /* 'F' → forward current message */
                cmd_forward(sel_cfg, opts.action_uid, tui_folder);
                printf("\n  [Press any key to return to inbox]\n");
                fflush(stdout);
                {
                    TermRawState *_r = terminal_raw_enter();
                    char _c; ssize_t _n = read(STDIN_FILENO, &_c, 1); (void)_n;
                    terminal_raw_exit(&_r);
                }
            } else if (ret == 6) {
                /* 'A' → reply-all */
                cmd_reply_all(sel_cfg, opts.action_uid, tui_folder);
                printf("\n  [Press any key to return to inbox]\n");
                fflush(stdout);
                {
                    TermRawState *_r = terminal_raw_enter();
                    char _c; ssize_t _n = read(STDIN_FILENO, &_c, 1); (void)_n;
                    terminal_raw_exit(&_r);
                }
            } else if (ret < 0) {
                /* Connection or command failed — show actionable advice */
                fprintf(stderr, "\n  Operation failed. Check the error above.\n"
                                "  Press 'i' on accounts screen to edit IMAP settings.\n\n"
                                "  Press any key to return to accounts...\n");
                fflush(stderr);
                {
                    TermRawState *_r = terminal_raw_enter();
                    char _c; ssize_t _n = read(STDIN_FILENO, &_c, 1); (void)_n;
                    terminal_raw_exit(&_r);
                }
                back_to_accounts = 1;
                break;
            } else {
                /* ret == 0: normal quit (ESC/q) */
                result = 0;
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
