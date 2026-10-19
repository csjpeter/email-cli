/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-24 */
/* Entry point of email-tui: the full-screen interactive client. */
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
#include "gmail_login.h"
#include "email_service.h"
#include "help_gmail.h"
#include "platform/terminal.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include "local_store.h"
#include "fs_util.h"
#include "mime_util.h"
#include "html_render.h"
#include "mail_account.h"
#include "input_line.h"
#include "tui_frame.h"
#include "key_table.h"
#include "status_line.h"
#include "body_editor.h"
#include "compose_draft.h"
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

/* The bottom status line of a screen of this program (EMAIL-111): the hints
 * that fit the width, in reverse video on the last row, a message in their
 * place when there is one.  The same widget as the lists' status line. */
static void screen_bar(const tui_statusline_hint *h, int nh, const char *help_key,
                       const char *msg) {
    int trows = terminal_rows(), tcols = terminal_cols();
    if (trows <= 0) trows = 24;
    if (tcols <= 0) tcols = 80;
    fflush(stdout);
    status_line_draw(trows, tcols, h, nh, help_key, 0, 0, msg);
}

/* The drop-down lists of the dialog are libtuikit tables drawn as an overlay
 * (EMAIL-110): contacts as address and name, files as the file name. */
typedef struct {
    const CDContact *contacts;
    const int       *match_idx;
    char (*files)[CD_ATTACH_PATH_SZ];
} CDDropdown;

static void cd_dd_cell(void *ctx, int index, int col, tui_table_cell *c) {
    const CDDropdown *dd = ctx;
    if (dd->contacts) {
        const CDContact *ct = &dd->contacts[dd->match_idx[index]];
        tui_table_cell_put(c, col == 0 ? ct->addr : ct->name,
                           col == 0 ? TUI_STYLE_ACCENT : TUI_STYLE_DEFAULT);
    } else {
        const char *fname = strrchr(dd->files[index], '/');
        fname = fname ? fname + 1 : dd->files[index];
        tui_table_cell_put(c, fname, TUI_STYLE_ACCENT);
    }
}

/** Draw the drop-down of @p n rows with row @p sel selected, from terminal row
 *  @p row and column @p col (1-based). */
