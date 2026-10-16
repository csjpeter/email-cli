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
#define MAXCOLS 200   /* the widest terminal a view is recorded at */
#define WAIT_MS 6000
#define SETTLE_MS 500

static pid_t g_mock_pid = -1;
static int   g_imap_port = 0;
static int   g_dead_port = 0;
static char  g_home[256];
static char  g_tui_bin[512];
static char  g_mock_bin[512];
static pid_t g_gmail_pid = -1;
static int   g_gmail_port = 0;
static char  g_sync_bin[512];
static char  g_gmail_bin[512];
static FILE *g_record = NULL;
static int   g_shown_cols = COLS;   /* width of the session being recorded */

/* ── Fixture ─────────────────────────────────────────────────────────── */

/* What a view changes in the account: another port or password, TLS
 * verification left on, a sync interval (cron mode).  Reset by use_home(). */
static int         g_cfg_port = 0;
static const char *g_cfg_pass = "testpass";
static int         g_cfg_verify = 0;
static int         g_cfg_interval = 0;

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
        "EMAIL_PASS=%s\n"
        "EMAIL_FOLDER=INBOX\n"
        "%s"
        "SMTP_HOST=smtps://localhost:%d\n",
        g_cfg_port ? g_cfg_port : g_imap_port, g_cfg_pass,
        g_cfg_verify ? "" : "SSL_NO_VERIFY=1\n", g_dead_port);
    if (g_cfg_interval) fprintf(fp, "SYNC_INTERVAL=%d\n", g_cfg_interval);
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
    g_cfg_port = 0; g_cfg_pass = "testpass"; g_cfg_verify = 0; g_cfg_interval = 0;
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

/* ── The Gmail fixture (EMAIL-88) ────────────────────────────────────── */

#define GMAIL_USER "screens@example.org"

static void stop_gmail(void) {
    if (g_gmail_pid > 0) {
        kill(g_gmail_pid, SIGKILL);
        waitpid(g_gmail_pid, NULL, 0);
        g_gmail_pid = -1;
    }
}

static void restart_gmail(void) {
    stop_gmail();
    if (!g_gmail_bin[0]) return;
    setenv("MOCK_GMAIL_COUNT", "5", 1);
    setenv("MOCK_GMAIL_EMAIL", GMAIL_USER, 1);
    setenv("MOCK_GMAIL_NO_USER_LABELS", "1", 1);   /* the recorded pictures list system labels only */
    g_gmail_pid = test_spawn_mock(g_gmail_bin, "MOCK_GMAIL", 0, &g_gmail_port);
    unsetenv("MOCK_GMAIL_COUNT");
    unsetenv("MOCK_GMAIL_EMAIL");
    unsetenv("MOCK_GMAIL_NO_USER_LABELS");
    char url[128];
    snprintf(url, sizeof(url), "http://localhost:%d/gmail/v1/users/me", g_gmail_port);
    setenv("GMAIL_API_BASE_URL", url, 1);
    setenv("GMAIL_TEST_TOKEN", "test_fake_token", 1);
}

