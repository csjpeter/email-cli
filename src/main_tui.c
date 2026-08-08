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
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
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
#include "mail_rules.h"

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
        "    l         Open rules editor\n"
        "    U         Refresh after sync\n"
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
/* Minimum token length that opens the contact autocomplete dropdown
 * (docs/spec/compose-dialog.md: "Typing >= 2 characters ... opens the
 * autocomplete dropdown"; a single character would match almost everything). */
#define CD_AC_MIN_CHARS     2
#define CD_NFIELDS          5
#define CD_FIELD_TO         0
#define CD_FIELD_CC         1
#define CD_FIELD_BCC        2
#define CD_FIELD_SUBJ       3
#define CD_FIELD_ATTACH     4
#define CD_FIELD_BUFSZ    512
#define CD_MAX_ATTACH       8
#define CD_ATTACH_PATH_SZ 4096
#define CD_MAX_FILE_MATCH   8

typedef struct { char addr[256]; char name[128]; } CDContact;
typedef struct {
    char to[CD_FIELD_BUFSZ];
    char cc[CD_FIELD_BUFSZ];
    char bcc[CD_FIELD_BUFSZ];
    char subj[CD_FIELD_BUFSZ];
    char attach_paths[CD_MAX_ATTACH][CD_ATTACH_PATH_SZ];
    int  attach_count;
} CDResult;

typedef struct {
    char paths[CD_MAX_ATTACH][CD_ATTACH_PATH_SZ];
    int  count;
    char matches[CD_MAX_FILE_MATCH][CD_ATTACH_PATH_SZ];
    int  match_count;
    char error[256];
} CDAttachState;

/** List filesystem entries matching a typed path prefix for Attach tab-completion.
 *  If path_so_far ends with '/', list all entries in that directory.
 *  Otherwise splits at last '/' to get dir + prefix and lists matching entries.
 *  Returns number of matches (up to max_matches). */
static int cd_files_match(const char *path_so_far,
                           char matches[CD_MAX_FILE_MATCH][CD_ATTACH_PATH_SZ],
                           int max_matches)
{
    if (!path_so_far || !path_so_far[0] || max_matches <= 0) return 0;
    char dir[CD_ATTACH_PATH_SZ];
    char prefix[CD_ATTACH_PATH_SZ];
    const char *last_slash = strrchr(path_so_far, '/');
    if (last_slash) {
        size_t dir_len = (size_t)(last_slash - path_so_far) + 1;
        if (dir_len >= CD_ATTACH_PATH_SZ) return 0;
        memcpy(dir, path_so_far, dir_len);
        dir[dir_len] = '\0';
        strncpy(prefix, last_slash + 1, CD_ATTACH_PATH_SZ - 1);
        prefix[CD_ATTACH_PATH_SZ - 1] = '\0';
    } else {
        strncpy(dir, "./", CD_ATTACH_PATH_SZ - 1);
        strncpy(prefix, path_so_far, CD_ATTACH_PATH_SZ - 1);
        prefix[CD_ATTACH_PATH_SZ - 1] = '\0';
    }
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    size_t prefix_len = strlen(prefix);
    struct dirent *de;
    while ((de = readdir(d)) != NULL && found < max_matches) {
        if (de->d_name[0] == '.') continue;
        if (prefix_len > 0 && strncmp(de->d_name, prefix, prefix_len) != 0) continue;
        size_t dlen = strlen(dir);
        size_t nlen = strlen(de->d_name);
        if (dlen + nlen >= CD_ATTACH_PATH_SZ) continue;
        memcpy(matches[found], dir, dlen);
        memcpy(matches[found] + dlen, de->d_name, nlen + 1);
        found++;
    }
    closedir(d);
    return found;
}

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

/** Returns 1 if the account's contacts.tsv exists (even when empty). */
static int cd_contacts_file_exists(const Config *cfg) {
    if (!cfg || !cfg->user) return 0;
    const char *data_base = platform_data_dir();
    if (!data_base) return 0;
    char path[1024];
    snprintf(path, sizeof(path), "%s/email-cli/accounts/%s/contacts.tsv",
             data_base, cfg->user);
    struct stat st;
    return stat(path, &st) == 0;
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
                     int match_count,
                     const CDAttachState *attach,
                     int match_sel,
                     const char *dlg_error) {
    int tcols = terminal_cols();
    if (tcols < 20) tcols = 80;

    printf("\033[H");
    printf("\033[7m");
    char title_line[256];
    snprintf(title_line, sizeof(title_line), "  %s", title);
    int tlen = (int)strlen(title_line);
    printf("%s", title_line);
    for (int i = tlen; i < tcols; i++) putchar(' ');
    printf("\033[0m");

    printf("\033[2;1H\033[K");

    int row = 3;
    int active_row = 3;

    static const char *labels[CD_NFIELDS] = {
        "To:      ",
        "Cc:      ",
        "Bcc:     ",
        "Subject: ",
        "Attach:  "
    };

    for (int f = 0; f < CD_NFIELDS; f++) {
        printf("\033[%d;1H\033[K", row);
        if (f == active) {
            printf("\033[1m");
            active_row = row;
        } else {
            printf("\033[2m");
        }
        printf("  %s  ", labels[f]);
        printf("\033[0m");

        const char *val = bufs[f];
        int val_len = (int)strlen(val);
        int avail = tcols - 13;
        if (avail < 1) avail = 1;

        int disp_start = 0;
        if (cursors[f] > avail - 1)
            disp_start = cursors[f] - (avail - 1);

        int print_len = val_len - disp_start;
        if (print_len > avail) print_len = avail;
        if (print_len > 0)
            fwrite(val + disp_start, 1, (size_t)print_len, stdout);
        row++;

        if (f == active && f < CD_FIELD_SUBJ && match_count > 0) {
            /* Contact autocomplete dropdown */
            for (int m = 0; m < match_count; m++) {
                printf("\033[%d;1H\033[K", row);
                printf("             %s\033[36m\xe2\x96\xb6 %-40s  %s\033[0m",
                       (m == match_sel) ? "\033[7m" : "",
                       contacts[match_idx[m]].addr,
                       contacts[match_idx[m]].name);
                row++;
            }
        } else if (f == active && f == CD_FIELD_ATTACH && attach && attach->match_count > 0) {
            /* File browser dropdown */
            for (int m = 0; m < attach->match_count; m++) {
                const char *fname = strrchr(attach->matches[m], '/');
                fname = fname ? fname + 1 : attach->matches[m];
                printf("\033[%d;1H\033[K", row);
                printf("             \033[36m\xe2\x96\xb6 %s\033[0m", fname);
                row++;
            }
        }
    }

    /* List of already-added attachments */
    if (attach && attach->count > 0) {
        printf("\033[%d;1H\033[K", row);
        printf("  \033[2mAdded:\033[0m");
        row++;
        for (int i = 0; i < attach->count; i++) {
            const char *fname = strrchr(attach->paths[i], '/');
            fname = fname ? fname + 1 : attach->paths[i];
            printf("\033[%d;1H\033[K", row);
            printf("    \033[32m[%d] %s\033[0m", i + 1, fname);
            row++;
        }
    }

    /* Dialog-level inline error (empty To, no-subject prompt, …) */
    if (dlg_error && dlg_error[0]) {
        printf("\033[%d;1H\033[K", row);
        printf("  \033[31m%s\033[0m", dlg_error);
        row++;
    }

    /* Inline error message */
    if (attach && attach->error[0]) {
        printf("\033[%d;1H\033[K", row);
        printf("  \033[31m%s\033[0m", attach->error);
        row++;
    }

    printf("\033[%d;1H\033[K", row);
    row++;

    printf("\033[%d;1H\033[K", row);
    if (active == CD_FIELD_ATTACH) {
        printf("\033[2m  Tab=complete  Shift-Tab=prev  Enter=add  d=remove last  Enter(empty)=OK  ESC=cancel\033[0m");
    } else {
        printf("\033[2m  Tab=next  Shift-Tab=prev  ;=add addr  Enter=OK  ESC=cancel\033[0m");
    }
    row++;

    printf("\033[%d;1H\033[J", row);
    fflush(stdout);
    return active_row;
}

/** Pre-compose dialog: collect To, Cc, Bcc, Subject, and optional Attach paths.
 *  Returns 1 if confirmed, 0 if cancelled (ESC).
 *  out->attach_paths/attach_count on entry are used to pre-populate the attach list. */
