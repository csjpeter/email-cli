/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-12 */
/* PTY tests of email-tui: every view compared cell for cell, text and style. */
/**
 * @file test_pty_screens.c
 * @brief Whole-screen, cell-exact tests of the email-tui views (EMAIL-34).
 *
 * Each view is reached through the keys a user would press, on a fixed
 * 100x24 terminal with TZ=UTC and a fixed mock, and then compared with a
 * picture of the expected text and a style map (attributes and colours per
 * cell).  The element inventory lives in docs/spec/tui-elements.md; every view
 * of it that has a screen has one case here.
 *
 * Usage: test-pty-screens <email-tui> <mock-imap-server>
 * With SCREENS_RECORD=<file> the screens are appended to <file> in the form
 * the expectations are written in, instead of being compared.
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ptytest.h"
#include "pty_assert.h"
#include "test_scratch.h"
#include "screens_expected.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

int g_tests_run = 0;
int g_tests_failed = 0;

#define RUN_TEST(fn) \
    do { \
        if (!test_selected(#fn)) break; \
        fprintf(stdout, "  %s...\n", #fn); \
        fflush(stdout); \
        fn(); \
    } while (0)

#define ROWS 24
#define COLS 100
#define WAIT_MS 6000
#define SETTLE_MS 500

static pid_t g_mock_pid = -1;
static int   g_imap_port = 0;
static int   g_dead_port = 0;
static char  g_home[256];
static char  g_tui_bin[512];
static char  g_mock_bin[512];
static FILE *g_record = NULL;
static int   g_shown_cols = COLS;   /* width of the session being recorded */

/* ── Fixture ─────────────────────────────────────────────────────────── */

static void write_account_config(void) {
    char dir[400], path[450];
    snprintf(dir, sizeof(dir), "%s/.config", g_home);            mkdir(g_home, 0700); mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.config/email-cli", g_home);  mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.config/email-cli/accounts", g_home); mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.config/email-cli/accounts/testuser", g_home); mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", dir);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:%d\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SSL_NO_VERIFY=1\n"
        "SMTP_HOST=smtps://localhost:%d\n", g_imap_port, g_dead_port);
    fclose(fp);
    chmod(path, 0600);
}

static void write_rules(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/email-cli/accounts/testuser/rules.ini", g_home);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "[rule \"SpamFilter\"]\n"
        "when = from:*@spam.example.com\n"
        "then-add-label = Junk\n"
        "\n"
        "[rule \"Newsletters\"]\n"
        "when = subject:\"*weekly report*\" or from:*@news.example.com\n"
        "then-add-label = News\n"
        "then-remove-label = INBOX\n");
    fclose(fp);
}

static void remove_rules(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/email-cli/accounts/testuser/rules.ini", g_home);
    unlink(path);
}

/** Every view starts from its own home: no saved cursor or preference of an
 *  earlier view is read (the folder browser remembers its last position). */
static void use_home(const char *view) {
    snprintf(g_home, sizeof(g_home), "%s", TP("h-"));
    size_t n = strlen(g_home);
    snprintf(g_home + n, sizeof(g_home) - n, "%s", view);
    write_account_config();
}

static void restart_mock(void) {
    if (g_mock_pid > 0) {
        kill(g_mock_pid, SIGKILL);
        waitpid(g_mock_pid, NULL, 0);
        g_mock_pid = -1;
    }
    int port = 0;
    g_mock_pid = test_spawn_mock(g_mock_bin, "MOCK_IMAP", g_imap_port, &port);
    if (g_mock_pid > 0) g_imap_port = port;
}

/* ── Opening a view ──────────────────────────────────────────────────── */

static int g_cols = COLS;

static PtySession *tui_open(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(g_cols, ROWS);
    if (!s) return NULL;
    pty_setenv(s, "TZ", "UTC");
    pty_setenv(s, "HOME", g_home);
    pty_setenv(s, "XDG_CONFIG_HOME", NULL);
    pty_setenv(s, "XDG_DATA_HOME", NULL);
    pty_setenv(s, "XDG_CACHE_HOME", NULL);
    pty_setenv(s, "EDITOR", "true");
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    return s;
}