/** A home with one Gmail account, its local store filled by email-sync. */
static void use_gmail_home(const char *view) {
    snprintf(g_home, sizeof(g_home), "%s", TP("g-"));
    size_t n = strlen(g_home);
    snprintf(g_home + n, sizeof(g_home) - n, "%s", view);
    char dir[500], path[600];
    snprintf(dir, sizeof(dir), "%s/.config", g_home);            mkdir(g_home, 0700); mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.config/email-cli", g_home);  mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.config/email-cli/accounts", g_home); mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.config/email-cli/accounts/%s", g_home, GMAIL_USER); mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", dir);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "GMAIL_MODE=1\n"
        "EMAIL_USER=%s\n"
        "GMAIL_ACCESS_TOKEN=fake_token\n"
        "GMAIL_REFRESH_TOKEN=fake_refresh\n"
        "GMAIL_TOKEN_EXPIRY=9999999999\n"
        "EMAIL_FOLDER=INBOX\n", GMAIL_USER);
    fclose(fp);
    chmod(path, 0600);
    if (!g_sync_bin[0]) return;
    setenv("HOME", g_home, 1);
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    pid_t pid = fork();
    if (pid == 0) {
        execl(g_sync_bin, "email-sync", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

/* ── Opening a view ──────────────────────────────────────────────────── */

static int g_cols = COLS;
static const char *g_term = NULL;   /* TERM of the view, NULL: the harness default */

static PtySession *tui_open(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(g_cols, ROWS);
    if (!s) return NULL;
    pty_setenv(s, "TZ", "UTC");
    if (g_term) pty_setenv(s, "TERM", g_term);
    pty_setenv(s, "HOME", g_home);
    pty_setenv(s, "XDG_CONFIG_HOME", NULL);
    pty_setenv(s, "XDG_DATA_HOME", NULL);
    pty_setenv(s, "XDG_CACHE_HOME", NULL);
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

static PtySession *at_gmail_labels(void) {
    PtySession *s = at_accounts();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Labels", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* Opens the label browser entry @p downs rows below Unread (Enter). */
static void gmail_open_entry(PtySession *s, int downs) {
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < downs; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "message(s)", WAIT_MS);
    pty_settle(s, SETTLE_MS);
}

static PtySession *at_gmail_entry(int downs) {
    PtySession *s = at_gmail_labels();
    if (!s) return NULL;
    gmail_open_entry(s, downs);
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

/* The default mock message: a multipart with two attachments (notes.txt,
 * data.bin).  The suite's count of 3 is lifted for these views. */
static PtySession *at_attach_reader(void) {
    unsetenv("MOCK_IMAP_COUNT");
    restart_mock();
    setenv("MOCK_IMAP_COUNT", "3", 1);
    write_account_config();
    PtySession *s = at_list();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "A save-all", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* An account that cannot be used: kind 1 connect (a dead port), 2 login (the
 * server wants another password), 3 TLS (the self-signed certificate is
 * verified).  The view is the folder browser's attempt after Enter. */
static PtySession *at_error(int kind) {
    if (kind == 1) g_cfg_port = g_dead_port;
    if (kind == 3) g_cfg_verify = 1;
    if (kind == 2) {
        setenv("MOCK_IMAP_PASSWORD", "the-right-password", 1);
        restart_mock();
        unsetenv("MOCK_IMAP_PASSWORD");
    }
    write_account_config();
    PtySession *s = at_accounts();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS * 4);
    return s;
}

/* Cron mode: the list is read from the local cache, a background sync can
 * run beside it.  The cache is filled by one email-sync run first. */
static PtySession *at_cron_list(void) {
    g_cfg_interval = 5;
    write_account_config();
    if (g_sync_bin[0]) {
        setenv("HOME", g_home, 1);
        unsetenv("XDG_DATA_HOME");
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_CACHE_HOME");
        pid_t pid = fork();
        if (pid == 0) {
            execl(g_sync_bin, "email-sync", (char *)NULL);
            _exit(127);
        }
        if (pid > 0) waitpid(pid, NULL, 0);
    }
    return at_list();
}

/* A view at another width (80 is the policy minimum) or on another terminal. */
static PtySession *at_cols(int cols, PtySession *(*open)(void)) {
    g_cols = cols;
    PtySession *s = open();
    g_cols = COLS;
    return s;
}

static PtySession *at_term(const char *term, PtySession *(*open)(void)) {
    g_term = term;
    PtySession *s = open();
    g_term = NULL;
    return s;
}

/* The default mock message with a chosen subject, as a list (reader 0) or
 * the opened message (reader 1). */
static PtySession *at_subject(const char *subject, int reader) {
    unsetenv("MOCK_IMAP_COUNT");
    setenv("MOCK_IMAP_SUBJECT", subject, 1);
    restart_mock();
    unsetenv("MOCK_IMAP_SUBJECT");
    setenv("MOCK_IMAP_COUNT", "3", 1);
    write_account_config();
    PtySession *s = at_list();
    if (!s) return NULL;
    if (reader) {
        pty_send_key(s, PTY_KEY_ENTER);
        pty_settle(s, SETTLE_MS * 2);
    }
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
    { 'Y', PTY_ATTR_BOLD,                    93,             PTY_COLOR_IDX(17) },
    { 'E', PTY_ATTR_BOLD,                    91,             PTY_COLOR_IDX(17) },
    { 'y', PTY_ATTR_BOLD,                    PTY_FG_YELLOW,  PTY_BG_DEFAULT },
    { 'Z', PTY_ATTR_NONE,                    PTY_FG_YELLOW,  PTY_BG_DEFAULT },
    /* The unread N on an 8-colour terminal: plain green, no bright variant. */
    { 'm', PTY_ATTR_BOLD,                    PTY_FG_GREEN,   PTY_BG_DEFAULT },
    /* A dim cell on the selected row (the account list's server column). */
    { 'a', PTY_ATTR_DIM,                     97,             PTY_COLOR_IDX(17) },
    /* The address drop-down's marker: cyan, reverse on the chosen row. */
    { 'c', PTY_ATTR_NONE,                    PTY_FG_CYAN,    PTY_BG_DEFAULT },
    { 'C', PTY_ATTR_REVERSE,                 PTY_FG_CYAN,    PTY_BG_DEFAULT },
    /* The rendered HTML body of the attachment message. */
    { 'x', PTY_ATTR_BOLD,                    PTY_COLOR_RGB(255, 0, 0), PTY_BG_DEFAULT },
    { 'X', PTY_ATTR_BOLD | PTY_ATTR_ITALIC,  PTY_COLOR_RGB(255, 0, 0), PTY_BG_DEFAULT },
    { 'i', PTY_ATTR_ITALIC,                  PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'u', PTY_ATTR_UNDERLINE,               PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'k', PTY_ATTR_UNDERLINE | PTY_ATTR_STRIKE, PTY_FG_DEFAULT, PTY_BG_DEFAULT },
    { 'e', PTY_ATTR_BOLD,                    PTY_FG_RED,     PTY_BG_DEFAULT },
    /* The frame and the title of a libtuikit dialog, the address in the
     * drop-down (bold cyan); the same on the selected row (EMAIL-110). */
    { 'T', PTY_ATTR_BOLD,                    PTY_FG_CYAN,    PTY_BG_DEFAULT },
    { 'Q', PTY_ATTR_BOLD,                    96,             PTY_COLOR_IDX(17) },
    { 0 }
};

static void record_view(const char *name, PtySession *s) {
    int rows_n = ROWS;
    pty_get_size(s, &g_shown_cols, &rows_n);
    char buf[ROWS * (MAXCOLS + 1) * 4 + 64];
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
 *  is compared from there.  A golden cursor column of -1 is the end of the
 *  text of its row (a path with a process number in it).  A golden row starting with '~' is compared with
 *  its digit runs read as one '#' (a port number); one starting with '%' is
 *  compared by its start only (a path), and its style is not compared. */
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
                char row[MAXCOLS * 4 + 4];
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
            if (exp[t][0] == '%') {
                /* A path that differs on every machine: only its start is
                 * compared (and the style of its row is not). */
                char row[MAXCOLS * 4 + 4];
                pty_row_text(s, t + offset, row, sizeof(row));
                size_t pl = strlen(exp[t] + 1);
                ASSERT(strncmp(row, exp[t] + 1, pl) == 0,
                       "row with a variable path starts as in its picture");
                t++;
                continue;
            }
            if (exp[t][0] == '~') {
                char row[MAXCOLS * 4 + 4], norm[MAXCOLS * 4 + 4];
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
            while (u < j && exp[u][0] != '~' && exp[u][0] != '%') u++;
            const char *part[ROWS + 1];
            int pk = 0;
            for (int r = t; r < u; r++) part[pk++] = exp[r];
            part[pk] = NULL;
            ASSERT_REGION_EQ(s, PTY_REGION(t + offset, 0, u - t, cols), part);
            t = u;
        }
        /* Styles, except of the rows whose path is machine-specific. */
        for (int a = i; a < j;) {
            if (exp[a][0] == '%') { a++; continue; }
            int b = a;
            while (b < j && exp[b][0] != '%') b++;
            const char *seg[ROWS + 1];
            int sk = 0;
            for (int r = a; r < b; r++) seg[sk++] = sty[r];
            seg[sk] = NULL;
            ASSERT_REGION_STYLE(s, PTY_REGION(a + offset, 0, b - a, cols), seg, LEGEND);
            a = b;
        }
        (void)srows;
        i = j;
    }
    int r = -1, c = -1;
    int vis = pty_cursor(s, &r, &c);
    int want_row = cur[0] + ((marker >= 0 && cur[0] > marker) ? offset : 0);
    /* A golden cursor column of -1 means "just after the text of the row":
     * the row ends in a path that holds the test process's number, so its
     * length (and the column) changes with the digits of that number. */
    int want_col = cur[1];
    if (want_col < 0) {
        char row[MAXCOLS * 4 + 4];
        pty_row_text(s, want_row, row, sizeof(row));
        size_t len = strlen(row);
        while (len > 0 && row[len - 1] == ' ') len--;
        want_col = 0;
        for (size_t q = 0; q < len; q++)
            if (((unsigned char)row[q] & 0xC0) != 0x80) want_col++;
    }
    char why[160];
    snprintf(why, sizeof(why),
             "cursor position and visibility: row %d col %d %s, want row %d col %d %s",
             r, c, vis ? "visible" : "hidden", want_row, want_col,
             cur[2] ? "visible" : "hidden");
    ASSERT(r == want_row && c == want_col && vis == cur[2], why);
}

/* The compose flow up to the body editor: the dialog filled, Enter. */
static void to_editor(PtySession *s) {
    pty_send_str(s, "c");
    pty_wait_for(s, "New Message", WAIT_MS);
    pty_send_str(s, "ann@example.org");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "Hello");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_wait_for(s, "Esc save and continue", WAIT_MS);
    pty_settle(s, SETTLE_MS);
}

static void editor_typed(PtySession *s) {
    to_editor(s);
    pty_send_str(s, "Dear Ann,\r\rWide: \xe4\xbd\xa0\xe5\xa5\xbd caf\xc3\xa9");
    pty_settle(s, SETTLE_MS);
}

static void to_review(PtySession *s) {
    editor_typed(s);
    pty_send_key(s, PTY_KEY_ESC);
    pty_wait_for(s, "s send", WAIT_MS);
    pty_settle(s, SETTLE_MS);
}

/* The compose flow to the review screen with Cc, Bcc and an attachment. */
static void to_review_full(PtySession *s, const char *to) {
    char path[512];
    snprintf(path, sizeof(path), "%s/report.pdf", g_home);
    FILE *fp = fopen(path, "w");
    if (fp) { fputs("%PDF", fp); fclose(fp); }
    pty_send_str(s, "c");
    pty_wait_for(s, "New Message", WAIT_MS);
    pty_send_str(s, to);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "cc@example.org");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "bcc@example.org");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "Quarterly report");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, path);
    pty_send_key(s, PTY_KEY_ENTER);   /* adds the attachment */
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ENTER);   /* confirms the dialog */
    pty_wait_for(s, "Esc save and continue", WAIT_MS);
    pty_settle(s, 200);
    pty_send_str(s, "Please find the report attached.");
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ESC);
    pty_wait_for(s, "s send", WAIT_MS);
    pty_settle(s, SETTLE_MS);
}