static int compose_dialog(const Config *cfg,
                           const char *title,
                           const char *prefill_to,
                           const char *prefill_cc,
                           const char *prefill_subject,
                           CDResult *out) {
    printf("\033[2J\033[H");
    fflush(stdout);

    CDContact *contacts = calloc((size_t)CD_MAX_CONTACTS, sizeof(CDContact));
    if (!contacts) return 0;
    int ncontacts = cd_contacts_load(cfg, contacts, CD_MAX_CONTACTS);
    if (ncontacts == 0 && cfg && cfg->user && !cd_contacts_file_exists(cfg)) {
        /* No contacts cache yet — build it once from the cached headers so the
         * very first compose already offers autocomplete.  Keyed on the file's
         * existence, not on the entry count: an account with no cached headers
         * yields an empty file, and re-scanning on every open would be slow. */
        local_contacts_rebuild();
        ncontacts = cd_contacts_load(cfg, contacts, CD_MAX_CONTACTS);
    }

    char bufs[CD_NFIELDS][CD_FIELD_BUFSZ];
    memset(bufs, 0, sizeof(bufs));
    if (prefill_to      && prefill_to[0])      strncpy(bufs[CD_FIELD_TO],   prefill_to,      CD_FIELD_BUFSZ - 1);
    if (prefill_cc      && prefill_cc[0])      strncpy(bufs[CD_FIELD_CC],   prefill_cc,      CD_FIELD_BUFSZ - 1);
    if (prefill_subject && prefill_subject[0]) strncpy(bufs[CD_FIELD_SUBJ], prefill_subject, CD_FIELD_BUFSZ - 1);

    int cursors[CD_NFIELDS];
    for (int i = 0; i < CD_NFIELDS; i++)
        cursors[i] = (int)strlen(bufs[i]);

    int active = (prefill_to && prefill_to[0]) ? CD_FIELD_CC : CD_FIELD_TO;

    /* Attachment state — pre-populate from out if caller passed existing attachments */
    CDAttachState attach_state;
    memset(&attach_state, 0, sizeof(attach_state));
    if (out && out->attach_count > 0 && out->attach_count <= CD_MAX_ATTACH) {
        memcpy(attach_state.paths, out->attach_paths, sizeof(attach_state.paths));
        attach_state.count = out->attach_count;
    }

    int result = 0;

    {
        RAII_TERM_RAW TermRawState *raw = terminal_raw_enter();
        if (!raw) { free(contacts); return 0; }

        int match_idx[CD_MAX_MATCH];
        int match_count = 0;
        int match_sel   = 0;   /* highlighted dropdown row */
        int dd_hidden   = 0;   /* Esc dismissed the dropdown for this token */
        char dlg_error[256] = "";

        for (;;) {
            /* Autocomplete: contacts for address fields, files for Attach */
            match_count = 0;
            attach_state.match_count = 0;
            if (active < CD_FIELD_SUBJ && !dd_hidden) {
                char tok[256];
                cd_current_token(bufs[active], cursors[active], tok, sizeof(tok));
                if (strlen(tok) >= CD_AC_MIN_CHARS)
                    match_count = cd_contacts_match(contacts, ncontacts, tok,
                                                    match_idx, CD_MAX_MATCH);
            }
            else if (active == CD_FIELD_ATTACH && bufs[CD_FIELD_ATTACH][0]) {
                attach_state.match_count = cd_files_match(bufs[CD_FIELD_ATTACH],
                                                           attach_state.matches,
                                                           CD_MAX_FILE_MATCH);
            }
            if (match_sel >= match_count) match_sel = 0;

            int active_row = cd_render(title, bufs, cursors, active,
                                       contacts, match_idx, match_count,
                                       &attach_state, match_sel, dlg_error);

            int tcols = terminal_cols();
            if (tcols < 20) tcols = 80;
            int avail = tcols - 13;
            if (avail < 1) avail = 1;
            int disp_start = 0;
            if (cursors[active] > avail - 1)
                disp_start = cursors[active] - (avail - 1);
            int cursor_col = 13 + (cursors[active] - disp_start) + 1;
            printf("\033[%d;%dH", active_row, cursor_col);
            fflush(stdout);

            TermKey key = terminal_read_key();

            if (key == TERM_KEY_ESC || key == TERM_KEY_QUIT) {
                if (match_count > 0) {
                    dd_hidden = 1;      /* dismiss dropdown, keep typed text */
                    continue;
                }
                result = 0;
                goto dialog_done;
            }

            if (key == TERM_KEY_ENTER) {
                if (match_count > 0 && active < CD_FIELD_SUBJ) {
                    /* Dropdown open: commit the highlighted address instead of
                     * confirming the dialog. */
                    cd_complete(bufs[active], &cursors[active],
                                contacts[match_idx[match_sel]].addr);
                    dd_hidden = 1;
                    match_sel = 0;
                    continue;
                }
                if (active == CD_FIELD_ATTACH) {
                    if (bufs[CD_FIELD_ATTACH][0]) {
                        /* Validate and add the typed path */
                        struct stat st;
                        if (stat(bufs[CD_FIELD_ATTACH], &st) != 0) {
                            snprintf(attach_state.error, sizeof(attach_state.error),
                                     "File not found: %.200s", bufs[CD_FIELD_ATTACH]);
                        } else if (!S_ISREG(st.st_mode)) {
                            snprintf(attach_state.error, sizeof(attach_state.error),
                                     "Not a file: %.220s", bufs[CD_FIELD_ATTACH]);
                        } else if (attach_state.count >= CD_MAX_ATTACH) {
                            snprintf(attach_state.error, sizeof(attach_state.error),
                                     "Max attachments (%d) reached", CD_MAX_ATTACH);
                        } else {
                            strncpy(attach_state.paths[attach_state.count],
                                    bufs[CD_FIELD_ATTACH], CD_ATTACH_PATH_SZ - 1);
                            attach_state.count++;
                            bufs[CD_FIELD_ATTACH][0] = '\0';
                            cursors[CD_FIELD_ATTACH] = 0;
                            attach_state.match_count = 0;
                            attach_state.error[0] = '\0';
                        }
                    } else {
                        /* Empty Attach field + Enter → confirm dialog */
                        goto confirm_dialog;
                    }
                } else {
                    goto confirm_dialog;
                }
                continue;
            }

            if (key == TERM_KEY_TAB) {
                if (match_count > 0 && active < CD_FIELD_SUBJ) {
                    cd_complete(bufs[active], &cursors[active],
                                contacts[match_idx[match_sel]].addr);
                    dd_hidden = 1;
                    match_sel = 0;
                } else if (active == CD_FIELD_ATTACH && attach_state.match_count > 0) {
                    size_t _ml = strlen(attach_state.matches[0]);
                    if (_ml >= CD_FIELD_BUFSZ) _ml = CD_FIELD_BUFSZ - 1;
                    memcpy(bufs[CD_FIELD_ATTACH], attach_state.matches[0], _ml);
                    bufs[CD_FIELD_ATTACH][_ml] = '\0';
                    cursors[CD_FIELD_ATTACH] = (int)strlen(bufs[CD_FIELD_ATTACH]);
                    attach_state.error[0] = '\0';
                } else {
                    active = (active + 1) % CD_NFIELDS;
                }
                continue;
            }

            if (key == TERM_KEY_NEXT_LINE && match_count > 0) {
                if (match_sel < match_count - 1) match_sel++;
                continue;
            }

            if (key == TERM_KEY_PREV_LINE && match_count > 0) {
                if (match_sel > 0) match_sel--;
                continue;
            }

            if (key == TERM_KEY_SHIFT_TAB) {
                active = (active - 1 + CD_NFIELDS) % CD_NFIELDS;
                continue;
            }

            if (key == TERM_KEY_BACK) {
                dd_hidden = 0;
                if (active < CD_FIELD_SUBJ && cursors[active] > 0) {
                    /* If the caret sits right after a separator the current
                     * token is empty — drop the whole preceding address. */
                    char tok[256];
                    cd_current_token(bufs[active], cursors[active], tok, sizeof(tok));
                    if (!tok[0]) {
                        int i = cursors[active] - 1;
                        while (i >= 0 && (bufs[active][i] == ' ' ||
                                          bufs[active][i] == ';' ||
                                          bufs[active][i] == ',')) i--;
                        while (i >= 0 && bufs[active][i] != ';' &&
                                         bufs[active][i] != ',') i--;
                        i++;                        /* first byte of that address */
                        while (bufs[active][i] == ' ') i++;
                        int rest = (int)strlen(bufs[active]) - cursors[active];
                        memmove(bufs[active] + i, bufs[active] + cursors[active],
                                (size_t)(rest + 1));
                        cursors[active] = i;
                        continue;
                    }
                }
                if (cursors[active] > 0) {
                    int i = cursors[active] - 1;
                    while (i > 0 && ((unsigned char)bufs[active][i] & 0xC0) == 0x80)
                        i--;
                    int rest = (int)strlen(bufs[active]) - cursors[active];
                    memmove(bufs[active] + i, bufs[active] + cursors[active],
                            (size_t)(rest + 1));
                    cursors[active] = i;
                    if (active == CD_FIELD_ATTACH) attach_state.error[0] = '\0';
                }
                continue;
            }

            if (key == TERM_KEY_DELETE) {
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

            if (key == TERM_KEY_HOME) { cursors[active] = 0; continue; }
            if (key == TERM_KEY_END)  { cursors[active] = (int)strlen(bufs[active]); continue; }

            if (0) {
confirm_dialog:
                /* Empty-To guard: inline error, focus stays on To. */
                if (!bufs[CD_FIELD_TO][0]) {
                    snprintf(dlg_error, sizeof(dlg_error),
                             "To: is empty \xe2\x80\x94 enter at least one recipient.");
                    active = CD_FIELD_TO;
                    continue;
                }
                /* No-subject warning: 'y' or Enter proceeds, anything else
                 * returns focus to the Subject field. */
                if (!bufs[CD_FIELD_SUBJ][0]) {
                    snprintf(dlg_error, sizeof(dlg_error),
                             "Subject is empty \xe2\x80\x94 send anyway? (y/n)");
                    cd_render(title, bufs, cursors, active, contacts,
                              match_idx, 0, &attach_state, 0, dlg_error);
                    TermKey ck = terminal_read_key();
                    int cch = (ck == TERM_KEY_IGNORE) ? terminal_last_printable() : 0;
                    dlg_error[0] = '\0';
                    if (!(cch == 'y' || cch == 'Y' || ck == TERM_KEY_ENTER)) {
                        active = CD_FIELD_SUBJ;
                        continue;
                    }
                }
                result = 1;
                goto dialog_done;
            }

            if (key == TERM_KEY_IGNORE) {
                int ch = terminal_last_printable();
                if (ch) { dd_hidden = 0; match_sel = 0; dlg_error[0] = '\0'; }
                if (ch == 'd' && active == CD_FIELD_ATTACH &&
                    bufs[CD_FIELD_ATTACH][0] == '\0') {
                    /* Remove last attachment (only when input field is empty) */
                    if (attach_state.count > 0) {
                        attach_state.count--;
                        attach_state.paths[attach_state.count][0] = '\0';
                        attach_state.error[0] = '\0';
                    }
                } else if (ch == ';' && active < CD_FIELD_SUBJ) {
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
                            if (active == CD_FIELD_ATTACH) attach_state.error[0] = '\0';
                        }
                    }
                }
                continue;
            }
        }

dialog_done:;
    } /* RAII_TERM_RAW scope ends */

    printf("\033[2J\033[H");
    fflush(stdout);

    if (result) {
        memcpy(out->to,   bufs[CD_FIELD_TO],   CD_FIELD_BUFSZ);
        memcpy(out->cc,   bufs[CD_FIELD_CC],   CD_FIELD_BUFSZ);
        memcpy(out->bcc,  bufs[CD_FIELD_BCC],  CD_FIELD_BUFSZ);
        memcpy(out->subj, bufs[CD_FIELD_SUBJ], CD_FIELD_BUFSZ);
        memcpy(out->attach_paths, attach_state.paths, sizeof(out->attach_paths));
        out->attach_count = attach_state.count;
    }

    free(contacts);
    return result;
}