static PtySession *at_accounts(void) {
    PtySession *s = tui_open();
    if (!s) return NULL;
    if (pty_wait_for(s, "Email Accounts", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static PtySession *at_folders(void) {
    PtySession *s = at_accounts();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static PtySession *at_list(void) {
    PtySession *s = at_folders();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "message(s) in", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static PtySession *at_empty_folder(void) {
    PtySession *s = at_folders();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 8; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "Empty", WAIT_MS);
    pty_settle(s, SETTLE_MS * 2);
    return s;
}

static PtySession *at_narrow_list(void) {
    g_cols = 80;
    PtySession *s = at_list();
    g_cols = COLS;
    return s;
}

static PtySession *at_reader(void) {
    PtySession *s = at_list();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS * 2);
    return s;
}

static PtySession *at_rules(void) {
    PtySession *s = at_list();
    if (!s) return NULL;
    pty_send_str(s, "l");
    if (pty_wait_for(s, "Rules for", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* ── Comparing or recording a view ───────────────────────────────────── */

/** Style legend of this client: what each map character stands for. */
static const PtyStyleKey LEGEND[] = {
    { ' ', PTY_ATTR_NONE,                    PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'b', PTY_ATTR_BOLD,                    PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'd', PTY_ATTR_DIM,                     PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'r', PTY_ATTR_REVERSE,                 PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'B', PTY_ATTR_BOLD | PTY_ATTR_REVERSE, PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'H', PTY_ATTR_BOLD,                    PTY_FG_WHITE,   PTY_BG_BLUE    },
    { 'g', PTY_ATTR_NONE,                    PTY_FG_GREEN,   PTY_BG_DEFAULT },
    { 'G', PTY_ATTR_REVERSE,                 PTY_FG_GREEN,   PTY_BG_DEFAULT },
    { 'R', PTY_ATTR_NONE,                    PTY_FG_RED,     PTY_BG_DEFAULT },
    /* The selection of a tui_table: dark blue background (palette 17), the
     * text bright white; the '>' bold; a coloured cell keeps its hue as the
     * bright variant plus bold. */
    { 'S', PTY_ATTR_NONE,                    97,             PTY_COLOR_IDX(17) },
    { 'A', PTY_ATTR_BOLD,                    97,             PTY_COLOR_IDX(17) },
    { 'n', PTY_ATTR_BOLD,                    92,             PTY_COLOR_IDX(17) },
    { 0 }
};

static void record_view(const char *name, PtySession *s) {
    int rows_n = ROWS;
    pty_get_size(s, &g_shown_cols, &rows_n);
    char buf[ROWS * (COLS + 1) * 4 + 64];
    fprintf(g_record, "=== %s\n", name);
    pty_region_dump_c(s, NULL, g_record);
    fprintf(g_record, "--- style\n%s\n",
            pty_region_style_map(s, NULL, LEGEND, buf, sizeof(buf)));
    /* Styles the legend has no key for: one description of each. */
    int seen[16][3], nseen = 0;
    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            if (col >= g_shown_cols) break;
            int a = pty_cell_attr(s, row, col), f = pty_cell_fg(s, row, col),
                b = pty_cell_bg(s, row, col), known = 0;
            for (const PtyStyleKey *k = LEGEND; k->key; k++)
                if (k->attr == a && k->fg == f && k->bg == b) { known = 1; break; }
            for (int i = 0; i < nseen && !known; i++)
                if (seen[i][0] == a && seen[i][1] == f && seen[i][2] == b) known = 1;
            if (known || nseen >= 16) continue;
            seen[nseen][0] = a; seen[nseen][1] = f; seen[nseen][2] = b; nseen++;
            char d[160];
            fprintf(g_record, "--- unknown style at row %d col %d: %s\n", row, col,
                    pty_cell_describe(s, row, col, d, sizeof(d)));
        }
    }
    int r = 0, c = 0, vis = pty_cursor(s, &r, &c);
    fprintf(g_record, "--- cursor row %d col %d %s\n\n", r, c, vis ? "visible" : "hidden");
    fflush(g_record);
}

/** Compare the screen with a picture and a style map, region by region, and
 *  the cursor.  A golden row that is "@@" stands for a block of any number of
 *  screen rows (the path of the message file, which wraps differently on every
 *  machine): the screen is searched for the row that follows it and the rest
 *  is compared from there.  A golden row starting with '~' is compared with
 *  its digit runs read as one '#' (a port number). */
static void check_view(PtySession *s, const char *const exp[],
                       const char *const sty[], const int cur[3]) {
    int n = 0;
    while (exp[n]) n++;
    int cols = COLS, rows_n = ROWS;
    pty_get_size(s, &cols, &rows_n);
    int offset = 0;          /* screen row = golden row + offset */
    int marker = -1;         /* golden row of the first "@@" */
    int i = 0;
    while (i < n) {
        if (strcmp(exp[i], "@@") == 0) {
            int found = -1;
            for (int r = i + offset; r < ROWS && i + 1 < n; r++) {
                char row[COLS * 4 + 4];
                pty_row_text(s, r, row, sizeof(row));
                char want[COLS * 4 + 4];
                snprintf(want, sizeof(want), "%s", exp[i + 1]);
                if (strcmp(row, want) == 0) { found = r; break; }
            }
            ASSERT(found >= 0, "the row after the variable block is on the screen");
            offset = found - (i + 1);
            if (marker < 0) marker = i;
            i++;
            continue;
        }
        int j = i;
        while (j < n && strcmp(exp[j], "@@") != 0) j++;
        const char *srows[ROWS + 1];
        int k = 0;
        for (int r = i; r < j; r++, k++) srows[k] = sty[r];
        srows[k] = NULL;
        /* Text: a golden row starting with '~' holds a port number or the like
         * (a different one on every run); on the screen every run of digits is
         * read as one '#' before the comparison.  The rows around it are
         * compared as one region each. */
        int t = i;
        while (t < j) {
            if (exp[t][0] == '~') {
                char row[COLS * 4 + 4], norm[COLS * 4 + 4];
                pty_row_text(s, t + offset, row, sizeof(row));
                size_t o = 0;
                for (const char *q = row; *q && o + 1 < sizeof(norm); q++) {
                    if (*q >= '0' && *q <= '9') {
                        if (o == 0 || norm[o - 1] != '#') norm[o++] = '#';
                    } else {
                        norm[o++] = *q;
                    }
                }
                norm[o] = '\0';
                ASSERT(strcmp(norm, exp[t] + 1) == 0,
                       "row with a variable number matches its picture");
                t++;
                continue;
            }
            int u = t;
            while (u < j && exp[u][0] != '~') u++;
            const char *part[ROWS + 1];
            int pk = 0;
            for (int r = t; r < u; r++) part[pk++] = exp[r];
            part[pk] = NULL;
            ASSERT_REGION_EQ(s, PTY_REGION(t + offset, 0, u - t, cols), part);
            t = u;
        }
        ASSERT_REGION_STYLE(s, PTY_REGION(i + offset, 0, j - i, cols), srows, LEGEND);
        i = j;
    }
    int r = -1, c = -1;
    int vis = pty_cursor(s, &r, &c);
    int want_row = cur[0] + ((marker >= 0 && cur[0] > marker) ? offset : 0);
    ASSERT(r == want_row && c == cur[1] && vis == cur[2],
           "cursor position and visibility");
}

#define VIEW(name, open_expr, setup) \
    static void test_screen_##name(void) { \
        restart_mock(); \
        use_home(#name); \
        PtySession *s = (open_expr); \
        ASSERT(s != NULL, #name ": the view opens"); \
        setup; \
        if (g_record) { record_view(#name, s); pty_close(s); return; } \
        check_view(s, exp_##name, sty_##name, cur_##name); \
        pty_close(s); \
    }

VIEW(accounts,         at_accounts(), (void)0)
VIEW(folders,          at_folders(),  (void)0)
VIEW(list,             at_list(),     (void)0)
VIEW(reader,           at_reader(),   (void)0)
VIEW(rules_empty,      (remove_rules(), at_rules()), (void)0)
VIEW(rules_list,       (write_rules(), at_rules()), (void)0)
VIEW(rule_detail,      (write_rules(), at_rules()),
     (pty_send_key(s, PTY_KEY_ENTER), pty_settle(s, SETTLE_MS)))
VIEW(rule_add_when,    (remove_rules(), at_rules()),
     (pty_send_str(s, "a"), pty_wait_for(s, "Add new rule", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(rule_add_actions, (remove_rules(), at_rules()),
     (pty_send_str(s, "a"), pty_wait_for(s, "Add new rule", WAIT_MS),
      pty_send_str(s, "q"), pty_wait_for(s, "step 2", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(compose_dialog,   at_list(),
     (pty_send_str(s, "c"), pty_wait_for(s, "New Message", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(accounts_help,    at_accounts(),
     (pty_send_str(s, "h"), pty_wait_for(s, "shortcuts", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(folders_help,     at_folders(),
     (pty_send_str(s, "h"), pty_wait_for(s, "shortcuts", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(list_help,        at_list(),
     (pty_send_str(s, "h"), pty_wait_for(s, "shortcuts", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(reader_help,      at_reader(),
     (pty_send_str(s, "h"), pty_wait_for(s, "shortcuts", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(list_search,      at_list(),
     (pty_send_str(s, "/"), pty_settle(s, SETTLE_MS)))
VIEW(wizard_type,      at_accounts(),
     (pty_send_str(s, "n"), pty_wait_for(s, "Account type", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(account_delete,   at_accounts(),
     (pty_send_str(s, "d"), pty_settle(s, SETTLE_MS)))

VIEW(list_second_row,  at_list(), (pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
VIEW(list_narrow,      at_narrow_list(), (void)0)
VIEW(list_empty,       at_empty_folder(), (void)0)
VIEW(folders_flat,     at_folders(), (pty_send_str(s, "t"), pty_settle(s, SETTLE_MS)))
VIEW(folders_search,   at_folders(), (pty_send_str(s, "/"), pty_settle(s, SETTLE_MS)))
VIEW(reader_source,    at_reader(), (pty_send_str(s, "v"), pty_settle(s, SETTLE_MS)))
VIEW(reader_search,    at_reader(), (pty_send_str(s, "/"), pty_settle(s, SETTLE_MS)))
VIEW(rules_second,     (write_rules(), at_rules()),
     (pty_send_str(s, "j"), pty_settle(s, SETTLE_MS)))
VIEW(rule_edit_form,   (write_rules(), at_rules()),
     (pty_send_key(s, PTY_KEY_ENTER), pty_settle(s, SETTLE_MS),
      pty_send_str(s, "e"), pty_settle(s, SETTLE_MS)))
VIEW(compose_typed,    at_list(),
     (pty_send_str(s, "c"), pty_wait_for(s, "New Message", WAIT_MS),
      pty_send_str(s, "ann@example.org"), pty_settle(s, SETTLE_MS)))
VIEW(compose_empty_to, at_list(),
     (pty_send_str(s, "c"), pty_wait_for(s, "New Message", WAIT_MS),
      pty_send_key(s, PTY_KEY_ENTER), pty_wait_for(s, "To: is empty", WAIT_MS),
      pty_settle(s, SETTLE_MS)))

int main(int argc, char *argv[]) {
    printf("--- email-tui screens (cell-exact) ---\n\n");
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <email-tui> <mock-imap-server>\n", argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(g_tui_bin, sizeof(g_tui_bin), "%s", argv[1]);
    snprintf(g_mock_bin, sizeof(g_mock_bin), "%s", argv[2]);

    const char *rec = getenv("SCREENS_RECORD");
    if (rec && *rec) g_record = fopen(rec, "w");

    snprintf(g_home, sizeof(g_home), "%s", TP("home"));
    test_set_home(g_home);
    mkdir(g_home, 0700);
    setenv("TZ", "UTC", 1);
    setenv("MOCK_IMAP_COUNT", "3", 1);
    g_dead_port = test_unused_port();
    restart_mock();
    if (g_mock_pid < 0 || g_dead_port < 0) {
        fprintf(stderr, "FATAL: cannot start the mock server\n");
        return EXIT_FAILURE;
    }

    RUN_TEST(test_screen_accounts);
    RUN_TEST(test_screen_folders);
    RUN_TEST(test_screen_list);
    RUN_TEST(test_screen_reader);
    RUN_TEST(test_screen_rules_empty);
    RUN_TEST(test_screen_rules_list);
    RUN_TEST(test_screen_rule_detail);
    RUN_TEST(test_screen_rule_add_when);
    RUN_TEST(test_screen_rule_add_actions);
    RUN_TEST(test_screen_compose_dialog);
    RUN_TEST(test_screen_accounts_help);
    RUN_TEST(test_screen_folders_help);
    RUN_TEST(test_screen_list_help);
    RUN_TEST(test_screen_reader_help);
    RUN_TEST(test_screen_list_search);
    RUN_TEST(test_screen_wizard_type);
    RUN_TEST(test_screen_account_delete);
    RUN_TEST(test_screen_list_second_row);
    RUN_TEST(test_screen_list_narrow);
    RUN_TEST(test_screen_list_empty);
    RUN_TEST(test_screen_folders_flat);
    RUN_TEST(test_screen_folders_search);
    RUN_TEST(test_screen_reader_source);
    RUN_TEST(test_screen_reader_search);
    RUN_TEST(test_screen_rules_second);
    RUN_TEST(test_screen_rule_edit_form);
    RUN_TEST(test_screen_compose_typed);
    RUN_TEST(test_screen_compose_empty_to);

    if (g_record) fclose(g_record);
    if (g_mock_pid > 0) { kill(g_mock_pid, SIGKILL); waitpid(g_mock_pid, NULL, 0); }

    printf("\n--- PTY Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