/* Contacts for the address drop-down: seven "ann..." (more than the five the
 * list shows), one "bart..." and one with a wide-character name. */
static void write_contacts(void) {
    char dir[600], path[700];
    snprintf(dir, sizeof(dir), "%s/.local", g_home);                       mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.local/share", g_home);                 mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.local/share/email-cli", g_home);       mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.local/share/email-cli/accounts", g_home); mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.local/share/email-cli/accounts/testuser", g_home); mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/contacts.tsv", dir);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    for (int i = 1; i <= 7; i++)
        fprintf(fp, "ann%d@example.org\tAnn Number %d\t%d\n", i, i, 10 - i);
    fprintf(fp, "bart@example.org\tBart Simpson\t3\n");
    fprintf(fp, "yamada@example.org\t\xe5\xb1\xb1\xe7\x94\xb0 \xe5\xa4\xaa\xe9\x83\x8e\t2\n");
    fclose(fp);
}

static void compose_type_to(PtySession *s, const char *text) {
    write_contacts();
    pty_send_str(s, "c");
    pty_wait_for(s, "New Message", WAIT_MS);
    pty_send_str(s, text);
    pty_settle(s, SETTLE_MS);
}

/* Files for path completion under the view's home. */
static void compose_type_path(PtySession *s, const char *prefix) {
    char dir[600], path[700];
    snprintf(dir, sizeof(dir), "%s/att_dir", g_home);
    mkdir(dir, 0700);
    const char *names[] = { "alpha.txt", "alpha2.txt", "beta.bin" };
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        FILE *fp = fopen(path, "w");
        if (fp) { fputs("x", fp); fclose(fp); }
    }
    pty_send_str(s, "c");
    pty_wait_for(s, "New Message", WAIT_MS);
    for (int i = 0; i < 4; i++) pty_send_key(s, PTY_KEY_TAB);   /* To → Attach */
    snprintf(path, sizeof(path), "%s/att_dir/%s", g_home, prefix);
    pty_send_str(s, path);
    pty_settle(s, SETTLE_MS);
}