/* ── Post-compose review screen ─────────────────────────────────────── */

#define PCR_SEND         0
#define PCR_CANCEL       (-1)
#define PCR_EDIT_BODY    1
#define PCR_EDIT_HEADERS 2

/* Render the review screen — called in a loop from post_compose_review. */
static void pcr_render(const char *from, const char *to, const char *cc,
                        const char *bcc, const char *subject,
                        const char *body,
                        char attach_paths[][CD_ATTACH_PATH_SZ],
                        int attach_count, const char *status_msg)
{
    int tcols = terminal_cols();
    if (tcols < 20) tcols = 80;

    printf("\033[2J\033[H");
    printf("\033[7m");
    const char *ttl = "  Review: Message";
    printf("%s", ttl);
    int tlen = (int)strlen(ttl);
    for (int i = tlen; i < tcols; i++) putchar(' ');
    printf("\033[0m\n\n");

    printf("  \033[2mFrom:\033[0m    %s\n", from   ? from    : "");
    printf("  \033[2mTo:\033[0m      %s\n", to     ? to      : "");
    if (cc  && cc[0])  printf("  \033[2mCc:\033[0m      %s\n", cc);
    if (bcc && bcc[0]) printf("  \033[2mBcc:\033[0m     %s\n", bcc);
    printf("  \033[2mSubject:\033[0m %s\n", subject ? subject : "(no subject)");

    int body_lines = 0;
    if (body && body[0]) {
        for (const char *p = body; *p; p++)
            if (*p == '\n') body_lines++;
        if (body[strlen(body) - 1] != '\n') body_lines++;
    }
    printf("  \033[2mBody:\033[0m    %d line%s\n", body_lines, body_lines != 1 ? "s" : "");

    if (attach_count == 0) {
        printf("  \033[2mAttach:\033[0m  (none)\n");
    } else {
        for (int i = 0; i < attach_count; i++) {
            const char *fname = strrchr(attach_paths[i], '/');
            fname = fname ? fname + 1 : attach_paths[i];
            if (i == 0) printf("  \033[2mAttach:\033[0m  %s\n", fname);
            else        printf("           %s\n", fname);
        }
    }

    printf("\n");
    if (status_msg && status_msg[0])
        printf("  \033[31m%s\033[0m\n\n", status_msg);

    printf("  \033[2m[s] Send  [e] Edit headers  [b] Edit body  [a] Add attachment  [d] Remove last  [q] Cancel\033[0m\n");
    fflush(stdout);
}

/* Inline single-field attachment picker used by [a] key in review.
 * Assumes terminal is already in raw mode.
 * Returns 1 if a file was added, 0 if cancelled. */
static int pcr_pick_attach_raw(char attach_paths[CD_MAX_ATTACH][CD_ATTACH_PATH_SZ],
                                int *attach_count)
{
    char input[CD_FIELD_BUFSZ] = "";
    int cursor = 0;
    char file_matches[CD_MAX_FILE_MATCH][CD_ATTACH_PATH_SZ];
    int file_match_count = 0;
    char error[256] = "";

    for (;;) {
        file_match_count = 0;
        if (input[0])
            file_match_count = cd_files_match(input, file_matches, CD_MAX_FILE_MATCH);

        int tcols = terminal_cols();
        if (tcols < 20) tcols = 80;

        printf("\033[2J\033[H");
        printf("\033[7m  Add Attachment\033[0m\n\n");
        printf("  \033[1mPath:\033[0m  %s\n", input);

        int row = 4;
        for (int m = 0; m < file_match_count; m++) {
            const char *fn = strrchr(file_matches[m], '/');
            fn = fn ? fn + 1 : file_matches[m];
            printf("\033[%d;1H\033[K  \033[36m\xe2\x96\xb6 %s\033[0m", row++, fn);
        }
        if (error[0]) {
            printf("\033[%d;1H\033[K  \033[31m%s\033[0m", row++, error);
        }
        printf("\033[%d;1H\033[K", row);
        printf("\033[%d;1H\033[2m  Tab=complete  Enter=add  ESC=cancel\033[0m", row + 1);
        printf("\033[3;%dH", 9 + cursor);   /* position cursor on input field */
        (void)tcols;
        fflush(stdout);

        TermKey key = terminal_read_key();

        if (key == TERM_KEY_ESC || key == TERM_KEY_QUIT) return 0;

        if (key == TERM_KEY_ENTER) {
            if (!input[0]) return 0;
            struct stat st;
            if (stat(input, &st) != 0) {
                snprintf(error, sizeof(error), "File not found: %.200s", input);
                continue;
            }
            if (!S_ISREG(st.st_mode)) {
                snprintf(error, sizeof(error), "Not a file: %.220s", input);
                continue;
            }
            if (*attach_count >= CD_MAX_ATTACH) {
                snprintf(error, sizeof(error), "Max attachments (%d) reached", CD_MAX_ATTACH);
                continue;
            }
            strncpy(attach_paths[*attach_count], input, CD_ATTACH_PATH_SZ - 1);
            attach_paths[*attach_count][CD_ATTACH_PATH_SZ - 1] = '\0';
            (*attach_count)++;
            return 1;
        }

        if (key == TERM_KEY_TAB && file_match_count > 0) {
            strncpy(input, file_matches[0], CD_FIELD_BUFSZ - 1);
            input[CD_FIELD_BUFSZ - 1] = '\0';
            cursor = (int)strlen(input);
            error[0] = '\0';
            continue;
        }

        if (key == TERM_KEY_BACK && cursor > 0) {
            int i = cursor - 1;
            while (i > 0 && ((unsigned char)input[i] & 0xC0) == 0x80) i--;
            int rest = (int)strlen(input) - cursor;
            memmove(input + i, input + cursor, (size_t)(rest + 1));
            cursor = i;
            error[0] = '\0';
            continue;
        }

        if (key == TERM_KEY_IGNORE) {
            const char *seq = terminal_last_utf8();
            if (seq && seq[0]) {
                int slen = (int)strlen(seq);
                int cur_len = (int)strlen(input);
                if (cur_len + slen < CD_FIELD_BUFSZ - 1) {
                    memmove(input + cursor + slen, input + cursor,
                            (size_t)(cur_len - cursor + 1));
                    memcpy(input + cursor, seq, (size_t)slen);
                    cursor += slen;
                    error[0] = '\0';
                }
            }
        }
    }
}

/* Post-compose review screen.
 * Runs its own key loop; attach_paths/attach_count are modified in place.
 * Returns PCR_SEND, PCR_CANCEL, PCR_EDIT_BODY, or PCR_EDIT_HEADERS. */