static void cd_draw_dropdown(const CDDropdown *dd, int n, int sel, int row, int col, int tcols) {
    static const tui_table_col contact_cols[2] = {
        { NULL, 40, 0, TUI_ALIGN_LEFT, 0, 0, 0 },
        { NULL, 10, 1, TUI_ALIGN_LEFT, 0, 0, 0 },
    };
    static const tui_table_col file_cols[1] = {
        { NULL, 10, 1, TUI_ALIGN_LEFT, 0, 0, 0 },
    };
    tui_table_model model = { NULL, cd_dd_cell, (void *)dd };
    tui_table t;
    if (dd->contacts) tui_table_init(&t, contact_cols, 2, &model);
    else              tui_table_init(&t, file_cols, 1, &model);
    tui_table_set_count(&t, n);
    tui_table_select(&t, sel);
    if (!dd->contacts) t.lv.selected = -1;     /* files: Tab picks, no row is selected */
    int w = tcols - (col - 1);
    if (w < 10) w = 10;
    tui_frame_overlay_rows(&t, row, col, n, w);
    tui_table_free(&t);
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

    char title_line[256];
    snprintf(title_line, sizeof(title_line), "  %s", title);
    tui_frame_print_bar(1, title_line, tcols);

    printf("\033[2;1H\033[K");

    int row = 3;
    int active_row = 3;
    int dd_row = 0, dd_n = 0, dd_files = 0;    /* the drop-down, drawn last */

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
            /* Contact autocomplete dropdown: the rows are cleared here and
             * drawn as a table overlay at the end */
            dd_row = row;
            dd_n   = match_count;
            for (int m = 0; m < match_count; m++) {
                printf("\033[%d;1H\033[K", row);
                row++;
            }
        } else if (f == active && f == CD_FIELD_ATTACH && attach && attach->match_count > 0) {
            /* File browser dropdown */
            dd_row   = row;
            dd_n     = attach->match_count;
            dd_files = 1;
            for (int m = 0; m < attach->match_count; m++) {
                printf("\033[%d;1H\033[K", row);
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
    row++;

    printf("\033[%d;1H\033[J", row);
    fflush(stdout);
    if (active == CD_FIELD_ATTACH) {
        static const tui_statusline_hint h[] = {
            { "Tab", "complete", SL_PRIO_HIGH },
            { "Shift-Tab", "prev", SL_PRIO_LOW },
            { "Enter", "add", SL_PRIO_KEEP },
            { "d", "remove last", SL_PRIO_LOW },
            { "Enter (empty)", "ok", SL_PRIO_MID },
            { "Esc", "cancel", SL_PRIO_KEEP },
        };
        screen_bar(h, 6, NULL, NULL);
    } else {
        static const tui_statusline_hint h[] = {
            { "Tab", "next", SL_PRIO_HIGH },
            { "Shift-Tab", "prev", SL_PRIO_LOW },
            { ";", "add addr", SL_PRIO_MID },
            { "Enter", "ok", SL_PRIO_KEEP },
            { "Esc", "cancel", SL_PRIO_KEEP },
        };
        screen_bar(h, 5, NULL, NULL);
    }
    if (dd_n > 0) {
        CDDropdown dd = { dd_files ? NULL : contacts, match_idx,
                          dd_files ? (char (*)[CD_ATTACH_PATH_SZ])attach->matches : NULL };
        cd_draw_dropdown(&dd, dd_n, dd_files ? 0 : match_sel, dd_row, 13, tcols);
    }
    return active_row;
}

/** 1 when the compose dialog holds anything Esc would throw away. */
static int cd_has_text(char bufs[CD_NFIELDS][CD_FIELD_BUFSZ], int attach_count) {
    if (attach_count > 0) return 1;
    for (int f = 0; f < CD_NFIELDS; f++)
        if (bufs[f][0]) return 1;
    return 0;
}

/** Asks "Discard what was typed?" over the dialog; No is the default (EMAIL-175). */
static int cd_confirm_discard(void) {
    tui_dialog d;
    if (tui_dialog_confirm(&d, "Discard", "Discard what was typed?", 0) != 0) return 1;
    tui_frame_run_dialog(&d);
    int yes = d.state == TUI_DIALOG_OK && d.answer == 1;
    tui_dialog_free(&d);
    return yes;
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
                /* Esc asks before it throws typed text away (EMAIL-175);
                 * ctrl-c leaves at once. */
                if (key == TERM_KEY_ESC && cd_has_text(bufs, attach_state.count) &&
                    !cd_confirm_discard())
                    continue;
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

/** Number of characters (not bytes) of the UTF-8 text s[0..n). */
static int pcr_chars(const char *s, size_t n)
{
    int c = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

/**
 * Print an address-list header line of the review screen.  A line that does
 * not fit is broken between addresses (after the ";" or "," that ends one,
 * outside < >), never inside an address; a continuation line is indented to
 * the value column (EMAIL-107).  One address wider than the line is left to
 * the terminal.
 */
static void pcr_print_addresses(const char *label, const char *value, int tcols)
{
    const int indent = 11;                      /* "  Label:    ", as the Subject line */
    int room = tcols - indent;
    if (room < 10) room = 10;
    printf("  \033[2m%s\033[0m", label);
    for (int pad = pcr_chars(label, strlen(label)) + 2; pad < indent; pad++) putchar(' ');
    if (!value) value = "";

    int used = 0, first = 1;
    const char *p = value;
    while (*p) {
        const char *e = p;                      /* end of this address */
        int angle = 0;
        while (*e) {
            if (*e == '<') angle = 1;
            else if (*e == '>') angle = 0;
            else if (!angle && (*e == ';' || *e == ',')) { e++; break; }
            e++;
        }
        const char *next = e;                   /* skip the blanks after the separator */
        while (*next == ' ' || *next == '\t') next++;
        const char *te = e;                     /* item without trailing blanks */
        while (te > p && (te[-1] == ' ' || te[-1] == '\t')) te--;
        int w = pcr_chars(p, (size_t)(te - p));
        if (!first && used + 1 + w > room) {
            printf("\n%*s", indent, "");
            used = 0;
            first = 1;
        }
        if (!first) { putchar(' '); used++; }
        fwrite(p, 1, (size_t)(te - p), stdout);
        used += w;
        first = 0;
        p = next;
    }
    putchar('\n');
}

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
    tui_frame_print_bar(1, "  Review: Message", tcols);
    printf("\n\n");

    printf("  \033[2mFrom:\033[0m    %s\n", from   ? from    : "");
    pcr_print_addresses("To:", to, tcols);
    if (cc  && cc[0])  pcr_print_addresses("Cc:", cc, tcols);
    if (bcc && bcc[0]) pcr_print_addresses("Bcc:", bcc, tcols);
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

    static const tui_statusline_hint h[] = {
        { ":", "actions (send)", SL_PRIO_KEEP },
        { "e", "edit headers", SL_PRIO_HIGH },
        { "b", "edit body", SL_PRIO_HIGH },
        { "a", "add attachment", SL_PRIO_MID },
        { "?", "help", SL_PRIO_MID },
        { "q", "discard", SL_PRIO_KEEP },
    };
    screen_bar(h, 6, NULL, NULL);
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
        int dd_row = row;
        for (int m = 0; m < file_match_count; m++)
            printf("\033[%d;1H\033[K", row++);          /* the table overlay fills them */
        if (error[0]) {
            printf("\033[%d;1H\033[K  \033[31m%s\033[0m", row++, error);
        }
        printf("\033[%d;1H\033[K", row);
        (void)tcols;
        fflush(stdout);
        {
            static const tui_statusline_hint h[] = {
                { "Tab", "complete", SL_PRIO_HIGH },
                { "Enter", "add", SL_PRIO_KEEP },
                { "Esc", "cancel", SL_PRIO_KEEP },
            };
            screen_bar(h, 3, NULL, NULL);
        }
        if (file_match_count > 0) {
            CDDropdown dd = { NULL, NULL, file_matches };
            cd_draw_dropdown(&dd, file_match_count, 0, dd_row, 3, tcols);
        }
        printf("\033[3;%dH", 9 + cursor);   /* position cursor on input field */
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

/* Ask which attachment to remove: the numbers on the screen, 1..count.
 * Returns the index, or -1 when cancelled (Esc or any other key). */
static int pcr_ask_attachment(char attach_paths[CD_MAX_ATTACH][CD_ATTACH_PATH_SZ],
                              int count)
{
    (void)attach_paths;
    printf("\033[2J\033[H");
    for (int i = 0; i < count; i++) {
        const char *fname = strrchr(attach_paths[i], '/');
        fname = fname ? fname + 1 : attach_paths[i];
        printf("  %d  %s\n", i + 1, fname);
    }
    printf("\n  Remove which attachment? [1-%d, Esc cancels] ", count);
    fflush(stdout);
    TermKey k = terminal_read_key();
    int ch = (k == TERM_KEY_IGNORE) ? terminal_last_printable() : 0;
    if (ch >= '1' && ch < '1' + count) return ch - '1';
    return -1;
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

        if (ch == ':') {
            /* Sending, and removing an attachment, are in the action menu;
             * sending asks first, naming the recipients (EMAIL-175). */
            const char *tt[EA_COUNT] = { NULL };
            tt[EA_SEND] = (to && to[0]) ? to : "(nobody)";
            EmailAction act = ekeys_menu(EKV_REVIEW, tt);
            if (act == EA_SEND) {
                if (!to || !to[0]) {
                    snprintf(status_msg, sizeof(status_msg),
                             "To: is empty — fill in recipient before sending.");
                    continue;
                }
                return PCR_SEND;
            }
            if (act == EA_UNATTACH && *attach_count > 0) {
                int which = *attach_count - 1;
                if (*attach_count > 1) {
                    which = pcr_ask_attachment(attach_paths, *attach_count);
                    if (which < 0) continue;
                }
                for (int i = which; i < *attach_count - 1; i++)
                    memcpy(attach_paths[i], attach_paths[i + 1], CD_ATTACH_PATH_SZ);
                (*attach_count)--;
                attach_paths[*attach_count][0] = '\0';
            }
            continue;
        }

        if (ch == '?') {
            ekeys_help(EKV_REVIEW, "Review shortcuts");
            continue;
        }

        if (ch == 'e') return PCR_EDIT_HEADERS;
        if (ch == 'b') return PCR_EDIT_BODY;

        if (ch == 'a') {
            pcr_pick_attach_raw(attach_paths, attach_count);
            continue;
        }

        if (ch == 'q' || key == TERM_KEY_ESC || key == TERM_KEY_QUIT) {
            printf("\033[2J\033[H");
            {
                int nl = 0;
                for (const char *p = body; p && *p; p++) if (*p == '\n') nl++;
                if (body && body[0] && body[strlen(body) - 1] != '\n') nl++;
                printf("\n  Discard draft? To: %s, Subject: %s, %d line(s), %d attachment(s) [y/n] ",
                       to ? to : "", subject ? subject : "", nl, *attach_count);
            }
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
    } else {
        fprintf(stderr, "  Cancelled — SMTP was left unconfigured.\n\n");
    }
}

/**
 * @brief Build from address from config (SMTP user or IMAP user).
 * Returns a pointer into cfg — do NOT free.
 */
static const char *from_address(const Config *cfg) {
    return cfg->smtp_user ? cfg->smtp_user : cfg->user;
}

/* The compose state the autosave writes out (EMAIL-87). */
typedef struct {
    const char *to, *cc, *bcc, *subj, *msgid;
    char (*attach)[CD_ATTACH_PATH_SZ];
    const int *attach_count;
} DraftCtx;

static void draft_autosave_cb(const char *text, void *ctx) {
    const DraftCtx *dc = ctx;
    ComposeDraft d;
    memset(&d, 0, sizeof(d));
    snprintf(d.to, sizeof(d.to), "%s", dc->to);
    snprintf(d.cc, sizeof(d.cc), "%s", dc->cc);
    snprintf(d.bcc, sizeof(d.bcc), "%s", dc->bcc);
    snprintf(d.subject, sizeof(d.subject), "%s", dc->subj);
    snprintf(d.in_reply_to, sizeof(d.in_reply_to), "%s", dc->msgid);
    for (int i = 0; i < *dc->attach_count && i < 16; i++) {
        snprintf(d.attach[i], sizeof(d.attach[i]), "%s", dc->attach[i]);
        d.attach_count++;
    }
    d.body = (char *)text;
    compose_draft_save(&d);
}

/**
 * @brief Interactive compose form — the built-in body editor (tui_textedit,
 *        autosaved draft, EMAIL-87), then the post-compose review screen
 *        (US-85).
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

    /* An unsent draft left by an earlier compose (EMAIL-87): offer it. */
    char *body = NULL;
    int first_write = 1;
    int need_edit = 1;
    int send_rc = -1;
    {
        ComposeDraft old;
        if (compose_draft_load(&old) == 0) {
            printf("\033[2J\033[H\n  Unsent draft found (To: %s, Subject: %s).\n"
                   "  Resume it? [y/N] ", old.to, old.subject);
            fflush(stdout);
            RAII_TERM_RAW TermRawState *rawq = terminal_raw_enter();
            TermKey qk = terminal_read_key();
            int qc = (qk == TERM_KEY_IGNORE) ? terminal_last_printable() : 0;
            if (qc == 'y' || qc == 'Y') {
                snprintf(to_buf, sizeof(to_buf), "%s", old.to);
                snprintf(cc_buf, sizeof(cc_buf), "%s", old.cc);
                snprintf(bcc_buf, sizeof(bcc_buf), "%s", old.bcc);
                snprintf(subj_buf, sizeof(subj_buf), "%s", old.subject);
                snprintf(msgid_buf, sizeof(msgid_buf), "%s", old.in_reply_to);
                attach_count = 0;
                for (int i = 0; i < old.attach_count && i < CD_MAX_ATTACH; i++) {
                    snprintf(attach_paths[i], CD_ATTACH_PATH_SZ, "%s", old.attach[i]);
                    attach_count++;
                }
                body = old.body ? strdup(old.body) : NULL;
                first_write = 0;
            }
            compose_draft_free(&old);
        }
    }

    for (;;) {
        if (need_edit) {
            if (first_write && !body && prefill_body && prefill_body[0])
                body = strdup(prefill_body);
            first_write = 0;
            DraftCtx dc = { to_buf, cc_buf, bcc_buf, subj_buf, msgid_buf,
                            attach_paths, &attach_count };
            char hdr[1100];
            snprintf(hdr, sizeof(hdr), "  Message body — To: %s  Subject: %s",
                     to_buf, subj_buf);
            if (body_editor_run(&body, hdr, draft_autosave_cb, &dc) != 0) {
                fprintf(stderr, "Error: Could not open the editor.\n");
                free(body);
                return -1;
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

            emc_message p = {.from = from_send, .to = to_buf,
                             .cc  = cc_buf[0]  ? cc_buf  : NULL,
                             .bcc = bcc_buf[0] ? bcc_buf : NULL,
                             .subject = subj_buf, .body = body_str,
                             .in_reply_to = reply_id,
                             .attachments = attach_count > 0 ? attach_ptrs : NULL,
                             .attachment_count = attach_count};
            char *msg = NULL;
            size_t msg_len = 0;
            if (mail_compose(&p, &msg, &msg_len) != 0) {
                fprintf(stderr, "Error: Failed to build message.\n");
                send_rc = -1;
                break;
            }
            printf("  Sending...\n");
            fflush(stdout);
            if (cfg->gmail_mode) {
                send_rc = mail_send_wire(cfg, from_send, to_buf, msg, msg_len);
                if (send_rc == 0) {
                    compose_draft_remove();
                    printf("  Message sent.\n");
                }
            } else {
                send_rc = mail_send_wire(cfg, from_send, to_buf, msg, msg_len);
                if (send_rc == 0) {
                    compose_draft_remove();   /* before anything can be cut short */
                    printf("  Message sent.\n");
                    fflush(stdout);
                    if (email_service_save_sent(cfg, msg, msg_len) == 0)
                        printf("  Saved locally.\n");
                    else
                        fprintf(stderr, "  Warning: could not save to Sent folder.\n");
                } else {
                    printf("  Send failed. Saving to Drafts...\n");
                    fflush(stdout);
                    if (email_service_save_draft(cfg, msg, msg_len) == 0) {
                        compose_draft_remove();
                        printf("  Saved to Drafts (will retry on next sync).\n");
                    }
                    else
                        fprintf(stderr, "  Warning: could not save to local Drafts folder.\n");
                }
            }
            fflush(stdout);
            free(msg);
            break;
        }

        if (action == PCR_CANCEL) {
            compose_draft_remove();
            printf("\n  Cancelled. Draft discarded.\n");
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
    int rc = emc_reply_meta(raw, &reply_to, &subject, &msg_id) == EMC_OK ? 0 : -1;
    if (rc != 0) {
        free(raw);
        fprintf(stderr, "Error: Could not parse message headers.\n");
        return -1;
    }

    char *quoted = email_service_reply_quote(raw);
    free(raw);

    CDResult dlg = {0};
    if (!compose_dialog(cfg, "Reply", reply_to, NULL, subject, &dlg)) {
        rc = -1;
    } else {
        rc = cmd_compose_interactive(cfg, dlg.to, dlg.cc, dlg.bcc, dlg.subj, msg_id, quoted, &dlg);
        if (rc == 0)
            email_service_mark_answered(cfg, folder, uid);
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

    /* The composition is shared with `email-cli forward` (EMAIL-134). */
    char *fwd_subj = NULL, *quoted = NULL;
    int crc = email_service_forward_compose(raw, NULL, &fwd_subj, &quoted);
    free(raw);
    if (crc != 0) {
        fprintf(stderr, "Error: Could not compose the forward of UID %s.\n", uid);
        return -1;
    }
    char fwd_subject[512];
    snprintf(fwd_subject, sizeof(fwd_subject), "%s", fwd_subj);
    free(fwd_subj);

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
    int rc = emc_reply_meta(raw, &reply_to, &subject, &msg_id) == EMC_OK ? 0 : -1;
    if (rc != 0) {
        free(raw);
        fprintf(stderr, "Error: Could not parse message headers.\n");
        return -1;
    }

    /* Original To + Cc minus own address, joined with "; " */
    char *cc_all = email_service_reply_all_cc(raw, from_address(cfg));
    char cc_buf[1024] = "";
    if (cc_all) snprintf(cc_buf, sizeof(cc_buf), "%s", cc_all);
    free(cc_all);
    char *quoted = email_service_reply_quote(raw);
    free(raw);

    CDResult dlg = {0};
    if (!compose_dialog(cfg, "Reply All", reply_to, cc_buf[0] ? cc_buf : NULL, subject, &dlg)) {
        rc = -1;
    } else {
        rc = cmd_compose_interactive(cfg, dlg.to, dlg.cc, dlg.bcc, dlg.subj, msg_id, quoted, &dlg);
        if (rc == 0)
            email_service_mark_answered(cfg, folder, uid);
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

/** Find the next " or " that is not inside a double-quoted pattern. */
static const char *when_find_or(const char *p) {
    int in_quote = 0;
    for (; *p; p++) {
        if (in_quote) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') in_quote = 0;
        } else if (*p == '"') {
            in_quote = 1;
        } else if (strncmp(p, " or ", 4) == 0) {
            return p;
        }
    }
    return NULL;
}

static void when_list_split(WhenAtomList *wl, const char *expr) {
    wl->count = 0; wl->cursor = 0; wl->scroll = 0;
    if (!expr || !expr[0]) return;
    const char *p = expr;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *next = when_find_or(p);
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

/* Model of the condition list's table: "N. atom". */
static void when_list_cell(void *ctx, int i, int col, tui_table_cell *c) {
    const WhenAtomList *wl = ctx;
    char buf[WHEN_ATOM_MAX + 16];
    (void)col;
    snprintf(buf, sizeof(buf), "%3d. %s", i + 1, wl->atoms[i]);
    tui_table_cell_put(c, buf, TUI_STYLE_DEFAULT);
}

/* The refusal of the last condition typed (EMAIL-95): shown on the bottom
 * line once, with the next drawing. */
static char g_when_err[256];

/* 1 when @p atom is a well-formed condition; else 0 and g_when_err says why. */
static int when_atom_valid(const char *atom) {
    WhenNode *n = when_parse(atom);
    if (!n) {
        snprintf(g_when_err, sizeof(g_when_err),
                 "  Not a valid condition: %.150s  (e.g. from:*@example.org)", atom);
        return 0;
    }
    when_node_free(n);
    return 1;
}

static void when_list_render(const WhenAtomList *wl, int vis_rows, const char *title) {
    (void)vis_rows;
    static const tui_table_col wcols[] = {
        { NULL, 20, 1, TUI_ALIGN_LEFT, 0, 0, 0 },
    };
    tui_table_model wm = { NULL, when_list_cell, (void *)wl };
    tui_table wt;
    tui_table_init(&wt, wcols, 1, &wm);
    wt.flags |= TUI_TABLE_NO_HEADER;
    tui_table_set_count(&wt, wl->count);
    tui_table_select(&wt, wl->cursor);
    wt.lv.scroll_top = wl->scroll;
    if (wl->count == 0)
        tui_table_set_message(&wt, "(empty \xe2\x80\x94 press 'a' to add a condition)",
                              TUI_STYLE_DEFAULT);
    int rows = terminal_rows(), cols = terminal_cols();
    if (rows <= 0) rows = 24;
    if (cols <= 0) cols = 80;
    tui_frame_draw_table(title, &wt, rows, cols, 0, NULL, NULL);
    {
        static const tui_statusline_hint h[] = {
            { "j/k", "move", SL_PRIO_MID },
            { "Enter/e", "edit", SL_PRIO_HIGH },
            { "a", "add", SL_PRIO_HIGH },
            { "d", "delete", SL_PRIO_HIGH },
            { "q", "confirm", SL_PRIO_KEEP },
            { "Esc", "cancel", SL_PRIO_KEEP },
        };
        screen_bar(h, 6, "?", g_when_err[0] ? g_when_err : NULL);
    }
    g_when_err[0] = '\0';
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
    if (!when_atom_valid(s)) return;
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
        if (ch == '?') {
            static const char *help[][2] = {
                { "j / \u2193",   "Next condition"                  },
                { "k / \u2191",   "Previous condition"              },
                { "Home / End",   "First / last condition"          },
                { "PgUp / PgDn",  "Page up / down"                  },
                { "Enter / e",    "Edit the condition"              },
                { "a",            "Add a condition"                 },
                { "d",            "Delete the condition"            },
                { "q",            "Done: go on to the actions"      },
                { "Esc",          "Cancel the rule"                 },
                { "?",            "Show this help"                  },
            };
            email_service_help_popup("Conditions shortcuts", help, 10);
            continue;
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
        } else if (key == TERM_KEY_HOME || key == TERM_KEY_END ||
                   key == TERM_KEY_PREV_PAGE || key == TERM_KEY_NEXT_PAGE) {
            /* Home/End jump to the edges, PgUp/PgDn move a page (EMAIL-164). */
            if (key == TERM_KEY_HOME) wl.cursor = 0;
            else if (key == TERM_KEY_END) wl.cursor = wl.count > 0 ? wl.count - 1 : 0;
            else if (key == TERM_KEY_PREV_PAGE) {
                wl.cursor -= vis_rows;
                if (wl.cursor < 0) wl.cursor = 0;
            } else {
                wl.cursor += vis_rows;
                if (wl.cursor > wl.count - 1) wl.cursor = wl.count > 0 ? wl.count - 1 : 0;
            }
            if (wl.cursor < wl.scroll) wl.scroll = wl.cursor;
            if (wl.cursor >= wl.scroll + vis_rows) wl.scroll = wl.cursor - vis_rows + 1;
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
                    if (sl > 0 && when_atom_valid(s))
                        snprintf(wl.atoms[wl.cursor], WHEN_ATOM_MAX, "%s", s);
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

        {
            char hdr[512];
            snprintf(hdr, sizeof(hdr), "%s rule for %s — step 2/2: actions",
                     prefill ? "Edit" : "Add new", account ? account : "?");
            printf("\033[2J\033[H");
            tui_frame_print_bar(1, hdr, terminal_cols());
        }
        {
            static const tui_statusline_hint h[] = {
                { "Enter", "next field", SL_PRIO_KEEP },
                { "Esc", "cancel", SL_PRIO_KEEP },
            };
            screen_bar(h, 2, NULL, NULL);
        }
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

        {
            char hdr[512];
            snprintf(hdr, sizeof(hdr), "Rule: %s", r->name ? r->name : "(unnamed)");
            printf("\033[2J\033[H");
            tui_frame_print_bar(1, hdr, terminal_cols());
            printf("\033[2;1HAccount: %s\n\n", account ? account : "?");
        }

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

        {
            static const tui_statusline_hint h[] = {
                { "e", "edit", SL_PRIO_HIGH },
                { "d", "delete", SL_PRIO_HIGH },
                { "Esc/q", "back", SL_PRIO_KEEP },
            };
            screen_bar(h, 3, "?", NULL);
        }

        TermKey key;
        int ch;
        char del_confirm = 0;
        {
            RAII_TERM_RAW TermRawState *_raw = terminal_raw_enter();
            key = terminal_read_key();
            ch  = terminal_last_printable();
            if (ch == 'd') {
                int footer = terminal_rows();
                if (footer <= 0) footer = 24;
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

        if (ch == '?') {
            static const char *help[][2] = {
                { "e",            "Edit the rule (conditions, then actions)" },
                { "d",            "Delete the rule (asks first)"             },
                { "?",            "Show this help"                           },
                { "Esc / q",      "Back to the rules list"                   },
            };
            mail_rules_free(rules);
            email_service_help_popup("Rule shortcuts", help, 4);
            continue;
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

/* Model of the rules editor's table: one column, the rule's name. */
typedef struct { const MailRules *rules; } RulesModel;

static void rules_cell(void *ctx, int i, int col, tui_table_cell *c) {
    const RulesModel *m = ctx;
    (void)col;
    const char *name = m->rules->rules[i].name ? m->rules->rules[i].name : "(unnamed)";
    tui_table_cell_put(c, name, TUI_STYLE_DEFAULT);
}

/**
 * Show the report of a rules run (EMAIL-126): the lines the CLI's
 * `rules apply [--dry-run] --verbose` prints, in a screen of their own.
 * Any key goes back.
 */
static void tui_rules_report(const char *account, int dry_run)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *out = open_memstream(&buf, &len);
    if (!out) return;
    int rc = email_service_apply_rules_to(out, account, dry_run, 1);
    fclose(out);

    int rows = terminal_rows();
    if (rows < 5) rows = 24;
    int cols = terminal_cols();
    if (cols <= 0) cols = 80;
    printf("\033[2J\033[H");
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "  Rules %s: %s",
             dry_run ? "preview (dry run, nothing changed)" : "applied", account);
    tui_frame_print_bar(1, hdr, cols);
    int row = 3;
    const char *p = buf ? buf : "";
    while (*p && row < rows - 1) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        printf("\033[%d;1H%.*s", row++, n, p);
        p += n + (nl ? 1 : 0);
    }
    if (rc < 0)
        printf("\033[%d;1H  Error: the rules could not be applied.", row);
    free(buf);
    {
        static const tui_statusline_hint h[] = {
            { "any key", "back", SL_PRIO_KEEP },
        };
        screen_bar(h, 1, NULL, NULL);
    }
    fflush(stdout);
    RAII_TERM_RAW TermRawState *_raw = terminal_raw_enter();
    (void)terminal_read_key();
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

    for (;;) {
        MailRules *rules = mail_rules_load(account);
        int count = rules ? rules->count : 0;

        if (cursor >= count) cursor = count > 0 ? count - 1 : 0;
        if (cursor < 0) cursor = 0;

        int rows = terminal_rows();
        if (rows < 5) rows = 24;
        int cols = terminal_cols();
        if (cols <= 0) cols = 80;

        RulesModel rm = { rules };
        static const tui_table_col rcols[] = {
            { NULL, 20, 1, TUI_ALIGN_LEFT, 0, 0, 0 },
        };
        tui_table_model rt = { NULL, rules_cell, &rm };
        tui_table rtbl;
        tui_table_init(&rtbl, rcols, 1, &rt);
        rtbl.flags |= TUI_TABLE_NO_HEADER;
        tui_table_set_count(&rtbl, count);
        tui_table_select(&rtbl, cursor);
        if (count == 0)
            tui_table_set_message(&rtbl, "(no rules -- press 'a' to add one)",
                                  TUI_STYLE_DEFAULT);
        char title[512];
        snprintf(title, sizeof(title), "Rules for %s (%d rule%s)",
                 account, count, count == 1 ? "" : "s");
        tui_frame_draw_table(title, &rtbl, rows, cols, 0, NULL, NULL);
        {
            static const tui_statusline_hint h[] = {
                { "j/k", "navigate", SL_PRIO_MID },
                { "Enter", "open", SL_PRIO_KEEP },
                { "a", "add", SL_PRIO_HIGH },
                { "d", "delete", SL_PRIO_HIGH },
                { "p", "preview", SL_PRIO_MID },
                { "A", "apply now", SL_PRIO_MID },
                { "Esc/q", "back", SL_PRIO_KEEP },
            };
            screen_bar(h, 7, "?", NULL);
        }

        TermKey key;
        int ch;
        char del_confirm = 0;
        {
            RAII_TERM_RAW TermRawState *_raw = terminal_raw_enter();
            key = terminal_read_key();
            ch  = terminal_last_printable();
            if (ch == 'A') {
                printf("\033[%d;1H\033[2K  Apply the rules to the cached messages now? (y/N) ",
                       rows);
                fflush(stdout);
                ssize_t _na = read(STDIN_FILENO, &del_confirm, 1); (void)_na;
            }
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

        if (ch == '?') {
            static const char *help[][2] = {
                { "j / \u2193",   "Next rule"                     },
                { "k / \u2191",   "Previous rule"                 },
                { "Home / End",   "First / last rule"             },
                { "PgUp / PgDn",  "Page up / down"                },
                { "Enter",        "Open the rule"                 },
                { "a",            "Add a rule"                    },
                { "d",            "Delete the rule (asks first)"  },
                { "p",            "Preview what the rules would do (dry run)" },
                { "A",            "Apply the rules now (asks first)" },
                { "?",            "Show this help"                },
                { "Esc / q",      "Back to the message list"      },
            };
            mail_rules_free(rules);
            email_service_help_popup("Rules shortcuts", help, 11);
            continue;
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

        /* Home/End jump to the edges, PgUp/PgDn move a page (EMAIL-164). */
        if (key == TERM_KEY_HOME || key == TERM_KEY_END ||
            key == TERM_KEY_PREV_PAGE || key == TERM_KEY_NEXT_PAGE) {
            int page = rows - 4;
            if (page < 1) page = 1;
            if (key == TERM_KEY_HOME) cursor = 0;
            else if (key == TERM_KEY_END) cursor = count > 0 ? count - 1 : 0;
            else if (key == TERM_KEY_PREV_PAGE) cursor = cursor > page ? cursor - page : 0;
            else cursor = cursor + page < count ? cursor + page : (count > 0 ? count - 1 : 0);
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

        if (ch == 'p' || (ch == 'A' && (del_confirm == 'y' || del_confirm == 'Y'))) {
            mail_rules_free(rules);
            tui_rules_report(account, ch == 'p');
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
        "Compose a new message: a form for To, Cc, Bcc, Subject and attachments,\n"
        "then the built-in body editor.\n"
        "\n"
        "Esc leaves the editor and saves the draft; Ctrl-S saves and stays.\n"
        "The text is also saved after a few idle seconds, so nothing is lost\n"
        "if the terminal dies; the next compose offers the draft again.  The\n"
        "review screen then sends (s), edits (e, b, a, d) or discards (q).\n"
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
        "Opens the form, then the built-in body editor with the quoted message.\n"
        "Esc leaves the editor and saves the draft; the review screen sends it.\n"
        "\n"
        "Options:\n"
        "  --help, -h   Show this help message\n"
    );
}

/* ── Entry point ─────────────────────────────────────────────────────── */

#ifndef EMAIL_CLI_VERSION
#define EMAIL_CLI_VERSION "unknown"
#endif

/* ── Renew a Gmail authorization (EMAIL-130) ─────────────────────────── */

static volatile sig_atomic_t g_reauth_cancelled;

static void reauth_on_sigint(int sig) { (void)sig; g_reauth_cancelled = 1; }

/**
 * Renews the OAuth2 authorization of the Gmail account @p cfg: the flow of
 * 'email-cli config reauth' (browser, loopback without a browser, paste),
 * run on the plain terminal so that its messages and the pasted line work as
 * they do in the CLI.  Every step that waits says so on the screen.
 *
 * @return a one-line notice for the accounts screen (static storage).
 */
static const char *tui_reauth_gmail(Config *cfg) {
    static char note[300];
    printf("\033[2J\033[H\n  Renew the Gmail authorization of %s\n\n"
           "    b   open the browser here, wait for the authorization\n"
           "    l   show the address only (no browser; for an SSH forward)\n"
           "    p   paste the redirect address (browser on another machine)\n"
           "    Esc cancel\n\n"
           "  Choose: ", cfg->user ? cfg->user : "(unknown)");
    fflush(stdout);

    GmailLoginMode mode = GMAIL_LOGIN_BROWSER;
    for (;;) {
        TermRawState *raw = terminal_raw_enter();
        TermKey k = terminal_read_key();
        int ch = terminal_last_printable();
        terminal_raw_exit(&raw);
        if (k == TERM_KEY_ESC || k == TERM_KEY_QUIT) return "  Cancelled — the authorization is unchanged.";
        if (k != TERM_KEY_IGNORE) continue;
        if (ch == 'b') { mode = GMAIL_LOGIN_BROWSER;  break; }
        if (ch == 'l') { mode = GMAIL_LOGIN_LOOPBACK; break; }
        if (ch == 'p') { mode = GMAIL_LOGIN_PASTE;    break; }
    }
    printf("%c\n", mode == GMAIL_LOGIN_BROWSER ? 'b'
                    : mode == GMAIL_LOGIN_LOOPBACK ? 'l' : 'p');
    if (mode != GMAIL_LOGIN_PASTE)
        printf("\n  Waiting for Google: the screen stays here until the browser has\n"
               "  returned the authorization, or Ctrl-C cancels (5 minutes at most).\n");
    fflush(stdout);

    /* Ctrl-C ends the wait, not the program: a handler without SA_RESTART
     * makes the blocking accept() return, which the library reports as a
     * failure. */
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = reauth_on_sigint;
    sigemptyset(&sa.sa_mask);
    g_reauth_cancelled = 0;
    sigaction(SIGINT, &sa, &old);
    int rc = gmail_login(cfg, mode, stdin);
    sigaction(SIGINT, &old, NULL);

    if (rc != 0) {
        snprintf(note, sizeof note, "  %s — the account is unchanged.",
                 g_reauth_cancelled ? "Cancelled" : "Authorization was not completed");
        fprintf(stderr, "\n  Press any key to return to accounts...\n");
        fflush(stderr);
        TermRawState *raw = terminal_raw_enter();
        char c; ssize_t n = read(STDIN_FILENO, &c, 1); (void)n;
        terminal_raw_exit(&raw);
        return note;
    }
    if (config_save_account(cfg) != 0) {
        fprintf(stderr, "  Error: the authorization succeeded but the new token"
                        " could not be saved.\n  Press any key to return to accounts...\n");
        fflush(stderr);
        TermRawState *raw = terminal_raw_enter();
        char c; ssize_t n = read(STDIN_FILENO, &c, 1); (void)n;
        terminal_raw_exit(&raw);
        return "  The new token could not be saved.";
    }
    if (email_service_check_credential(cfg) != 0)
        return "  Authorization renewed, but the account does not answer yet.";
    return "  Gmail authorization renewed.";
}

int main(int argc, char *argv[]) {
    /* 0. Set locale so wcwidth() and mbsrtowcs() work correctly for
     *    multi-byte UTF-8 characters (needed for column-width calculations). */
    setlocale(LC_ALL, "");
    {
        /* The key table is checked against the keyboard policy at start. */
        char why[200] = "";
        if (ekeys_init(why, sizeof(why)) != 0) {
            fprintf(stderr, "email-tui: key table: %s\n", why);
            return EXIT_FAILURE;
        }
    }

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

    /* The full-screen interface cannot be drawn on a terminal that does not
     * understand escape sequences (TERM unset or dumb); the non-interactive
     * send still works, with the codes dropped from its output (EMAIL-108). */
    if (!terminal_understands_escapes()) {
        if (subcmd && strcmp(subcmd, "send") == 0) {
            terminal_plain_output_if_needed();
        } else {
            fprintf(stderr, "email-tui draws a full-screen interface and needs a terminal that\n"
                            "understands escape sequences; TERM is unset or 'dumb'.\n"
                            "Use email-cli instead, or set TERM (for example xterm-256color).\n");
            return EXIT_FAILURE;
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
    emc_log_open(log_file, 1);   /* libemail-client logs into the same file */
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
                /* Test the credential now rather than letting the first sync
                 * be the thing that discovers a typo. */
                email_service_check_credential(cfg);
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
            /* The headers are the dialog's; the editor holds the body only. */
            CDResult dlg;
            memset(&dlg, 0, sizeof(dlg));
            int cr = -1;
            if (compose_dialog(cfg, "New Message", NULL, NULL, NULL, &dlg))
                cr = cmd_compose_interactive(cfg, dlg.to, dlg.cc, dlg.bcc,
                                             dlg.subj, NULL, NULL, &dlg);
            rc = (cr == 0) ? EXIT_SUCCESS : EXIT_FAILURE;

        } else if (strcmp(subcmd, "send") == 0) {
            const char *to = NULL, *subject = NULL, *body_text = NULL;
            const char *attach_arr[8];
            int attach_count = 0;
            int send_args_ok = 1;
            for (int i = sub_i + 1; i < argc; i++) {
                int has_val = i + 1 < argc;
                if (strcmp(argv[i], "--to") == 0 && has_val) to = argv[++i];
                else if (strcmp(argv[i], "--subject") == 0 && has_val) subject = argv[++i];
                else if (strcmp(argv[i], "--body") == 0 && has_val) body_text = argv[++i];
                else if (strcmp(argv[i], "--batch") == 0) (void)0;
                else if (strcmp(argv[i], "--attach") == 0) {
                    if (!has_val) {
                        fprintf(stderr, "Error: --attach requires a file path.\n");
                        send_args_ok = 0;
                    } else if (attach_count < 8) {
                        attach_arr[attach_count++] = argv[++i];
                    } else {
                        fprintf(stderr, "Error: maximum 8 attachments allowed.\n");
                        send_args_ok = 0;
                    }
                }
            }
            if (!send_args_ok) {
                /* already reported */
            } else if (!to) {
                fprintf(stderr, "Error: --to is required for send.\n"
                                "Run 'email-tui send --help' for usage.\n");
            } else {
                const char *from = from_address(cfg);
                emc_message p = {.from = from, .to = to,
                                 .subject = subject   ? subject   : "",
                                 .body    = body_text ? body_text : "",
                                 .attachments = attach_count > 0 ? attach_arr : NULL,
                                 .attachment_count = attach_count};
                char *msg = NULL; size_t msg_len = 0;
                if (mail_compose(&p, &msg, &msg_len) != 0) {
                    fprintf(stderr, "Error: Failed to build message.\n");
                } else {
                    int sr = mail_send_wire(cfg, from, to, msg, msg_len);
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
            if (acc == 5) {
                /* ':' menu → renew the Gmail authorization of the account */
                acc_flash = tui_reauth_gmail(sel_cfg);
                config_free(sel_cfg);
                continue;
            }
            if (acc == 4) {
                /* 'i' → edit IMAP settings for selected account */
                if (setup_wizard_imap(sel_cfg) == 0) {
                    config_save_account(sel_cfg);
                    acc_flash = "  IMAP settings saved.";
                } else {
                    acc_flash = "  Cancelled — nothing was changed.";
                }
                config_free(sel_cfg);
                continue;  /* re-display accounts screen with updated info */
            }
            if (acc == 2) {
                /* 'e' → edit SMTP settings for selected account */
                if (setup_wizard_smtp(sel_cfg) == 0) {
                    config_save_account(sel_cfg);
                    acc_flash = "  SMTP settings saved.";
                } else {
                    acc_flash = "  Cancelled — nothing was changed.";
                }
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
                                  : "__unread__";

        /* Open folder/label browser — user picks which folder/label to enter.
         * Loop so that 'c' (compose) re-enters the browser after the dialog. */
        int go_up = 0;
        char *tui_folder = NULL;
        const char *browser_init = init_folder;
        unsigned errors_before = logger_stderr_error_count();
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
            /* Connection error or empty folder list.  A specific error line
             * (login rejected, TLS failure, ...) already says why: print it
             * once and no generic block after it (EMAIL-105). */
            if (logger_stderr_error_count() != errors_before) {
                fprintf(stderr, "\n");
            } else {
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
            EmailListOpts opts = {0};
            opts.folder = tui_folder;
            opts.limit  = page_size;
            opts.pager  = 1;
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