/* The setup wizard: `n` on the accounts screen, then the given answers, one
 * Enter each; the text after the last answer is typed but not confirmed. */
static void wizard_answers(PtySession *s, const char *const *answers, int n,
                           const char *typing) {
    pty_send_str(s, "n");
    pty_wait_for(s, "Account type", WAIT_MS);
    for (int i = 0; i < n; i++) {
        pty_settle(s, 300);
        pty_send_str(s, answers[i]);
        pty_send_key(s, PTY_KEY_ENTER);
    }
    pty_settle(s, SETTLE_MS);
    if (typing && typing[0]) {
        pty_send_str(s, typing);
        pty_settle(s, SETTLE_MS);
    }
}

static PtySession *at_rules_written(void) { write_rules(); return at_rules(); }
static PtySession *at_compose_editor(void) {
    PtySession *s = at_list();
    if (s) to_editor(s);
    return s;
}
static PtySession *at_review_screen(void) {
    PtySession *s = at_list();
    if (s) to_review(s);
    return s;
}
/* The review screen with a To list too long for a narrow terminal (EMAIL-107). */
static PtySession *at_review_long_to(void) {
    PtySession *s = at_list();
    if (s) to_review_full(s, "annabelle.longname@example.org; bartholomew.longname@example.org; "
                             "cornelia.longname@example.org; dominic.longname@example.org");
    return s;
}
static PtySession *at_list_help(void) {
    PtySession *s = at_list();
    if (s) {
        pty_send_str(s, "h");
        pty_wait_for(s, "shortcuts", WAIT_MS);
        pty_settle(s, SETTLE_MS);
    }
    return s;
}
static PtySession *at_compose_form(void) {
    PtySession *s = at_list();
    if (s) {
        pty_send_str(s, "c");
        pty_wait_for(s, "New Message", WAIT_MS);
        pty_settle(s, SETTLE_MS);
    }
    return s;
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

#define GVIEW(name, open_expr, setup) \
    static void test_screen_##name(void) { \
        restart_mock(); \
        restart_gmail(); \
        use_gmail_home(#name); \
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
     (pty_send_str(s, "d"), pty_wait_for(s, "(y/N)", WAIT_MS),
      pty_send_str(s, "y"), pty_settle(s, SETTLE_MS)))
VIEW(account_delete_ask, at_accounts(),
     (pty_send_str(s, "d"), pty_wait_for(s, "(y/N)", WAIT_MS), pty_settle(s, SETTLE_MS)))

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

VIEW(list_marked,       at_list(), (pty_send_str(s, " "), pty_settle(s, SETTLE_MS)))
VIEW(list_marked_two,   at_list(),
     (pty_send_str(s, " "), pty_send_str(s, " "), pty_settle(s, SETTLE_MS)))
VIEW(list_bulk_prompt,  at_list(),
     (pty_send_str(s, " "), pty_send_str(s, "m"), pty_wait_for(s, "Esc=cancel", WAIT_MS),
      pty_settle(s, SETTLE_MS)))
VIEW(list_bulk_cursor_prompt, at_list(),
     (pty_send_str(s, "m"), pty_wait_for(s, "Esc=cancel", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(list_move_prompt,  at_list(),
     (pty_send_str(s, " "), pty_send_str(s, "m"), pty_wait_for(s, "Esc=cancel", WAIT_MS),
      pty_send_str(s, "m"), pty_wait_for(s, "Move to folder", WAIT_MS),
      pty_send_str(s, "Archive"), pty_settle(s, SETTLE_MS)))
VIEW(list_delete_ask,   at_list(),
     (pty_send_str(s, " "), pty_send_str(s, "m"), pty_wait_for(s, "Esc=cancel", WAIT_MS),
      pty_send_str(s, "D"), pty_wait_for(s, "(y/N)", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(list_bulk_done,    at_list(),
     (pty_send_str(s, " "), pty_send_str(s, "m"), pty_wait_for(s, "Esc=cancel", WAIT_MS),
      pty_send_str(s, "f"), pty_wait_for(s, "starred", WAIT_MS), pty_settle(s, SETTLE_MS)))

VIEW(att_reader,        at_attach_reader(), (void)0)
VIEW(att_picker,        at_attach_reader(),
     (pty_send_str(s, "a"), pty_wait_for(s, "Attachments", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(att_picker_second, at_attach_reader(),
     (pty_send_str(s, "a"), pty_wait_for(s, "Attachments", WAIT_MS),
      pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
VIEW(att_save_prompt,   at_attach_reader(),
     (pty_send_str(s, "a"), pty_wait_for(s, "Attachments", WAIT_MS), pty_settle(s, 200),
      pty_send_key(s, PTY_KEY_ENTER), pty_wait_for(s, "Save as:", WAIT_MS),
      pty_settle(s, SETTLE_MS)))
VIEW(att_saved,         at_attach_reader(),
     (pty_send_str(s, "a"), pty_wait_for(s, "Attachments", WAIT_MS), pty_settle(s, 200),
      pty_send_key(s, PTY_KEY_ENTER), pty_wait_for(s, "Save as:", WAIT_MS), pty_settle(s, 200),
      pty_send_key(s, PTY_KEY_ENTER), pty_wait_for(s, "Saved:", WAIT_MS),
      pty_settle(s, SETTLE_MS)))
VIEW(att_save_all,      at_attach_reader(),
     (pty_send_str(s, "A"), pty_settle(s, SETTLE_MS)))

VIEW(review_full,       at_list(), (to_review_full(s, "ann@example.org"), (void)0))
VIEW(review_long_to,    at_list(),
     (to_review_full(s, "annabelle.longname@example.org; bartholomew.longname@example.org; "
                        "cornelia.longname@example.org; dominic.longname@example.org"), (void)0))
VIEW(review_attach_ask, at_list(),
     (to_review_full(s, "ann@example.org"), pty_send_str(s, "a"),
      pty_wait_for(s, "Add Attachment", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(review_attach_removed, at_list(),
     (to_review_full(s, "ann@example.org"), pty_send_str(s, "d"), pty_settle(s, SETTLE_MS)))
VIEW(review_send_failed, at_list(),
     (to_review_full(s, "ann@example.org"), pty_send_str(s, "s"),
      pty_wait_for(s, "Saved to Drafts", WAIT_MS), pty_settle(s, SETTLE_MS)))

VIEW(dd_many,           at_list(), (compose_type_to(s, "ann"), (void)0))
VIEW(dd_one,            at_list(), (compose_type_to(s, "bart"), (void)0))
VIEW(dd_second,         at_list(),
     (compose_type_to(s, "ann"), pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
VIEW(dd_none,           at_list(), (compose_type_to(s, "zzz"), (void)0))
VIEW(dd_wide,           at_list(), (compose_type_to(s, "yam"), (void)0))
VIEW(dd_accepted,       at_list(),
     (compose_type_to(s, "ann"), pty_send_key(s, PTY_KEY_DOWN), pty_send_key(s, PTY_KEY_ENTER),
      pty_settle(s, SETTLE_MS)))
VIEW(path_many,         at_list(), (compose_type_path(s, "al"), (void)0))
VIEW(path_one,          at_list(), (compose_type_path(s, "be"), (void)0))

VIEW(err_connect,       at_error(1), (void)0)
VIEW(err_login,         at_error(2), (void)0)
VIEW(err_tls,           at_error(3), (void)0)
VIEW(list_syncing,      at_cron_list(),
     (pty_send_str(s, "s"), pty_wait_for(s, "syncing", WAIT_MS), pty_settle(s, 100)))
VIEW(list_new_mail,     at_cron_list(),
     (pty_send_str(s, "s"), pty_wait_for(s, "syncing", WAIT_MS), pty_settle(s, 3000),
      pty_send_key(s, PTY_KEY_DOWN), pty_wait_for(s, "New mail", WAIT_MS),
      pty_settle(s, SETTLE_MS)))
VIEW(list_star_feedback, at_list(),
     (pty_send_str(s, "f"), pty_settle(s, SETTLE_MS)))

static const char *const W_IMAP[] = { "1", "mail.example.org", "", "alice", "secret", "", "" };
static const char *const W_SMTP[] = { "1", "mail.example.org", "", "alice", "secret", "", "smtp.example.org" };
static const char *const W_GMAIL[] = { "2" };
VIEW(wiz_imap_host,   at_accounts(), (wizard_answers(s, W_IMAP, 1, NULL), (void)0))
VIEW(wiz_imap_port,   at_accounts(), (wizard_answers(s, W_IMAP, 2, NULL), (void)0))
VIEW(wiz_imap_user,   at_accounts(), (wizard_answers(s, W_IMAP, 3, NULL), (void)0))
VIEW(wiz_imap_pass,   at_accounts(), (wizard_answers(s, W_IMAP, 4, NULL), (void)0))
VIEW(wiz_imap_masked, at_accounts(), (wizard_answers(s, W_IMAP, 4, "secret"), (void)0))
VIEW(wiz_imap_folder, at_accounts(), (wizard_answers(s, W_IMAP, 5, NULL), (void)0))
VIEW(wiz_smtp_host,   at_accounts(), (wizard_answers(s, W_IMAP, 6, NULL), (void)0))
VIEW(wiz_smtp_port,   at_accounts(), (wizard_answers(s, W_SMTP, 7, NULL), (void)0))
VIEW(wiz_imap_done,   at_accounts(), (wizard_answers(s, W_IMAP, 7, NULL), pty_settle(s, 2000)))
VIEW(wiz_gmail,       at_accounts(), (wizard_answers(s, W_GMAIL, 1, NULL), (void)0))
VIEW(wiz_bad_type,    at_accounts(),
     (pty_send_str(s, "n"), pty_wait_for(s, "Account type", WAIT_MS), pty_settle(s, 300),
      pty_send_str(s, "9"), pty_send_key(s, PTY_KEY_ENTER), pty_settle(s, SETTLE_MS)))

VIEW(rules_help,        (write_rules(), at_rules()),
     (pty_send_str(s, "h"), pty_wait_for(s, "Rules shortcuts", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(rule_detail_help,  (write_rules(), at_rules()),
     (pty_send_key(s, PTY_KEY_ENTER), pty_settle(s, SETTLE_MS), pty_send_str(s, "h"),
      pty_wait_for(s, "Rule shortcuts", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(rule_when_help,    (remove_rules(), at_rules()),
     (pty_send_str(s, "a"), pty_wait_for(s, "Add new rule", WAIT_MS), pty_send_str(s, "h"),
      pty_wait_for(s, "Conditions shortcuts", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(rules_delete_ask,  (write_rules(), at_rules()),
     (pty_send_str(s, "d"), pty_wait_for(s, "(y/N)", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(rules_deleted,     (write_rules(), at_rules()),
     (pty_send_str(s, "d"), pty_wait_for(s, "(y/N)", WAIT_MS), pty_send_str(s, "y"),
      pty_settle(s, SETTLE_MS * 2)))
VIEW(rule_detail_delete_ask, (write_rules(), at_rules()),
     (pty_send_key(s, PTY_KEY_ENTER), pty_settle(s, SETTLE_MS), pty_send_str(s, "d"),
      pty_wait_for(s, "(y/N)", WAIT_MS), pty_settle(s, SETTLE_MS)))
VIEW(rule_when_typed,   (remove_rules(), at_rules()),
     (pty_send_str(s, "a"), pty_wait_for(s, "Add new rule", WAIT_MS), pty_send_str(s, "a"),
      pty_settle(s, 300), pty_send_str(s, "from:*@example.org"), pty_settle(s, SETTLE_MS)))
VIEW(rule_when_added,   (remove_rules(), at_rules()),
     (pty_send_str(s, "a"), pty_wait_for(s, "Add new rule", WAIT_MS), pty_send_str(s, "a"),
      pty_settle(s, 300), pty_send_str(s, "from:*@example.org"), pty_send_key(s, PTY_KEY_ENTER),
      pty_settle(s, SETTLE_MS)))
VIEW(rule_when_bad,     (remove_rules(), at_rules()),
     (pty_send_str(s, "a"), pty_wait_for(s, "Add new rule", WAIT_MS), pty_send_str(s, "a"),
      pty_settle(s, 300), pty_send_str(s, "((("), pty_send_key(s, PTY_KEY_ENTER),
      pty_settle(s, SETTLE_MS)))
VIEW(rule_step2_field,  (remove_rules(), at_rules()),
     (pty_send_str(s, "a"), pty_wait_for(s, "Add new rule", WAIT_MS), pty_send_str(s, "q"),
      pty_wait_for(s, "step 2", WAIT_MS), pty_settle(s, 300), pty_send_str(s, "Work mail"),
      pty_send_key(s, PTY_KEY_ENTER), pty_settle(s, SETTLE_MS)))

VIEW(w80_accounts, at_cols(80, at_accounts), (void)0)
VIEW(w80_folders, at_cols(80, at_folders), (void)0)
VIEW(w80_list, at_cols(80, at_list), (void)0)
VIEW(w80_reader, at_cols(80, at_reader), (void)0)
VIEW(w80_list_help, at_cols(80, at_list_help), (void)0)
VIEW(w80_compose_form, at_cols(80, at_compose_form), (void)0)
VIEW(w80_body_editor, at_cols(80, at_compose_editor), (void)0)
VIEW(w80_review, at_cols(80, at_review_screen), (void)0)
VIEW(w80_rules, at_cols(80, at_rules_written), (void)0)
VIEW(w80_review_long_to, at_cols(80, at_review_long_to), (void)0)
VIEW(w140_accounts, at_cols(140, at_accounts), (void)0)
VIEW(w140_folders, at_cols(140, at_folders), (void)0)
VIEW(w140_list, at_cols(140, at_list), (void)0)
VIEW(w140_reader, at_cols(140, at_reader), (void)0)
VIEW(w140_list_help, at_cols(140, at_list_help), (void)0)
VIEW(w140_compose_form, at_cols(140, at_compose_form), (void)0)
VIEW(w140_body_editor, at_cols(140, at_compose_editor), (void)0)
VIEW(w140_review, at_cols(140, at_review_screen), (void)0)
VIEW(w140_rules, at_cols(140, at_rules_written), (void)0)
VIEW(w140_review_long_to, at_cols(140, at_review_long_to), (void)0)
VIEW(list_third_row, at_list(), (pty_send_key(s, PTY_KEY_DOWN), pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
VIEW(folders_second_row, at_folders(), (pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
VIEW(list_wide_subject, at_subject("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe4\xbb\xb6\xe5\x90\x8d with mixed \xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88", 0), (void)0)
VIEW(reader_wide_subject, at_subject("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe4\xbb\xb6\xe5\x90\x8d with mixed \xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88", 1), (void)0)
VIEW(list_long_subject, at_subject("A very long subject line that cannot fit the subject column of the message list and must be cut with an ellipsis at the column edge", 0), (void)0)
VIEW(accounts_8colour, at_term("linux", at_accounts), (void)0)
VIEW(folders_8colour, at_term("linux", at_folders), (void)0)
VIEW(list_8colour, at_term("linux", at_list), (void)0)
VIEW(list_8colour_third, at_term("linux", at_list), (pty_send_key(s, PTY_KEY_DOWN), pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
VIEW(reader_8colour, at_term("linux", at_reader), (void)0)

VIEW(body_editor,       at_list(), (to_editor(s), (void)0))
VIEW(body_editor_typed, at_list(), (editor_typed(s), (void)0))
VIEW(review,            at_list(), (to_review(s), (void)0))
VIEW(review_discard_ask, at_list(),
     (to_review(s), pty_send_str(s, "q"), pty_wait_for(s, "Discard draft?", WAIT_MS),
      pty_settle(s, SETTLE_MS)))

GVIEW(gmail_labels, at_gmail_labels(), (void)0)
GVIEW(gmail_unread,     at_gmail_entry(0), (void)0)
GVIEW(gmail_flagged,    at_gmail_entry(1), (void)0)
GVIEW(gmail_inbox,      at_gmail_entry(2), (void)0)
GVIEW(gmail_inbox_second, at_gmail_entry(2),
      (pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_inbox_d,    at_gmail_entry(2),
      (pty_send_str(s, "d"), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_inbox_trash_mark, at_gmail_entry(2),
      (pty_send_str(s, "D"), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_inbox_archive_mark, at_gmail_entry(2),
      (pty_send_str(s, "a"), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_inbox_marked, at_gmail_entry(2),
      (pty_send_str(s, " "), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_inbox_d_off, at_gmail_entry(2),
      (pty_send_str(s, "d"), pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_inbox_trash_off, at_gmail_entry(2),
      (pty_send_str(s, "D"), pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_inbox_archive_off, at_gmail_entry(2),
      (pty_send_str(s, "a"), pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_starred_filled, at_gmail_entry(2),
      (pty_send_str(s, "f"), pty_settle(s, SETTLE_MS), pty_send_key(s, PTY_KEY_ESC),
       pty_wait_for(s, "Labels", WAIT_MS), pty_settle(s, SETTLE_MS), gmail_open_entry(s, 1)))
GVIEW(gmail_trash_filled, at_gmail_entry(2),
      (pty_send_str(s, "D"), pty_settle(s, SETTLE_MS), pty_send_key(s, PTY_KEY_ESC),
       pty_wait_for(s, "Labels", WAIT_MS), pty_settle(s, SETTLE_MS), gmail_open_entry(s, 7)))
GVIEW(gmail_archive_filled, at_gmail_entry(2),
      (pty_send_str(s, "a"), pty_settle(s, SETTLE_MS), pty_send_key(s, PTY_KEY_ESC),
       pty_wait_for(s, "Labels", WAIT_MS), pty_settle(s, SETTLE_MS), gmail_open_entry(s, 5)))
/* A label exists in the browser once a message carries it: add "Work" to the
 * first message through the bulk-action prompt (m, l). */
static void gmail_add_work_label(PtySession *s) {
    pty_send_str(s, "m");
    pty_wait_for(s, "l=+label", WAIT_MS);
    pty_send_str(s, "l");
    pty_wait_for(s, "Add label", WAIT_MS);
    pty_send_str(s, "Work");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_wait_for(s, "Labels", WAIT_MS);
    pty_settle(s, SETTLE_MS);
}

GVIEW(gmail_label_user, at_gmail_entry(2),
      (gmail_add_work_label(s), (void)0))
GVIEW(gmail_user_label, at_gmail_entry(2),
      (gmail_add_work_label(s), gmail_open_entry(s, 2)))

GVIEW(gmail_picker,       at_gmail_entry(2),
      (pty_send_str(s, "t"), pty_wait_for(s, "Toggle Labels", WAIT_MS), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_picker_second, at_gmail_entry(2),
      (pty_send_str(s, "t"), pty_wait_for(s, "Toggle Labels", WAIT_MS),
       pty_send_key(s, PTY_KEY_DOWN), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_picker_toggled, at_gmail_entry(2),
      (pty_send_str(s, "t"), pty_wait_for(s, "Toggle Labels", WAIT_MS),
       pty_send_key(s, PTY_KEY_DOWN), pty_send_str(s, " "), pty_settle(s, SETTLE_MS)))
GVIEW(gmail_picker_done,  at_gmail_entry(2),
      (pty_send_str(s, "t"), pty_wait_for(s, "Toggle Labels", WAIT_MS),
       pty_send_key(s, PTY_KEY_DOWN), pty_send_str(s, " "), pty_settle(s, 200),
       pty_send_key(s, PTY_KEY_ENTER), pty_wait_for(s, "Label removed", WAIT_MS),
       pty_settle(s, SETTLE_MS)))

GVIEW(gmail_archive,    at_gmail_entry(5), (void)0)
GVIEW(gmail_trash,      at_gmail_entry(7), (void)0)


int main(int argc, char *argv[]) {
    printf("--- email-tui screens (cell-exact) ---\n\n");
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <email-tui> <mock-imap-server>\n", argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(g_tui_bin, sizeof(g_tui_bin), "%s", argv[1]);
    snprintf(g_mock_bin, sizeof(g_mock_bin), "%s", argv[2]);
    if (argc >= 5) {
        snprintf(g_sync_bin, sizeof(g_sync_bin), "%s", argv[3]);
        snprintf(g_gmail_bin, sizeof(g_gmail_bin), "%s", argv[4]);
    }

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
    RUN_TEST(test_screen_account_delete_ask);
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
    RUN_TEST(test_screen_body_editor);
    RUN_TEST(test_screen_body_editor_typed);
    RUN_TEST(test_screen_review);
    RUN_TEST(test_screen_w80_review_long_to);
    RUN_TEST(test_screen_w140_review_long_to);
    RUN_TEST(test_screen_review_discard_ask);
    RUN_TEST(test_screen_w80_accounts);
    RUN_TEST(test_screen_w80_folders);
    RUN_TEST(test_screen_w80_list);
    RUN_TEST(test_screen_w80_reader);
    RUN_TEST(test_screen_w80_list_help);
    RUN_TEST(test_screen_w80_compose_form);
    RUN_TEST(test_screen_w80_body_editor);
    RUN_TEST(test_screen_w80_review);
    RUN_TEST(test_screen_w80_rules);
    RUN_TEST(test_screen_w140_accounts);
    RUN_TEST(test_screen_w140_folders);
    RUN_TEST(test_screen_w140_list);
    RUN_TEST(test_screen_w140_reader);
    RUN_TEST(test_screen_w140_list_help);
    RUN_TEST(test_screen_w140_compose_form);
    RUN_TEST(test_screen_w140_body_editor);
    RUN_TEST(test_screen_w140_review);
    RUN_TEST(test_screen_w140_rules);
    RUN_TEST(test_screen_list_third_row);
    RUN_TEST(test_screen_folders_second_row);
    RUN_TEST(test_screen_list_wide_subject);
    RUN_TEST(test_screen_reader_wide_subject);
    RUN_TEST(test_screen_list_long_subject);
    RUN_TEST(test_screen_accounts_8colour);
    RUN_TEST(test_screen_folders_8colour);
    RUN_TEST(test_screen_list_8colour);
    RUN_TEST(test_screen_list_8colour_third);
    RUN_TEST(test_screen_reader_8colour);
    RUN_TEST(test_screen_rules_help);
    RUN_TEST(test_screen_rule_detail_help);
    RUN_TEST(test_screen_rule_when_help);
    RUN_TEST(test_screen_rules_delete_ask);
    RUN_TEST(test_screen_rules_deleted);
    RUN_TEST(test_screen_rule_detail_delete_ask);
    RUN_TEST(test_screen_rule_when_typed);
    RUN_TEST(test_screen_rule_when_added);
    RUN_TEST(test_screen_rule_when_bad);
    RUN_TEST(test_screen_rule_step2_field);
    RUN_TEST(test_screen_wiz_imap_host);
    RUN_TEST(test_screen_wiz_imap_port);
    RUN_TEST(test_screen_wiz_imap_user);
    RUN_TEST(test_screen_wiz_imap_pass);
    RUN_TEST(test_screen_wiz_imap_masked);
    RUN_TEST(test_screen_wiz_imap_folder);
    RUN_TEST(test_screen_wiz_smtp_host);
    RUN_TEST(test_screen_wiz_smtp_port);
    RUN_TEST(test_screen_wiz_imap_done);
    RUN_TEST(test_screen_wiz_gmail);
    RUN_TEST(test_screen_wiz_bad_type);
    RUN_TEST(test_screen_err_connect);
    RUN_TEST(test_screen_err_login);
    RUN_TEST(test_screen_err_tls);
    RUN_TEST(test_screen_list_syncing);
    RUN_TEST(test_screen_list_new_mail);
    RUN_TEST(test_screen_list_star_feedback);
    RUN_TEST(test_screen_dd_many);
    RUN_TEST(test_screen_dd_one);
    RUN_TEST(test_screen_dd_second);
    RUN_TEST(test_screen_dd_none);
    RUN_TEST(test_screen_dd_wide);
    RUN_TEST(test_screen_dd_accepted);
    RUN_TEST(test_screen_path_many);
    RUN_TEST(test_screen_path_one);
    RUN_TEST(test_screen_review_full);
    RUN_TEST(test_screen_review_long_to);
    RUN_TEST(test_screen_review_attach_ask);
    RUN_TEST(test_screen_review_attach_removed);
    RUN_TEST(test_screen_review_send_failed);
    RUN_TEST(test_screen_att_reader);
    RUN_TEST(test_screen_att_picker);
    RUN_TEST(test_screen_att_picker_second);
    RUN_TEST(test_screen_att_save_prompt);
    RUN_TEST(test_screen_att_saved);
    RUN_TEST(test_screen_att_save_all);
    RUN_TEST(test_screen_list_marked);
    RUN_TEST(test_screen_list_marked_two);
    RUN_TEST(test_screen_list_bulk_prompt);
    RUN_TEST(test_screen_list_bulk_cursor_prompt);
    RUN_TEST(test_screen_list_move_prompt);
    RUN_TEST(test_screen_list_delete_ask);
    RUN_TEST(test_screen_list_bulk_done);
    RUN_TEST(test_screen_gmail_labels);
    RUN_TEST(test_screen_gmail_unread);
    RUN_TEST(test_screen_gmail_flagged);
    RUN_TEST(test_screen_gmail_inbox);
    RUN_TEST(test_screen_gmail_inbox_second);
    RUN_TEST(test_screen_gmail_inbox_d);
    RUN_TEST(test_screen_gmail_inbox_trash_mark);
    RUN_TEST(test_screen_gmail_inbox_archive_mark);
    RUN_TEST(test_screen_gmail_inbox_marked);
    RUN_TEST(test_screen_gmail_archive);
    RUN_TEST(test_screen_gmail_picker);
    RUN_TEST(test_screen_gmail_picker_second);
    RUN_TEST(test_screen_gmail_picker_toggled);
    RUN_TEST(test_screen_gmail_picker_done);
    RUN_TEST(test_screen_gmail_inbox_d_off);
    RUN_TEST(test_screen_gmail_inbox_trash_off);
    RUN_TEST(test_screen_gmail_inbox_archive_off);
    RUN_TEST(test_screen_gmail_starred_filled);
    RUN_TEST(test_screen_gmail_trash_filled);
    RUN_TEST(test_screen_gmail_archive_filled);
    RUN_TEST(test_screen_gmail_label_user);
    RUN_TEST(test_screen_gmail_user_label);
    RUN_TEST(test_screen_gmail_trash);

    if (g_record) fclose(g_record);
    stop_gmail();
    if (g_mock_pid > 0) { kill(g_mock_pid, SIGKILL); waitpid(g_mock_pid, NULL, 0); }

    printf("\n--- PTY Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