static int post_compose_review(const char *from, const char *to,
                                const char *cc, const char *bcc,
                                const char *subject, const char *body,
                                char attach_paths[CD_MAX_ATTACH][CD_ATTACH_PATH_SZ],
                                int *attach_count)
{
    char status_msg[256] = "";

    RAII_TERM_RAW TermRawState *raw = terminal_raw_enter();
    if (!raw) return PCR_CANCEL;

    for (;;) {
        pcr_render(from, to, cc, bcc, subject, body,
                   attach_paths, *attach_count, status_msg);
        status_msg[0] = '\0';

        TermKey key = terminal_read_key();
        int ch = (key == TERM_KEY_IGNORE) ? terminal_last_printable() : 0;

        if (ch == 's') {
            if (!to || !to[0]) {
                snprintf(status_msg, sizeof(status_msg),
                         "To: is empty — fill in recipient before sending.");
                continue;
            }
            return PCR_SEND;
        }

        if (ch == 'e') return PCR_EDIT_HEADERS;
        if (ch == 'b') return PCR_EDIT_BODY;

        if (ch == 'a') {
            pcr_pick_attach_raw(attach_paths, attach_count);
            continue;
        }

        if (ch == 'd') {
            if (*attach_count > 0) {
                (*attach_count)--;
                attach_paths[*attach_count][0] = '\0';
            }
            continue;
        }

        if (ch == 'q' || key == TERM_KEY_ESC || key == TERM_KEY_QUIT) {
            printf("\033[2J\033[H");
            printf("  Discard draft? [y/n] ");
            fflush(stdout);
            TermKey k2 = terminal_read_key();
            int ch2 = (k2 == TERM_KEY_IGNORE) ? terminal_last_printable() : 0;
            if (ch2 == 'y' || ch2 == 'Y') return PCR_CANCEL;
            /* 'n' or anything else → stay in review */
        }
    }
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
 * @brief Interactive compose form — opens $EDITOR on a draft temp file,
 *        then shows a post-compose review screen (US-85).
 *
 * Supports iterative editing: 's' sends, 'b' reopens editor, 'e' reopens
 * header dialog, 'a'/'d' manage attachments, 'q' discards.
 *
 * @param init_cd   Optional initial CDResult for attachments (may be NULL).
 * Returns 0 on successful send, -1 on abort or SMTP error.
 */
static int cmd_compose_interactive(Config *cfg,
                                   const char *prefill_to,
                                   const char *prefill_cc,
                                   const char *prefill_bcc,
                                   const char *prefill_subject,
                                   const char *reply_to_msg_id,
                                   const char *prefill_body,
                                   const CDResult *init_cd) {
    ensure_smtp_configured(cfg);

    /* Mutable header state */
    char from_buf[512]  = "";
    char to_buf[512]    = "";
    char cc_buf[512]    = "";
    char bcc_buf[512]   = "";
    char subj_buf[512]  = "";
    char msgid_buf[512] = "";

    const char *from = from_address(cfg);
    if (from) strncpy(from_buf, from, sizeof(from_buf) - 1);
    if (prefill_to)      strncpy(to_buf,    prefill_to,      sizeof(to_buf)    - 1);
    if (prefill_cc)      strncpy(cc_buf,    prefill_cc,      sizeof(cc_buf)    - 1);
    if (prefill_bcc)     strncpy(bcc_buf,   prefill_bcc,     sizeof(bcc_buf)   - 1);
    if (prefill_subject) strncpy(subj_buf,  prefill_subject, sizeof(subj_buf)  - 1);
    if (reply_to_msg_id) strncpy(msgid_buf, reply_to_msg_id, sizeof(msgid_buf) - 1);

    /* Attachment list from initial compose dialog */
    char attach_paths[CD_MAX_ATTACH][CD_ATTACH_PATH_SZ];
    memset(attach_paths, 0, sizeof(attach_paths));
    int attach_count = 0;
    if (init_cd && init_cd->attach_count > 0 && init_cd->attach_count <= CD_MAX_ATTACH) {
        memcpy(attach_paths, init_cd->attach_paths, sizeof(attach_paths));
        attach_count = init_cd->attach_count;
    }

    /* Create temp file for body editing */
    char tmppath[] = "/tmp/email-tui-XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) { perror("email-tui: mkstemp"); return -1; }
    close(fd);

    const char *editor = getenv("EDITOR");
    if (!editor || !editor[0]) editor = "vim";

    char *body = NULL;
    size_t body_len = 0;
    int first_write = 1;
    int need_edit = 1;
    int send_rc = -1;

    for (;;) {
        if (need_edit) {
            /* Write draft to tmppath */
            {
                FILE *f = fopen(tmppath, "w");
                if (!f) { free(body); unlink(tmppath); return -1; }
                const char *fb = from_buf[0] ? from_buf : (from ? from : "");
                fprintf(f, "From: %s\n",    fb);
                fprintf(f, "To: %s\n",      to_buf);
                if (cc_buf[0])    fprintf(f, "Cc: %s\n",          cc_buf);
                if (bcc_buf[0])   fprintf(f, "Bcc: %s\n",         bcc_buf);
                fprintf(f, "Subject: %s\n", subj_buf);
                if (msgid_buf[0]) fprintf(f, "In-Reply-To: %s\n", msgid_buf);
                fprintf(f, "\n");
                if (body && body_len > 0) {
                    fprintf(f, "%s", body);
                } else if (first_write && prefill_body && prefill_body[0]) {
                    fprintf(f, "%s", prefill_body);
                }
                first_write = 0;
                fclose(f);
            }

            /* Restore cursor, then launch $EDITOR */
            printf("\033[?25h");
            fflush(stdout);
            fflush(stderr);
            {
                char cmd_buf[1024];
                snprintf(cmd_buf, sizeof(cmd_buf), "%s %s", editor, tmppath);
                int rc = system(cmd_buf);
                (void)rc;
            }

            /* Read back edited draft */
            free(body); body = NULL; body_len = 0;
            {
                FILE *rf = fopen(tmppath, "r");
                if (!rf) {
                    fprintf(stderr, "Error: Could not read draft after editing.\n");
                    unlink(tmppath);
                    return -1;
                }
                /* Reset headers — re-parse from what editor saved */
                from_buf[0] = to_buf[0] = cc_buf[0] = bcc_buf[0] = subj_buf[0] = '\0';
                char line[4096];
                int in_body = 0;
                while (fgets(line, sizeof(line), rf)) {
                    size_t ll = strlen(line);
                    while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
                        line[--ll] = '\0';
                    if (!in_body) {
                        if (ll == 0) { in_body = 1; continue; }
#define HCOPY(dst, src) do { \
    size_t _hl = strlen(src); \
    if (_hl >= sizeof(dst)) _hl = sizeof(dst) - 1; \
    memcpy((dst), (src), _hl); (dst)[_hl] = '\0'; } while (0)
                        if      (strncasecmp(line, "From: ",        6) == 0) { HCOPY(from_buf,  line+6);  }
                        else if (strncasecmp(line, "To: ",          4) == 0) { HCOPY(to_buf,    line+4);  }
                        else if (strncasecmp(line, "Cc: ",          4) == 0) { HCOPY(cc_buf,    line+4);  }
                        else if (strncasecmp(line, "Bcc: ",         5) == 0) { HCOPY(bcc_buf,   line+5);  }
                        else if (strncasecmp(line, "Subject: ",     9) == 0) { HCOPY(subj_buf,  line+9);  }
                        else if (strncasecmp(line, "In-Reply-To: ",13) == 0) { HCOPY(msgid_buf, line+13); }
#undef HCOPY
                    } else {
                        size_t add = ll + 1;
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
            need_edit = 0;
        }

        /* Show post-compose review screen */
        int action = post_compose_review(
            from_buf[0] ? from_buf : (from ? from : ""),
            to_buf, cc_buf, bcc_buf, subj_buf,
            body ? body : "",
            attach_paths, &attach_count);

        if (action == PCR_SEND) {
            if (!to_buf[0]) { send_rc = -1; break; }

            const char *from_send = from_buf[0] ? from_buf : from_address(cfg);
            const char *reply_id  = msgid_buf[0] ? msgid_buf : reply_to_msg_id;
            const char *body_str  = body ? body : "";

            const char *attach_ptrs[CD_MAX_ATTACH + 1];
            for (int i = 0; i < attach_count; i++)
                attach_ptrs[i] = attach_paths[i];
            attach_ptrs[attach_count] = NULL;

            ComposeParams p = {from_send, to_buf,
                               cc_buf[0]  ? cc_buf  : NULL,
                               bcc_buf[0] ? bcc_buf : NULL,
                               subj_buf, body_str, reply_id,
                               attach_count > 0 ? attach_ptrs : NULL,
                               attach_count};
            char *msg = NULL;
            size_t msg_len = 0;
            if (compose_build_message(&p, &msg, &msg_len) != 0) {
                fprintf(stderr, "Error: Failed to build message.\n");
                send_rc = -1;
                break;
            }
            printf("  Sending...\n");
            fflush(stdout);
            if (cfg->gmail_mode) {
                RAII_MAIL MailClient *mc = mail_client_connect((Config *)cfg);
                send_rc = mc ? mail_client_append(mc, NULL, msg, msg_len) : -1;
                if (send_rc == 0) printf("  Message sent.\n");
            } else {
                send_rc = smtp_send(cfg, from_send, to_buf, msg, msg_len);
                if (send_rc == 0) {
                    printf("  Message sent.\n");
                    fflush(stdout);
                    if (email_service_save_sent(cfg, msg, msg_len) == 0)
                        printf("  Saved locally.\n");
                    else
                        fprintf(stderr, "  Warning: could not save to Sent folder.\n");
                } else {
                    printf("  Send failed. Saving to Drafts...\n");
                    fflush(stdout);
                    if (email_service_save_draft(cfg, msg, msg_len) == 0)
                        printf("  Saved to Drafts (will retry on next sync).\n");
                    else
                        fprintf(stderr, "  Warning: could not save to local Drafts folder.\n");
                }
            }
            fflush(stdout);
            free(msg);
            break;
        }

        if (action == PCR_CANCEL) {
            printf("  Cancelled. Draft discarded.\n");
            fflush(stdout);
            send_rc = -1;
            break;
        }

        if (action == PCR_EDIT_BODY) {
            need_edit = 1;
            continue;
        }

        if (action == PCR_EDIT_HEADERS) {
            /* Reopen compose dialog pre-filled with current values + attachments */
            CDResult cdresult;
            memset(&cdresult, 0, sizeof(cdresult));
            memcpy(cdresult.to,   to_buf,   sizeof(cdresult.to));
            memcpy(cdresult.cc,   cc_buf,   sizeof(cdresult.cc));
            memcpy(cdresult.bcc,  bcc_buf,  sizeof(cdresult.bcc));
            memcpy(cdresult.subj, subj_buf, sizeof(cdresult.subj));
            memcpy(cdresult.attach_paths, attach_paths, sizeof(cdresult.attach_paths));
            cdresult.attach_count = attach_count;
            if (compose_dialog(cfg, "Edit Message", to_buf, cc_buf, subj_buf, &cdresult)) {
                memcpy(to_buf,   cdresult.to,   sizeof(to_buf));
                memcpy(cc_buf,   cdresult.cc,   sizeof(cc_buf));
                memcpy(bcc_buf,  cdresult.bcc,  sizeof(bcc_buf));
                memcpy(subj_buf, cdresult.subj, sizeof(subj_buf));
                memcpy(attach_paths, cdresult.attach_paths, sizeof(attach_paths));
                attach_count = cdresult.attach_count;
            }
            continue;
        }
    }

    unlink(tmppath);
    free(body);
    return send_rc;
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
        rc = cmd_compose_interactive(cfg, dlg.to, dlg.cc, dlg.bcc, dlg.subj, msg_id, quoted, &dlg);
        if (rc == 0) {
            /* Mark original message as Replied */
            Manifest *mf = manifest_load(folder);
            if (mf) {
                ManifestEntry *me = manifest_find(mf, uid);
                if (me) {
                    me->flags |= MSG_FLAG_ANSWERED;
                    manifest_save(folder, mf);
                }
                manifest_free(mf);
            }
        }
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
    int rc = cmd_compose_interactive(cfg, dlg.to, dlg.cc, dlg.bcc, dlg.subj, NULL, quoted, &dlg);
    if (rc == 0) {
        /* Mark original message as Forwarded */
        Manifest *mf = manifest_load(folder);
        if (mf) {
            ManifestEntry *me = manifest_find(mf, uid);
            if (me) {
                me->flags |= MSG_FLAG_FORWARDED;
                manifest_save(folder, mf);
            }
            manifest_free(mf);
        }
    }
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
        rc = cmd_compose_interactive(cfg, dlg.to, dlg.cc, dlg.bcc, dlg.subj, msg_id, quoted, &dlg);
        if (rc == 0) {
            /* Mark original message as Replied */
            Manifest *mf = manifest_load(folder);
            if (mf) {
                ManifestEntry *me = manifest_find(mf, uid);
                if (me) {
                    me->flags |= MSG_FLAG_ANSWERED;
                    manifest_save(folder, mf);
                }
                manifest_free(mf);
            }
        }
    }
    free(quoted);
    free(reply_to);
    free(subject);
    free(msg_id);
    return rc;
}

/* ── Rules editor ────────────────────────────────────────────────────── */

/* Number of then-add/rm-label slots shown in the edit form. */
#define RULES_FORM_LABEL_SLOTS 3

/* ── when expression list editor ─────────────────────────────────────── */

#define WHEN_MAX_ATOMS 128
#define WHEN_ATOM_MAX  512

typedef struct {
    char atoms[WHEN_MAX_ATOMS][WHEN_ATOM_MAX];
    int  count;
    int  cursor;
    int  scroll;
} WhenAtomList;

static void when_list_split(WhenAtomList *wl, const char *expr) {
    wl->count = 0; wl->cursor = 0; wl->scroll = 0;
    if (!expr || !expr[0]) return;
    const char *p = expr;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *next = strstr(p, " or ");
        size_t len = next ? (size_t)(next - p) : strlen(p);
        while (len > 0 && p[len-1] == ' ') len--;
        if (len > 0 && wl->count < WHEN_MAX_ATOMS) {
            if (len >= WHEN_ATOM_MAX) len = WHEN_ATOM_MAX - 1;
            memcpy(wl->atoms[wl->count], p, len);
            wl->atoms[wl->count][len] = '\0';
            wl->count++;
        }
        if (!next) break;
        p = next + 4;
    }
}

static void when_list_join(const WhenAtomList *wl, char *buf, size_t bufsz) {
    buf[0] = '\0';
    for (int i = 0; i < wl->count; i++) {
        if (i > 0) {
            size_t sl = strlen(buf);
            if (sl + 4 < bufsz) { memcpy(buf + sl, " or ", 4); buf[sl+4] = '\0'; }
        }
        size_t sl = strlen(buf), al = strlen(wl->atoms[i]);
        if (sl + al < bufsz) { memcpy(buf + sl, wl->atoms[i], al); buf[sl+al] = '\0'; }
    }
}

static void when_list_render(const WhenAtomList *wl, int vis_rows, const char *title) {
    printf("\033[2J\033[H");
    printf("%s\n", title);
    printf("j/k=move  Enter/e=edit  a=add  d=delete  q=confirm  ESC=cancel\n");
    if (wl->count == 0) {
        printf("\n  (empty — press 'a' to add a condition)\n");
        fflush(stdout);
        return;
    }
    for (int i = wl->scroll; i < wl->count && i < wl->scroll + vis_rows; i++) {
        if (i == wl->cursor)
            printf("  \033[7m%3d. %s\033[m\n", i + 1, wl->atoms[i]);
        else
            printf("       %3d. %s\n", i + 1, wl->atoms[i]);
    }
    fflush(stdout);
}

static void when_list_do_add(WhenAtomList *wl, int vis_rows, const char *title) {
    if (wl->count >= WHEN_MAX_ATOMS) return;
    char tmp[WHEN_ATOM_MAX] = {0};
    InputLine il = {0};
    input_line_init(&il, tmp, sizeof(tmp), NULL);
    int nrow = 3 + (wl->count - wl->scroll) + 1;
    int maxrow = terminal_rows() - 2;
    if (nrow > maxrow) nrow = maxrow;
    when_list_render(wl, vis_rows, title);
    if (!input_line_run(&il, nrow, "new:  ")) return;
    char *s = tmp;
    while (*s == ' ') s++;
    size_t sl = strlen(s);
    while (sl > 0 && s[sl-1] == ' ') sl--;
    s[sl] = '\0';
    if (sl == 0) return;
    snprintf(wl->atoms[wl->count], WHEN_ATOM_MAX, "%s", s);
    wl->cursor = wl->count;
    wl->count++;
    while (wl->cursor >= wl->scroll + vis_rows) wl->scroll++;
}

/* Returns 1 if confirmed (proceed to next form step), 0 if cancelled (abort form).
 * Called while the terminal is already in raw mode.
 * title: shown in header line (e.g. "Add new rule for user@host — conditions"). */
static int tui_when_list_edit(char *buf, size_t bufsz, const char *title) {
    WhenAtomList wl;
    when_list_split(&wl, buf);

    int vis_rows = terminal_rows() - 4;
    if (vis_rows < 3) vis_rows = 3;

    for (;;) {
        when_list_render(&wl, vis_rows, title);

        TermKey key = terminal_read_key();
        int ch = (key == TERM_KEY_IGNORE) ? terminal_last_printable() : 0;

        if (key == TERM_KEY_ESC || key == TERM_KEY_QUIT) {
            when_list_join(&wl, buf, bufsz);
            return 0;
        }
        if (ch == 'q') {
            when_list_join(&wl, buf, bufsz);
            return 1;
        }

        if (key == TERM_KEY_NEXT_LINE || ch == 'j') {
            if (wl.cursor < wl.count - 1) {
                wl.cursor++;
                if (wl.cursor >= wl.scroll + vis_rows) wl.scroll++;
            }
        } else if (key == TERM_KEY_PREV_LINE || ch == 'k') {
            if (wl.cursor > 0) {
                wl.cursor--;
                if (wl.cursor < wl.scroll) wl.scroll--;
            }
        } else if (key == TERM_KEY_ENTER || ch == 'e') {
            if (wl.count == 0) {
                when_list_do_add(&wl, vis_rows, title);
            } else {
                char tmp[WHEN_ATOM_MAX];
                snprintf(tmp, sizeof(tmp), "%s", wl.atoms[wl.cursor]);
                InputLine il = {0};
                input_line_init(&il, tmp, sizeof(tmp), tmp);
                int erow = 3 + (wl.cursor - wl.scroll) + 1;
                when_list_render(&wl, vis_rows, title);
                if (input_line_run(&il, erow, "edit: ")) {
                    char *s = tmp;
                    while (*s == ' ') s++;
                    size_t sl = strlen(s);
                    while (sl > 0 && s[sl-1] == ' ') sl--;
                    s[sl] = '\0';
                    if (sl > 0) snprintf(wl.atoms[wl.cursor], WHEN_ATOM_MAX, "%s", s);
                }
            }
        } else if (ch == 'a') {
            when_list_do_add(&wl, vis_rows, title);
        } else if (ch == 'd' && wl.count > 0) {
            for (int i = wl.cursor; i < wl.count - 1; i++)
                memcpy(wl.atoms[i], wl.atoms[i+1], WHEN_ATOM_MAX);
            wl.count--;
            if (wl.cursor >= wl.count && wl.cursor > 0) wl.cursor--;
            if (wl.scroll > wl.cursor) wl.scroll = wl.cursor;
        }
    }
}

/**
 * Full-screen form to create (prefill=NULL) or edit an existing rule.
 * Uses input_line_run for every field — proper UTF-8 and cursor movement.
 * All prefill strings are copied into local buffers immediately, so the
 * caller may free the source MailRules at any time after this returns.
 */
static void tui_rules_add_form(const char *account, const MailRule *prefill)
{
    char name_buf[256]   = {0};
    char when_buf[2048]  = {0};
    char add_bufs[RULES_FORM_LABEL_SLOTS][256];
    char rm_bufs[RULES_FORM_LABEL_SLOTS][256];
    char folder_buf[256] = {0};
    char fwd_buf[256]    = {0};
    memset(add_bufs, 0, sizeof(add_bufs));
    memset(rm_bufs,  0, sizeof(rm_bufs));

    if (prefill) {
        if (prefill->name)
            snprintf(name_buf, sizeof(name_buf), "%s", prefill->name);
        /* Prefill when: use existing when expression, or convert from flat fields */
        if (prefill->when && prefill->when[0]) {
            snprintf(when_buf, sizeof(when_buf), "%s", prefill->when);
        } else {
            char *w = when_from_flat(
                prefill->if_from, prefill->if_subject, prefill->if_to, prefill->if_label,
                prefill->if_not_from, prefill->if_not_subject, prefill->if_not_to,
                prefill->if_body, prefill->if_age_gt, prefill->if_age_lt);
            if (w) { snprintf(when_buf, sizeof(when_buf), "%s", w); free(w); }
        }
        for (int j = 0; j < prefill->then_add_count && j < RULES_FORM_LABEL_SLOTS; j++)
            if (prefill->then_add_label[j])
                snprintf(add_bufs[j], 256, "%s", prefill->then_add_label[j]);
        for (int j = 0; j < prefill->then_rm_count && j < RULES_FORM_LABEL_SLOTS; j++)
            if (prefill->then_rm_label[j])
                snprintf(rm_bufs[j], 256, "%s", prefill->then_rm_label[j]);
        if (prefill->then_move_folder)
            snprintf(folder_buf, sizeof(folder_buf), "%s", prefill->then_move_folder);
        if (prefill->then_forward_to)
            snprintf(fwd_buf, sizeof(fwd_buf), "%s", prefill->then_forward_to);
    }

    struct { const char *prompt; char *buf; size_t bufsz; int row; } fields[] = {
        { "Name:             ", name_buf,    sizeof(name_buf),   3 },
        { "add-label [1]:    ", add_bufs[0], 256,                4 },
        { "add-label [2]:    ", add_bufs[1], 256,                5 },
        { "add-label [3]:    ", add_bufs[2], 256,                6 },
        { "rm-label  [1]:    ", rm_bufs[0],  256,                7 },
        { "rm-label  [2]:    ", rm_bufs[1],  256,                8 },
        { "rm-label  [3]:    ", rm_bufs[2],  256,                9 },
        { "then-move-folder: ", folder_buf,  sizeof(folder_buf), 10 },
        { "then-forward-to:  ", fwd_buf,     sizeof(fwd_buf),    11 },
    };
    int nfields = (int)(sizeof(fields) / sizeof(fields[0]));

    /* Step 1: when expression list editor (full-screen, raw mode) */
    char when_title[320];
    if (prefill && prefill->name)
        snprintf(when_title, sizeof(when_title),
                 "%s rule \"%s\" for %s — conditions (step 1/2)",
                 "Edit", prefill->name, account ? account : "?");
    else
        snprintf(when_title, sizeof(when_title),
                 "Add new rule for %s — conditions (step 1/2)",
                 account ? account : "?");
    int cancelled = 0;
    {
        RAII_TERM_RAW TermRawState *_raw = terminal_raw_enter();
        if (!tui_when_list_edit(when_buf, sizeof(when_buf), when_title))
            cancelled = 1;
    }

    if (cancelled) return;

    /* Step 1: remaining fields */
    char confirm_char = 0;
    {
        RAII_TERM_RAW TermRawState *_raw = terminal_raw_enter();

        printf("\033[2J\033[H");
        printf("%s rule for %s — step 2/2: actions\n",
               prefill ? "Edit" : "Add new", account ? account : "?");
        printf("Enter=next field  ESC=cancel\n");
        for (int i = 0; i < nfields; i++)
            printf("\033[%d;1H\033[2K%s%s", fields[i].row, fields[i].prompt, fields[i].buf);
        fflush(stdout);

        for (int i = 0; i < nfields && !cancelled; i++) {
            InputLine il = {0};
            input_line_init(&il, fields[i].buf, fields[i].bufsz, fields[i].buf);
            if (!input_line_run(&il, fields[i].row, fields[i].prompt))
                cancelled = 1;
        }

        if (!cancelled && !name_buf[0]) {
            printf("\033[23;1H\033[2KRule name is required. Press any key...");
            fflush(stdout);
            char c; ssize_t _nc = read(STDIN_FILENO, &c, 1); (void)_nc; (void)c;
            cancelled = 1;
        }

        if (!cancelled) {
            printf("\033[23;1H\033[2KSave? (y/N) ");
            fflush(stdout);
            ssize_t cn = read(STDIN_FILENO, &confirm_char, 1); (void)cn;
            if (confirm_char != 'y' && confirm_char != 'Y') cancelled = 1;
        }
    }

    if (cancelled) return;

    MailRules *rules = mail_rules_load(account);
    if (!rules) {
        rules = calloc(1, sizeof(MailRules));
        if (!rules) { fprintf(stderr, "Error: out of memory.\n"); return; }
    }

    MailRule *mr = NULL;
    if (prefill && prefill->name) {
        for (int r = 0; r < rules->count; r++) {
            if (rules->rules[r].name &&
                strcmp(rules->rules[r].name, prefill->name) == 0) {
                mr = &rules->rules[r]; break;
            }
        }
    }
    if (!mr) {
        if (rules->count >= rules->cap) {
            int nc = rules->cap ? rules->cap * 2 : 8;
            MailRule *tmp = realloc(rules->rules, (size_t)nc * sizeof(MailRule));
            if (!tmp) {
                mail_rules_free(rules);
                fprintf(stderr, "Error: out of memory.\n");
                return;
            }
            rules->rules = tmp;
            rules->cap   = nc;
        }
        mr = &rules->rules[rules->count++];
        memset(mr, 0, sizeof(*mr));
    } else {
        free(mr->name); free(mr->when);
        free(mr->if_from); free(mr->if_subject); free(mr->if_to); free(mr->if_label);
        free(mr->if_not_from); free(mr->if_not_subject); free(mr->if_not_to);
        free(mr->if_body); free(mr->then_move_folder); free(mr->then_forward_to);
        for (int j = 0; j < mr->then_add_count; j++) free(mr->then_add_label[j]);
        for (int j = 0; j < mr->then_rm_count;  j++) free(mr->then_rm_label[j]);
        memset(mr, 0, sizeof(*mr));
    }

    mr->name             = strdup(name_buf);
    mr->when             = when_buf[0] ? strdup(when_buf) : NULL;
    mr->then_move_folder = folder_buf[0] ? strdup(folder_buf) : NULL;
    mr->then_forward_to  = fwd_buf[0]    ? strdup(fwd_buf)    : NULL;
    for (int j = 0; j < RULES_FORM_LABEL_SLOTS && add_bufs[j][0]; j++)
        mr->then_add_label[mr->then_add_count++] = strdup(add_bufs[j]);
    for (int j = 0; j < RULES_FORM_LABEL_SLOTS && rm_bufs[j][0]; j++)
        mr->then_rm_label[mr->then_rm_count++]   = strdup(rm_bufs[j]);

    if (mail_rules_save(account, rules) != 0)
        fprintf(stderr, "Error: failed to save rules.\n");
    mail_rules_free(rules);
}

static void rules_free_at(MailRules *rules, int idx)
{
    MailRule *rr = &rules->rules[idx];
    free(rr->name); free(rr->when);
    free(rr->if_from); free(rr->if_subject); free(rr->if_to); free(rr->if_label);
    free(rr->if_not_from); free(rr->if_not_subject); free(rr->if_not_to);
    free(rr->if_body); free(rr->then_move_folder); free(rr->then_forward_to);
    for (int j = 0; j < rr->then_add_count; j++) free(rr->then_add_label[j]);
    for (int j = 0; j < rr->then_rm_count;  j++) free(rr->then_rm_label[j]);
    for (int r = idx; r < rules->count - 1; r++)
        rules->rules[r] = rules->rules[r + 1];
    rules->count--;
}

/**
 * Detail view for a single rule.
 * Keys: e=edit  d=delete  ESC/q=back
 */
static void tui_rules_detail(const char *account, int idx)
{
    for (;;) {
        MailRules *rules = mail_rules_load(account);
        if (!rules || idx < 0 || idx >= rules->count) {
            mail_rules_free(rules);
            break;
        }
        const MailRule *r = &rules->rules[idx];

        printf("\033[2J\033[H");
        printf("Rule: %s\n", r->name ? r->name : "(unnamed)");
        printf("Account: %s\n\n", account ? account : "?");

        int row = 4;
        if (r->when && r->when[0])
            { printf("\033[%d;1H  when: %s", row, r->when); row++; }
        else
            { printf("\033[%d;1H  when: (no condition — matches all)", row); row++; }
        row++;
        for (int j = 0; j < r->then_add_count; j++)
            if (r->then_add_label[j])
                { printf("\033[%d;1H  then-add-label:   %s", row++, r->then_add_label[j]); }
        for (int j = 0; j < r->then_rm_count; j++)
            if (r->then_rm_label[j])
                { printf("\033[%d;1H  then-rm-label:    %s", row++, r->then_rm_label[j]); }
        if (r->then_move_folder)
            { printf("\033[%d;1H  then-move-folder: %s", row++, r->then_move_folder); }
        if (r->then_forward_to)
            { printf("\033[%d;1H  then-forward-to:  %s", row++, r->then_forward_to); }

        int trows = terminal_rows();
        int footer = (trows > row + 1) ? trows : row + 1;
        printf("\033[%d;1H  e=edit  d=delete  ESC/q=back", footer);
        fflush(stdout);

        TermKey key;
        int ch;
        char del_confirm = 0;
        {
            RAII_TERM_RAW TermRawState *_raw = terminal_raw_enter();
            key = terminal_read_key();
            ch  = terminal_last_printable();
            if (ch == 'd') {
                printf("\033[%d;1H\033[2K  Delete \"%s\"? (y/N) ",
                       footer, r->name ? r->name : "?");
                fflush(stdout);
                ssize_t _nd = read(STDIN_FILENO, &del_confirm, 1); (void)_nd;
            }
        }

        if (key == TERM_KEY_ESC || key == TERM_KEY_QUIT || ch == 'q') {
            mail_rules_free(rules);
            break;
        }

        if (ch == 'e') {
            tui_rules_add_form(account, r);
            mail_rules_free(rules);
            continue;
        }

        if (ch == 'd') {
            if (del_confirm == 'y' || del_confirm == 'Y') {
                rules_free_at(rules, idx);
                mail_rules_save(account, rules);
            }
            mail_rules_free(rules);
            break;
        }

        mail_rules_free(rules);
    }
}

/**
 * Full-screen rules list with cursor navigation.
 * Keys: j/k/↑/↓=navigate  Enter=open detail  a=add  d=delete  ESC/q=back
 */
static void tui_rules_editor(const Config *cfg)
{
    if (!cfg || !cfg->user) return;
    const char *account = cfg->user;
    int cursor = 0;
    int scroll = 0;

    for (;;) {
        MailRules *rules = mail_rules_load(account);
        int count = rules ? rules->count : 0;

        if (cursor >= count) cursor = count > 0 ? count - 1 : 0;
        if (cursor < 0) cursor = 0;

        int rows = terminal_rows();
        if (rows < 5) rows = 24;
        int list_start = 3;
        int list_rows  = rows - 3;
        if (list_rows < 1) list_rows = 1;

        if (cursor < scroll) scroll = cursor;
        if (cursor >= scroll + list_rows) scroll = cursor - list_rows + 1;

        printf("\033[2J\033[H");
        printf("Rules for %s (%d rule%s)\n\n",
               account, count, count == 1 ? "" : "s");

        for (int i = 0; i < list_rows; i++) {
            int ri = scroll + i;
            printf("\033[%d;1H\033[2K", list_start + i);
            if (ri < count) {
                const char *name = rules->rules[ri].name
                                 ? rules->rules[ri].name : "(unnamed)";
                if (ri == cursor)
                    printf("  \033[7m %-56s \033[m", name);
                else
                    printf("    %-56s", name);
            }
        }
        if (count == 0)
            printf("\033[3;1H\033[2K  (no rules -- press 'a' to add one)");

        printf("\033[%d;1H\033[2K"
               "  j/k=navigate  Enter=open  a=add  d=delete  ESC/q=back",
               rows);
        fflush(stdout);

        TermKey key;
        int ch;
        char del_confirm = 0;
        {
            RAII_TERM_RAW TermRawState *_raw = terminal_raw_enter();
            key = terminal_read_key();
            ch  = terminal_last_printable();
            if (ch == 'd' && count > 0) {
                const char *rname = rules->rules[cursor].name
                                  ? rules->rules[cursor].name : "?";
                printf("\033[%d;1H\033[2K  Delete \"%s\"? (y/N) ", rows, rname);
                fflush(stdout);
                ssize_t _nd = read(STDIN_FILENO, &del_confirm, 1); (void)_nd;
            }
        }

        if (key == TERM_KEY_ESC || key == TERM_KEY_QUIT || ch == 'q') {
            mail_rules_free(rules);
            break;
        }

        if (key == TERM_KEY_NEXT_LINE || ch == 'j') {
            if (cursor < count - 1) cursor++;
            mail_rules_free(rules);
            continue;
        }

        if (key == TERM_KEY_PREV_LINE || ch == 'k') {
            if (cursor > 0) cursor--;
            mail_rules_free(rules);
            continue;
        }

        if (key == TERM_KEY_ENTER && count > 0) {
            int saved = cursor;
            mail_rules_free(rules);
            tui_rules_detail(account, saved);
            cursor = saved;
            continue;
        }

        if (ch == 'a') {
            mail_rules_free(rules);
            tui_rules_add_form(account, NULL);
            continue;
        }

        if (ch == 'd' && count > 0) {
            if (del_confirm == 'y' || del_confirm == 'Y') {
                rules_free_at(rules, cursor);
                mail_rules_save(account, rules);
                if (cursor >= rules->count && cursor > 0) cursor--;
            }
            mail_rules_free(rules);
            continue;
        }

        mail_rules_free(rules);
    }
}

/* ── Subcommand help pages ───────────────────────────────────────────── */

static void help_compose(void) {
    printf(
        "Usage: email-tui compose\n"
        "\n"
        "Open $EDITOR to compose a new message.\n"
        "\n"
        "After saving and exiting the editor, you will be asked to confirm\n"
        "before the message is sent.\n"
        "\n"
        "Draft file headers:\n"
        "  From:    sender address\n"
        "  To:      recipient (required)\n"
        "  Subject: message subject\n"
        "  Cc:, Bcc: carbon copy recipients\n"
        "\n"
        "Options:\n"
        "  --help, -h   Show this help message\n"
    );
}

static void help_send(void) {
    printf(
        "Usage: email-tui send --to <addr> --subject <text> --body <text>\n"
        "\n"
        "Sends a message non-interactively (batch/scriptable mode).\n"
        "\n"
        "Options:\n"
        "  --to <addr>       Recipient email address (required)\n"
        "  --subject <text>  Subject line (required)\n"
        "  --body <text>     Message body text (required)\n"
        "\n"
        "SMTP settings must be configured (run 'email-cli config smtp').\n"
        "\n"
        "Options:\n"
        "  --help, -h   Show this help message\n"
    );
}

static void help_reply(void) {
    printf(
        "Usage: email-tui reply <uid>\n"
        "\n"
        "Reply to a message identified by its UID.\n"
        "\n"
        "Arguments:\n"
        "  <uid>  Message UID to reply to (required)\n"
        "\n"
        "Opens $EDITOR with a quoted draft. After saving and exiting the editor,\n"
        "you will be asked to confirm before the reply is sent.\n"
        "\n"
        "Options:\n"
        "  --help, -h   Show this help message\n"
    );
}

/* ── Entry point ─────────────────────────────────────────────────────── */

#ifndef EMAIL_CLI_VERSION
#define EMAIL_CLI_VERSION "unknown"
#endif

int main(int argc, char *argv[]) {
    /* 0. Set locale so wcwidth() and mbsrtowcs() work correctly for
     *    multi-byte UTF-8 characters (needed for column-width calculations). */
    setlocale(LC_ALL, "");

    /* Ensure SIGTERM causes a clean exit so atexit() handlers run (e.g. gcov). */
    signal(SIGTERM, exit);

    /* Parse command line: handle --help/--version and detect subcommands */
    const char *subcmd = NULL;  /* "compose" | "send" | "reply" | NULL */
    int    sub_i       = 0;     /* index in argv[] where subcommand starts */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("email-tui %s\n", EMAIL_CLI_VERSION);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            help();
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--batch") == 0) {
            continue;
        }
        if (strcmp(argv[i], "compose") == 0 ||
            strcmp(argv[i], "send")    == 0 ||
            strcmp(argv[i], "reply")   == 0) {
            subcmd = argv[i];
            sub_i  = i;
            break;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option '%s'.\nRun 'email-tui --help' for usage.\n",
                    argv[i]);
            return EXIT_FAILURE;
        }
        fprintf(stderr, "email-tui does not accept arguments.\n"
                        "Run 'email-tui --help' for usage.\n");
        return EXIT_FAILURE;
    }

    /* Handle subcommand --help early (before loading config) */
    if (subcmd) {
        for (int i = sub_i + 1; i < argc; i++) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
                if (strcmp(subcmd, "compose") == 0) help_compose();
                else if (strcmp(subcmd, "send") == 0) help_send();
                else if (strcmp(subcmd, "reply") == 0) help_reply();
                return EXIT_SUCCESS;
            }
        }
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

    /* 4b. Subcommand dispatch (compose / send / reply) — exits here; TUI not started */
    if (subcmd) {
        int rc = EXIT_FAILURE;

        if (strcmp(subcmd, "compose") == 0) {
            int cr = cmd_compose_interactive(cfg, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
            rc = (cr == 0) ? EXIT_SUCCESS : EXIT_FAILURE;

        } else if (strcmp(subcmd, "send") == 0) {
            const char *to = NULL, *subject = NULL, *body_text = NULL;
            const char *attach_arr[8];
            int attach_count = 0;
            for (int i = sub_i + 1; i < argc - 1; i++) {
                if (strcmp(argv[i], "--to")      == 0) to         = argv[++i];
                else if (strcmp(argv[i], "--subject") == 0) subject = argv[++i];
                else if (strcmp(argv[i], "--body")    == 0) body_text = argv[++i];
                else if (strcmp(argv[i], "--batch")   == 0) (void)0;
                else if (strcmp(argv[i], "--attach")  == 0 && i + 1 < argc - 1 &&
                         attach_count < 8) attach_arr[attach_count++] = argv[++i];
            }
            if (!to) {
                fprintf(stderr, "Error: --to is required for send.\n"
                                "Run 'email-tui send --help' for usage.\n");
            } else {
                const char *from = from_address(cfg);
                ComposeParams p = {from, to, NULL, NULL,
                                   subject   ? subject   : "",
                                   body_text ? body_text : "", NULL,
                                   attach_count > 0 ? attach_arr : NULL,
                                   attach_count};
                char *msg = NULL; size_t msg_len = 0;
                if (compose_build_message(&p, &msg, &msg_len) != 0) {
                    fprintf(stderr, "Error: Failed to build message.\n");
                } else {
                    int sr = smtp_send(cfg, from, to, msg, msg_len);
                    free(msg);
                    if (sr == 0) {
                        printf("Message sent.\n");
                        rc = EXIT_SUCCESS;
                    } else {
                        fprintf(stderr, "Error: Send failed.\n");
                    }
                }
            }

        } else if (strcmp(subcmd, "reply") == 0) {
            const char *uid = NULL;
            for (int i = sub_i + 1; i < argc; i++) {
                if (argv[i][0] != '-') { uid = argv[i]; break; }
            }
            if (!uid) {
                fprintf(stderr, "Error: reply requires a UID argument.\n"
                                "Run 'email-tui reply --help' for usage.\n");
            } else {
                const char *folder = cfg->folder ? cfg->folder : "INBOX";
                int rr = cmd_reply(cfg, uid, folder);
                rc = (rr == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
            }
        }

        config_free(cfg);
        logger_log(LOG_INFO, "--- email-tui subcommand finished ---");
        logger_close();
        return rc;
    }

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

    const char *acc_flash = NULL;
    for (;;) {  /* outer: accounts screen */
        Config *sel_cfg = NULL;

        {
            int acc = email_service_account_interactive(&sel_cfg, &account_cursor,
                                                        acc_flash);
            acc_flash = NULL;
            if (acc == 0) break;  /* ESC/quit */
            if (acc == 3) {
                /* 'n' → add new account via wizard */
                Config *new_cfg = setup_wizard_run();
                if (new_cfg) {
                    if (config_save_account(new_cfg) != 0)
                        fprintf(stderr, "Warning: Failed to save new account.\n");
                    config_free(new_cfg);
                } else {
                    acc_flash = "  Wizard aborted.";
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
                                  : "__unread__";

        /* Open folder/label browser — user picks which folder/label to enter.
         * Loop so that 'c' (compose) re-enters the browser after the dialog. */
        int go_up = 0;
        char *tui_folder = NULL;
        const char *browser_init = init_folder;
        for (;;) {
            tui_folder = sel_cfg->gmail_mode
                ? email_service_list_labels_interactive(sel_cfg, browser_init, &go_up)
                : email_service_list_folders_interactive(sel_cfg, browser_init, &go_up);
            if (!tui_folder || strcmp(tui_folder, "__compose__") != 0) break;
            free(tui_folder); tui_folder = NULL;
            CDResult dlg = {0};
            if (compose_dialog(sel_cfg, "New Message", NULL, NULL, NULL, &dlg)) {
                cmd_compose_interactive(sel_cfg, dlg.to, dlg.cc, dlg.bcc,
                                        dlg.subj, NULL, NULL, &dlg);
                printf("\n  [Press any key to return to inbox]\n");
                fflush(stdout);
                TermRawState *_r = terminal_raw_enter();
                char _c2; ssize_t _n2 = read(STDIN_FILENO, &_c2, 1); (void)_n2;
                terminal_raw_exit(&_r);
            }
        }
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
            EmailListOpts opts = {0, tui_folder, page_size, 0, 1, {0}, {0}};
            int ret = email_service_list(sel_cfg, &opts);
            if (ret == 1) {
                /* Backspace from message list → folder/label browser */
                char *sel = sel_cfg->gmail_mode
                    ? email_service_list_labels_interactive(sel_cfg, tui_folder, &go_up)
                    : email_service_list_folders_interactive(sel_cfg, tui_folder, &go_up);
                if (sel && strcmp(sel, "__compose__") == 0) {
                    free(sel);
                    CDResult dlg = {0};
                    if (compose_dialog(sel_cfg, "New Message", NULL, NULL, NULL, &dlg)) {
                        cmd_compose_interactive(sel_cfg, dlg.to, dlg.cc, dlg.bcc,
                                                dlg.subj, NULL, NULL, &dlg);
                        printf("\n  [Press any key to return to inbox]\n");
                        fflush(stdout);
                        TermRawState *_r = terminal_raw_enter();
                        char _c; ssize_t _n = read(STDIN_FILENO, &_c, 1); (void)_n;
                        terminal_raw_exit(&_r);
                    }
                    continue;
                }
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
                                                dlg.subj, NULL, NULL, &dlg);
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
                const char *af = opts.action_folder[0] ? opts.action_folder : tui_folder;
                cmd_reply(sel_cfg, opts.action_uid, af);
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
            } else if (ret == 7) {
                /* 'l' → rules editor */
                tui_rules_editor(sel_cfg);
                continue;
            } else if (ret == 5) {
                /* 'F' → forward current message */
                {
                    const char *af = opts.action_folder[0] ? opts.action_folder : tui_folder;
                    cmd_forward(sel_cfg, opts.action_uid, af);
                }
                printf("\n  [Press any key to return to inbox]\n");
                fflush(stdout);
                {
                    TermRawState *_r = terminal_raw_enter();
                    char _c; ssize_t _n = read(STDIN_FILENO, &_c, 1); (void)_n;
                    terminal_raw_exit(&_r);
                }
            } else if (ret == 6) {
                /* 'A' → reply-all */
                {
                    const char *af = opts.action_folder[0] ? opts.action_folder : tui_folder;
                    cmd_reply_all(sel_cfg, opts.action_uid, af);
                }
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
