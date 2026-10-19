/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-04-19 */
/* PTY tests of the Gmail interactions of the TUI. */
/**
 * @file test_pty_gmail_tui.c
 * @brief PTY tests for Gmail TUI interactions.
 *
 * Covers:
 *   - 'd' key pending-remove visual feedback (pending marker character)
 *
 * Usage: test-pty-gmail-tui <email-tui> <email-sync> <mock-gmail-server>
 *
 * The mock Gmail API server is started on a port the kernel picks; the environment
 * variable GMAIL_API_BASE_URL is set so that both email-sync and email-tui
 * talk to the mock instead of the real Gmail API.
 *
 * Message ordering in the list:
 *   The TUI sorts entries by date descending (then UID descending as
 *   tiebreaker).  The mock server's 5 messages all share the same date,
 *   so they are ordered by UID descending: Message 5, 4, 3, 2, 1.
 *   Cursor starts at position 0 → "Message 5".
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ptytest.h"
#include "pty_assert.h"
#include "test_scratch.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>

int g_tests_run    = 0;
int g_tests_failed = 0;

#define RUN_TEST(fn) do { if (!test_selected(#fn)) break; printf("  %s...\n", #fn); fn(); } while(0)

/* ── Configuration ───────────────────────────────────────────────────── */

#define GMAIL_PTY_COUNT  5
#define GMAIL_TEST_EMAIL "ptygtest@gmail.com"
/* Top-most message in the list (sorted by UID descending, same date) */
#define GMAIL_TOP_MSG    "Message 5"
/* Bottom-most message (lowest UID) */
#define GMAIL_BOT_MSG    "Message 1"
#define WAIT_MS  8000
#define SETTLE_MS 700
#define ROWS 30
#define COLS 120
#define FEEDBACK_ROW (ROWS - 1)   /* 0-indexed PTY row of the status line */

/* The keys of the keyboard policy (EMAIL-175): the registry keys act at once
 * ('*' star, '+' read, '-' unread), 'd' asks before it deletes, and the rest
 * of the actions that change state are in the ':' menu.  The rows of the
 * Gmail message list menu, in table order (libemail/src/core/key_table.c). */
#define GM_MENU_ARCHIVE   9
#define GM_MENU_RESTORE   10
#define GM_MENU_RM_LABEL  11

static void gm_menu_pick(PtySession *s, int row) {
    pty_send_str(s, ":");
    pty_wait_for(s, "Actions", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    for (int i = 0; i < row; i++) pty_send_key(s, PTY_KEY_DOWN);
    pty_send_key(s, PTY_KEY_ENTER);
}
static void gm_archive(PtySession *s)   { gm_menu_pick(s, GM_MENU_ARCHIVE); }
static void gm_restore(PtySession *s)   { gm_menu_pick(s, GM_MENU_RESTORE); }
static void gm_rm_label(PtySession *s)  { gm_menu_pick(s, GM_MENU_RM_LABEL); }
/* 'd', then Yes to the dialog that names the message. */
static void gm_trash(PtySession *s) {
    pty_send_str(s, "d");
    pty_wait_for(s, "Yes", WAIT_MS);      /* the dialog names the message */
    pty_send_str(s, "y");
}


/* ── Global state ────────────────────────────────────────────────────── */

/* The selected row is drawn by the table widget with the bright variant of a
 * colour (hue kept, bold added): a colour matches in either form. */
static int fg_is(int fg, int base) { return fg == base || fg == base + 60; }

/* The cursor row: ">" in column 0 on the dark blue selection background. */
#define SELECTION_BG PTY_COLOR_IDX(17)

static pid_t g_mock_pid = -1;
static int   g_mock_port = 0;   /* picked by the kernel, published by the mock */
static char  g_test_home[256];
static char  g_tui_bin[512];
static char  g_sync_bin[512];
static char  g_mock_bin[512];
static char  g_api_url[256];

/* ── Infrastructure ──────────────────────────────────────────────────── */

static void write_gmail_config(void) {
    char d1[350], d2[400], d3[450], path[500];
    snprintf(d1,   sizeof(d1),   "%s/.config/email-cli/accounts",              g_test_home);
    snprintf(d2,   sizeof(d2),   "%s/.config/email-cli/accounts/%s",           g_test_home, GMAIL_TEST_EMAIL);
    snprintf(path, sizeof(path), "%s/.config/email-cli/accounts/%s/config.ini",g_test_home, GMAIL_TEST_EMAIL);

    /* Ensure directory chain exists */
    snprintf(d3, sizeof(d3), "%s/.config", g_test_home);
    mkdir(g_test_home, 0700);
    mkdir(d3,           0700);
    snprintf(d3, sizeof(d3), "%s/.config/email-cli", g_test_home);
    mkdir(d3, 0700);
    mkdir(d1, 0700);
    mkdir(d2, 0700);

    FILE *fp = fopen(path, "w");
    if (!fp) { perror("fopen config.ini"); return; }
    fprintf(fp,
        "GMAIL_MODE=1\n"
        "EMAIL_USER=%s\n"
        "GMAIL_ACCESS_TOKEN=fake_token\n"
        "GMAIL_REFRESH_TOKEN=fake_refresh\n"
        "GMAIL_TOKEN_EXPIRY=9999999999\n"
        "EMAIL_FOLDER=INBOX\n",
        GMAIL_TEST_EMAIL);
    fclose(fp);
    chmod(path, 0600);
}

static int start_gmail_mock(void) {
    /* The mock's scenario is its environment; the port is the kernel's pick,
     * read back from the file the mock publishes it in. */
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", GMAIL_PTY_COUNT);
    setenv("MOCK_GMAIL_COUNT", count_str, 1);
    setenv("MOCK_GMAIL_EMAIL", GMAIL_TEST_EMAIL, 1);
    g_mock_pid = test_spawn_mock(g_mock_bin, "MOCK_GMAIL", 0, &g_mock_port);
    unsetenv("MOCK_GMAIL_COUNT");
    unsetenv("MOCK_GMAIL_EMAIL");
    return g_mock_pid < 0 ? -1 : 0;
}

static void stop_gmail_mock(void) {
    if (g_mock_pid > 0) {
        kill(g_mock_pid, SIGKILL);
        waitpid(g_mock_pid, NULL, 0);
        g_mock_pid = -1;
    }
}

/** Run email-sync synchronously (inherits HOME + GMAIL_API_BASE_URL from env). */
static int run_gmail_sync(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execl(g_sync_bin, "email-sync", (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/**
 * Persist the per-account folder cursor in the shared UI prefs file.
 *
 * email-tui reads "folder_cursor_<account>" on startup and pre-selects that
 * folder in the label browser; with no entry it defaults to the __unread__
 * virtual label.
 */
static void write_folder_pref(const char *folder) {
    char dir[400], path[450];
    snprintf(dir, sizeof(dir), "%s/.local", g_test_home);              mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.local/share", g_test_home);        mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.local/share/email-cli", g_test_home); mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/ui.ini", dir);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "folder_cursor_%s=%s\n", GMAIL_TEST_EMAIL, folder);
    fclose(fp);
}

/**
 * Reset account local data and re-populate from the mock server.
 *
 * Deletes the account's local store (labels/, headers/, history ID, etc.)
 * so the next email-sync performs a full fresh sync instead of an
 * incremental one.  This ensures every test starts from the same clean
 * state (all 5 messages in INBOX with their original labels).
 */
static int reset_and_sync(void) {
    /* Remove the per-account local store */
    char store[600];
    snprintf(store, sizeof(store), "%s/.local/share/email-cli/accounts/%s",
             g_test_home, GMAIL_TEST_EMAIL);
    if (test_rmtree(store) != 0) {
        fprintf(stderr, "gmail-tui: cannot wipe %s\n", store);
        g_tests_failed++;
        return -1;
    }

    /* Also clear the shared UI prefs file (it remembers the last-used folder
     * cursor; stale entries would make subsequent tests open the wrong label). */
    char ui_ini[600];
    snprintf(ui_ini, sizeof(ui_ini), "%s/.local/share/email-cli/ui.ini",
             g_test_home);
    if (unlink(ui_ini) != 0 && errno != ENOENT) {
        fprintf(stderr, "gmail-tui: cannot remove %s: %s\n", ui_ini, strerror(errno));
        g_tests_failed++;
        return -1;
    }

    /* Pin the folder cursor to INBOX.  Without a saved preference email-tui
     * opens the __unread__ virtual label instead, and navigate_to_inbox()
     * would land on a virtual view where label operations ('d') are no-ops. */
    write_folder_pref("INBOX");

    return run_gmail_sync();
}

/** Find the first screen row containing @p text; returns -1 if absent. */
static int find_row(PtySession *s, const char *text) {
    for (int r = 0; r < ROWS; r++)
        if (pty_row_contains(s, r, text)) return r;
    return -1;
}

/**
 * Navigate email-tui from startup to the INBOX message list.
 *
 * The TUI presents three screens in sequence:
 *   1. Account selector — wait for "Email Accounts", press Enter
 *   2. Label browser    — wait for "Labels",          press Enter (INBOX pre-selected)
 *   3. Message list     — wait for GMAIL_TOP_MSG ("Message 5", the topmost entry)
 *
 * Returns 0 on success, -1 if any step times out.
 */
static int navigate_to_inbox(PtySession *s) {
    /* Step 1: account selector */
    if (pty_wait_for(s, "Email Accounts", WAIT_MS) != 0) {
        printf("  [FAIL] navigate_to_inbox: timed out waiting for \"Email Accounts\"\n");
        return -1;
    }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);  /* open the Gmail account */

    /* Step 2: label browser (INBOX pre-selected via EMAIL_FOLDER=INBOX) */
    if (pty_wait_for(s, "Labels", WAIT_MS) != 0) {
        printf("  [FAIL] navigate_to_inbox: timed out waiting for \"Labels\"\n");
        return -1;
    }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);  /* open INBOX */

    /* Step 3: message list (cursor starts at top = highest UID = GMAIL_TOP_MSG) */
    if (pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS) != 0) {
        printf("  [FAIL] navigate_to_inbox: timed out waiting for \"%s\"\n",
               GMAIL_TOP_MSG);
        return -1;
    }
    pty_settle(s, SETTLE_MS);

    return 0;
}

/* ── Navigation helpers ─────────────────────────────────────────────── */

/**
 * From the accounts screen, navigate to the Trash label view.
 * Trash is always the last entry in the label browser (PTY_KEY_END).
 */
static int navigate_to_trash(PtySession *s) {
    if (pty_wait_for(s, "Email Accounts", WAIT_MS) != 0) {
        printf("  [FAIL] navigate_to_trash: timed out at \"Email Accounts\"\n");
        return -1;
    }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Labels", WAIT_MS) != 0) {
        printf("  [FAIL] navigate_to_trash: timed out at \"Labels\"\n");
        return -1;
    }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_END);   /* Trash is always the last entry */
    pty_settle(s, SETTLE_MS / 2);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    return 0;
}

/**
 * Within an already-visible Labels browser, navigate to Archive (_nolabel).
 * Label order (no user labels in test env): …DRAFTS | Archive Spam Trash
 * Archive = End − 2 steps.
 */
static int navigate_labels_to_archive(PtySession *s) {
    pty_send_key(s, PTY_KEY_END);   /* → Trash (last) */
    pty_settle(s, SETTLE_MS / 2);
    pty_send_key(s, PTY_KEY_UP);    /* → Spam */
    pty_send_key(s, PTY_KEY_UP);    /* → Archive */
    pty_settle(s, SETTLE_MS / 2);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'd' key → pending-remove row remains visible
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * After pressing 'd' (remove current label) in the Gmail TUI, the message
 * must NOT be removed from the list immediately.  The row stays visible so
 * the user can see what they acted on and undo if needed.
 *
 * The cursor starts on GMAIL_TOP_MSG ("Message 5"), which is the first
 * displayed entry (highest UID, sorted newest-first).
 */
static void test_d_key_row_stays_visible(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "d-vis: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "d-vis: pty_run");

    /* Navigate through account selector and label browser to message list */
    ASSERT(navigate_to_inbox(s) == 0, "d-vis: navigate_to_inbox");

    /* Press 'd' — removes current label (INBOX) from the cursor row */
    gm_rm_label(s);
    pty_settle(s, SETTLE_MS);

    /* The cursor row (GMAIL_TOP_MSG) must still be visible after label removal */
    ASSERT_SCREEN_CONTAINS(s, GMAIL_TOP_MSG);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'd' key → pending-remove row has pending marker character
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * The pending-remove row must be rendered with a visible 'd' marker at
 * column 0 so the user immediately sees that the label removal is queued.
 *
 * The cursor row (GMAIL_TOP_MSG = "Message 5") is the one that gets the
 * marker after 'd' is pressed.
 */
static void test_d_key_row_strikethrough(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "d-strike: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "d-strike: pty_run");

    ASSERT(navigate_to_inbox(s) == 0, "d-strike: navigate_to_inbox");

    /* The cursor row is GMAIL_TOP_MSG (the first/topmost entry in the list) */
    int row_before = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_before >= 0, "d-strike: " GMAIL_TOP_MSG " present before d");

    /* Check that the cursor row does NOT have a pending marker BEFORE pressing 'd' */
    if (row_before >= 0) {
        ASSERT(strcmp(pty_cell_text(s, row_before, 1), " ") == 0,
               "d-strike: no marker before d");
    }

    /* Press 'd' — removes INBOX label from the cursor row */
    gm_rm_label(s);
    pty_settle(s, SETTLE_MS);

    /* The cursor row must still be visible (pending-remove keeps it) */
    int row_after = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_after >= 0, "d-strike: " GMAIL_TOP_MSG " still present after d");

    if (row_after >= 0) {
        /* The pending-remove row is rendered with a yellow 'd' marker at col 0. */
        ASSERT(strcmp(pty_cell_text(s, row_after, 1), "d") == 0,
               "d-strike: cursor row has 'd' marker after d");
    }

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'd' key → pending-remove row retains inverse cursor highlight
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * After pressing 'd', the cursor row is rendered with a yellow 'd' marker
 * AND inverse-video so the cursor position remains clearly visible.
 * The selection background (palette 17) must stay on the row alongside the pending marker.
 *
 * The cursor starts on GMAIL_TOP_MSG ("Message 5").
 */
static void test_d_key_cursor_row_has_reverse(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "d-rev: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "d-rev: pty_run");

    ASSERT(navigate_to_inbox(s) == 0, "d-rev: navigate_to_inbox");

    int row_before = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_before >= 0, "d-rev: " GMAIL_TOP_MSG " present before d");

    /* Before 'd': cursor row has the selection background, no pending marker */
    if (row_before >= 0) {
        ASSERT(pty_cell_bg(s, row_before, 4) == SELECTION_BG,
               "d-rev: cursor row has the selection background before d");
        ASSERT(strcmp(pty_cell_text(s, row_before, 1), " ") == 0,
               "d-rev: no marker before d");
    }

    gm_rm_label(s);
    pty_settle(s, SETTLE_MS);

    int row_after = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_after >= 0, "d-rev: " GMAIL_TOP_MSG " still present after d");

    /* After 'd': cursor row keeps the selection background and has the 'd' marker */
    if (row_after >= 0) {
        ASSERT(pty_cell_bg(s, row_after, 4) == SELECTION_BG,
               "d-rev: cursor row has the selection background after d");
        ASSERT(strcmp(pty_cell_text(s, row_after, 1), "d") == 0,
               "d-rev: cursor row has 'd' marker after d");
    }

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: second 'd' undoes the pending-remove (toggle)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Pressing 'd' twice on the same row must cancel the pending-remove:
 *   - After first 'd': row has yellow 'd' pending marker at col 0.
 *   - After second 'd': marker is gone; label is restored locally;
 *     pressing 'R' (refresh) must keep the row visible.
 */
static void test_d_key_toggle_undo(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "d-undo: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "d-undo: pty_run");

    ASSERT(navigate_to_inbox(s) == 0, "d-undo: navigate_to_inbox");

    /* First 'd': mark for removal */
    gm_rm_label(s);
    pty_settle(s, SETTLE_MS);

    int row1 = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row1 >= 0, "d-undo: " GMAIL_TOP_MSG " present after first d");
    if (row1 >= 0)
        ASSERT(strcmp(pty_cell_text(s, row1, 1), "d") == 0,
               "d-undo: 'd' marker set after first d");

    /* Second 'd': undo */
    gm_rm_label(s);
    pty_settle(s, SETTLE_MS);

    int row2 = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row2 >= 0, "d-undo: " GMAIL_TOP_MSG " still present after second d");
    if (row2 >= 0)
        ASSERT(strcmp(pty_cell_text(s, row2, 1), " ") == 0,
               "d-undo: marker cleared after second d");

    /* Refresh — row must survive (label was restored) */
    pty_send_str(s, "U");
    pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_screen_contains(s, GMAIL_TOP_MSG),
           "d-undo: " GMAIL_TOP_MSG " visible after refresh (undo preserved)");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: label picker shows UNREAD
 * ══════════════════════════════════════════════════════════════════════ */

/** Pressing 't' opens the Toggle Labels popup which must list UNREAD. */
static void test_label_picker_shows_unread(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "picker-unread: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "picker-unread: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "picker-unread: navigate_to_inbox");

    pty_send_str(s, "t");       /* open label picker */
    pty_wait_for(s, "Toggle Labels", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "UNREAD");

    pty_send_key(s, PTY_KEY_ESC);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** Space ticks without changing anything; Esc closes the picker and leaves
 *  every label as it was (EMAIL-110); Enter applies the ticked set. */
static void test_label_picker_esc_discards_enter_applies(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "picker-esc: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "picker-esc: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "picker-esc: navigate_to_inbox");

    pty_send_str(s, "t");
    pty_wait_for(s, "Toggle Labels", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[x] INBOX");
    pty_send_key(s, PTY_KEY_DOWN);              /* UNREAD -> INBOX */
    pty_send_str(s, " ");                       /* untick INBOX */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[ ] INBOX");
    pty_send_key(s, PTY_KEY_ESC);               /* discard */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Toggle Labels");
    ASSERT(!pty_row_contains(s, FEEDBACK_ROW, "Label removed"),
           "picker-esc: Esc changes no label");

    pty_send_str(s, "t");                       /* the label is still there */
    pty_wait_for(s, "Toggle Labels", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "[x] INBOX");
    pty_send_key(s, PTY_KEY_DOWN);
    pty_send_str(s, " ");
    pty_send_key(s, PTY_KEY_ENTER);             /* apply */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Toggle Labels");
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Label removed"),
           "picker-esc: Enter applies the unticked label");

    pty_send_key(s, PTY_KEY_ESC);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Label browser: cross-folder search ('/') and label creation ('c')
 * ══════════════════════════════════════════════════════════════════════ */

static PtySession *open_label_browser(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    if (pty_run(s, args) != 0 ||
        pty_wait_for(s, "Email Accounts", WAIT_MS) != 0) {
        pty_close(s);
        return NULL;
    }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Labels", WAIT_MS) != 0) {
        pty_close(s);
        return NULL;
    }
    pty_settle(s, SETTLE_MS);
    return s;
}

static void test_label_browser_search_prompt_and_scopes(void) {
    PtySession *s = open_label_browser();
    ASSERT(s != NULL, "label browser: reaches the label list");
    pty_send_str(s, "/");
    ASSERT_WAIT_FOR(s, "Search [Subject]:", WAIT_MS);
    pty_send_str(s, "abc");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Search [Subject]: abc");
    pty_send_key(s, PTY_KEY_BACK);                 /* Backspace removes one */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Search [Subject]: ab");
    pty_send_key(s, PTY_KEY_TAB);                  /* Tab cycles the scope */
    ASSERT_WAIT_FOR(s, "Search [From]:", WAIT_MS);
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "Search [To]:", WAIT_MS);
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "Search [Body]:", WAIT_MS);
    pty_send_key(s, PTY_KEY_TAB);
    ASSERT_WAIT_FOR(s, "Search [Subject]:", WAIT_MS);   /* wraps around */
    pty_send_key(s, PTY_KEY_ESC);                  /* cancel: back in the browser */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Labels");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_label_browser_search_runs(void) {
    PtySession *s = open_label_browser();
    ASSERT(s != NULL, "label browser: reaches the label list");
    pty_send_str(s, "/");
    ASSERT_WAIT_FOR(s, "Search [Subject]:", WAIT_MS);
    pty_send_str(s, "Message");
    pty_send_key(s, PTY_KEY_ENTER);
    /* A search is a listing of the cached messages that match. */
    ASSERT_WAIT_FOR(s, "message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Search");
    pty_send_key(s, PTY_KEY_ESC);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** EMAIL-104: a label that exists on the server but carries no message is
 *  listed by the label browser after a sync. */
static void test_label_browser_lists_a_label_without_messages(void) {
    PtySession *s = open_label_browser();
    ASSERT(s != NULL, "label browser: reaches the label list");
    ASSERT_SCREEN_CONTAINS(s, "Empty Label");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_label_browser_create_label_prompt(void) {
    PtySession *s = open_label_browser();
    ASSERT(s != NULL, "label browser: reaches the label list");
    gm_menu_pick(s, 0);                            /* Create a label... */
    ASSERT_WAIT_FOR(s, "New label name:", WAIT_MS);
    pty_send_str(s, "PtyMade");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "New label name: PtyMade");
    pty_send_key(s, PTY_KEY_ESC);                  /* cancel: nothing is created */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Labels");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_label_browser_create_label_confirmed(void) {
    PtySession *s = open_label_browser();
    ASSERT(s != NULL, "label browser: reaches the label list");
    gm_menu_pick(s, 0);                            /* Create a label... */
    ASSERT_WAIT_FOR(s, "New label name:", WAIT_MS);
    pty_send_str(s, "PtyMade");
    pty_send_key(s, PTY_KEY_ENTER);
    /* The browser redraws with the label list rebuilt; it is still the
     * label browser, and the new name is among the labels. */
    ASSERT_WAIT_FOR(s, "PtyMade", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Labels");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'a' key → row has yellow marker 'd' (immediate feedback)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * After pressing 'a' (archive), the cursor row must stay visible with
 * a yellow 'd' pending marker at column 0, giving immediate feedback.
 */
static void test_archive_row_has_strikethrough(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "arch-strike: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "arch-strike: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "arch-strike: navigate_to_inbox");

    int row_before = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_before >= 0, "arch-strike: " GMAIL_TOP_MSG " present before a");
    if (row_before >= 0)
        ASSERT(strcmp(pty_cell_text(s, row_before, 1), " ") == 0,
               "arch-strike: no pending marker before a");

    gm_archive(s);
    pty_settle(s, SETTLE_MS);

    int row_after = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_after >= 0, "arch-strike: " GMAIL_TOP_MSG " still visible after a");
    if (row_after >= 0)
        ASSERT(strcmp(pty_cell_text(s, row_after, 1), "d") == 0,
               "arch-strike: 'd' pending marker set after a");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'a' key → message gone from INBOX after refresh
 * ══════════════════════════════════════════════════════════════════════ */

/** After 'a' + 'R', the archived message must no longer appear in INBOX. */
static void test_archive_removes_from_inbox(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "arch-gone: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "arch-gone: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "arch-gone: navigate_to_inbox");

    gm_archive(s);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "U");
    pty_wait_for(s, GMAIL_BOT_MSG, WAIT_MS);  /* wait for list to reload */
    pty_settle(s, SETTLE_MS);

    ASSERT(!pty_screen_contains(s, GMAIL_TOP_MSG),
           "arch-gone: " GMAIL_TOP_MSG " absent from INBOX after R");
    ASSERT(pty_screen_contains(s, GMAIL_BOT_MSG),
           "arch-gone: other messages still present");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'a' key → archived message appears in Archive view
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * After archiving, navigate to Archive (_nolabel) and verify the message is there.
 */
static void test_archive_message_in_archive_view(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "arch-view: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "arch-view: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "arch-view: navigate_to_inbox");

    /* Archive top message */
    gm_archive(s);
    pty_settle(s, SETTLE_MS);
    /* Press 'R' so the inbox index is refreshed and the row is removed */
    pty_send_str(s, "U");
    pty_settle(s, SETTLE_MS);

    /* Go back to label browser */
    pty_send_key(s, PTY_KEY_BACK);
    if (pty_wait_for(s, "Labels", WAIT_MS) != 0) {
        printf("  [FAIL] arch-view: timed out waiting for Labels screen\n");
        pty_close(s); g_tests_failed++; return;
    }
    pty_settle(s, SETTLE_MS);

    /* Navigate to Archive (End → Trash, Up×2 → Archive) */
    navigate_labels_to_archive(s);
    pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT(pty_screen_contains(s, GMAIL_TOP_MSG),
           "arch-view: " GMAIL_TOP_MSG " appears in Archive view");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: label picker → unarchive (add INBOX to archived message)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * After archiving a message, open the Archive view, use the label picker
 * to add INBOX, and verify the message is removed from Archive on refresh.
 */
static void test_label_picker_unarchive(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "unarchive: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "unarchive: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "unarchive: navigate_to_inbox");

    /* Archive Message 5 */
    gm_archive(s);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "U");
    pty_settle(s, SETTLE_MS);

    /* Back to labels */
    pty_send_key(s, PTY_KEY_BACK);
    pty_wait_for(s, "Labels", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Navigate to Archive */
    navigate_labels_to_archive(s);
    pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT(pty_screen_contains(s, GMAIL_TOP_MSG),
           "unarchive: " GMAIL_TOP_MSG " in Archive view before restore");

    /* Open label picker, add INBOX (INBOX is 2nd item: UNREAD is first, Down once) */
    pty_send_str(s, "t");
    pty_wait_for(s, "Toggle Labels", WAIT_MS);
    pty_settle(s, SETTLE_MS / 2);
    pty_send_key(s, PTY_KEY_DOWN);  /* cursor → INBOX */
    pty_send_str(s, " "); /* toggle INBOX on (Space ticks) */
    pty_settle(s, SETTLE_MS / 2);
    pty_send_key(s, PTY_KEY_ENTER);   /* Enter applies the ticked set */
    pty_settle(s, SETTLE_MS);

    /* Refresh Archive view — message must be gone (now in INBOX) */
    pty_send_str(s, "U");
    pty_settle(s, WAIT_MS / 4);
    pty_settle(s, SETTLE_MS);

    ASSERT(!pty_screen_contains(s, GMAIL_TOP_MSG),
           "unarchive: " GMAIL_TOP_MSG " gone from Archive after INBOX added");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: Trash statusbar points at the action menu
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * When viewing the Trash folder, the status bar must show "u restore" to
 * guide the user.  This test works even with an empty Trash.
 */
static void test_trash_statusbar_restore_hint(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "trash-sb: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "trash-sb: pty_run");
    ASSERT(navigate_to_trash(s) == 0, "trash-sb: navigate_to_trash");

    /* The status bar (last row) points at the action menu, where the restore
     * is (EMAIL-175: no direct key writes to the server but the registry's). */
    ASSERT_SCREEN_CONTAINS(s, ": actions");
    ASSERT_SCREEN_CONTAINS(s, "d del");
    ASSERT_SCREEN_NOT_CONTAINS(s, "u restore");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: label browser — Up on the first selectable row is a no-op
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Row 0 of the label browser is the "Tags / Flags" section header, and row 1
 * is the first selectable entry ("Unread").  Header rows are drawn without the
 * selection highlight, so a cursor parked on one is invisible to the user.
 *
 * Holding Up on "Unread" (key auto-repeat) must therefore leave the cursor on
 * "Unread" — the status bar keeps reporting position 2 — instead of walking
 * onto the header at position 1.
 */
static void test_labels_up_at_top_is_noop(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "lbl-up: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "lbl-up: pty_run");

    ASSERT(pty_wait_for(s, "Email Accounts", WAIT_MS) == 0, "lbl-up: accounts screen");
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT(pty_wait_for(s, "Labels", WAIT_MS) == 0, "lbl-up: label browser");
    pty_settle(s, SETTLE_MS);

    /* Go to the first selectable row, then keep pressing Up (auto-repeat). */
    pty_send_key(s, PTY_KEY_HOME);
    pty_settle(s, SETTLE_MS / 2);
    for (int i = 0; i < 8; i++) pty_send_key(s, PTY_KEY_UP);
    pty_settle(s, SETTLE_MS);

    /* Status bar shows a 1-based cursor position: "[2/N]" = first selectable
     * row.  "[1/N]" would mean the cursor sits on the section header. */
    ASSERT_SCREEN_CONTAINS(s, "[2/");
    ASSERT_SCREEN_NOT_CONTAINS(s, "[1/");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'D' key → row has red marker 'D' (immediate feedback)
 * ══════════════════════════════════════════════════════════════════════ */

/** After pressing 'D' (trash), cursor row must show red 'D' pending marker immediately. */
static void test_D_key_row_has_strikethrough(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "D-strike: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "D-strike: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "D-strike: navigate_to_inbox");

    int row_before = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_before >= 0, "D-strike: " GMAIL_TOP_MSG " present before D");
    if (row_before >= 0)
        ASSERT(strcmp(pty_cell_text(s, row_before, 1), " ") == 0,
               "D-strike: no pending marker before D");

    gm_trash(s);
    pty_settle(s, SETTLE_MS);

    int row_after = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_after >= 0, "D-strike: " GMAIL_TOP_MSG " still visible after D");
    if (row_after >= 0)
        ASSERT(strcmp(pty_cell_text(s, row_after, 1), "D") == 0,
               "D-strike: 'D' pending marker set after D");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'u' key → row has green marker '↩' (immediate feedback)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * After pressing 'u' (restore from trash) in the Trash view, the row must
 * remain visible with a green '↩' pending marker at column 0 until 'R'.
 */
static void test_u_key_row_has_strikethrough(void) {
    /* First, trash a message so the Trash view has something */
    const char *args[] = { g_tui_bin, NULL };

    /* Session 1: trash Message 5 */
    {
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "u-strike: pty_open (s1)");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "u-strike: pty_run (s1)");
        ASSERT(navigate_to_inbox(s) == 0, "u-strike: navigate_to_inbox");
        gm_trash(s);   /* trash Message 5 */
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }

    /* Session 2: open Trash, verify Message 5 there, press 'u', check pending marker */
    {
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "u-strike: pty_open (s2)");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "u-strike: pty_run (s2)");
        ASSERT(navigate_to_trash(s) == 0, "u-strike: navigate_to_trash");

        pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
        pty_settle(s, SETTLE_MS);

        int row_before = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row_before >= 0, "u-strike: " GMAIL_TOP_MSG " in Trash before u");
        if (row_before >= 0)
            ASSERT(strcmp(pty_cell_text(s, row_before, 1), " ") == 0,
                   "u-strike: no pending marker before u");

        gm_restore(s);   /* restore from trash */
        pty_settle(s, SETTLE_MS);

        int row_after = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row_after >= 0, "u-strike: " GMAIL_TOP_MSG " still visible after u");
        if (row_after >= 0)
            ASSERT(strcmp(pty_cell_text(s, row_after, 1), "u") == 0,
                   "u-strike: 'u' pending marker at col 0 after u (green restore marker)");

        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'u' key → message gone from Trash after refresh
 * ══════════════════════════════════════════════════════════════════════ */

/** After 'u' + 'R', the restored message must no longer appear in Trash. */
static void test_u_key_row_gone_after_refresh(void) {
    const char *args[] = { g_tui_bin, NULL };

    /* Session 1: trash Message 5 */
    {
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "u-gone: pty_open (s1)");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "u-gone: pty_run (s1)");
        ASSERT(navigate_to_inbox(s) == 0, "u-gone: navigate_to_inbox");
        gm_trash(s);
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }

    /* Session 2: open Trash, press 'u', press 'R', Message 5 must be gone */
    {
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "u-gone: pty_open (s2)");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "u-gone: pty_run (s2)");
        ASSERT(navigate_to_trash(s) == 0, "u-gone: navigate_to_trash");

        pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
        pty_settle(s, SETTLE_MS);

        gm_restore(s);
        pty_settle(s, SETTLE_MS);
        pty_send_str(s, "U");
        pty_settle(s, WAIT_MS / 4);
        pty_settle(s, SETTLE_MS);

        ASSERT(!pty_screen_contains(s, GMAIL_TOP_MSG),
               "u-gone: " GMAIL_TOP_MSG " absent from Trash after u+R");

        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'd' key → message absent after explicit refresh
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * After pressing 'd' on the cursor row and then 'R' (refresh), the
 * message should no longer appear in the INBOX view because its INBOX
 * label was removed from the local index.  'R' re-reads the list from
 * INBOX.idx which no longer contains the UID.
 *
 * This test navigates to the bottom-most entry (GMAIL_BOT_MSG = "Message 1")
 * using the End key before pressing 'd', so the lower boundary of the list
 * is exercised independently of any prior test state.
 */
static void test_d_key_row_gone_after_refresh(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "d-gone: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "d-gone: pty_run");

    ASSERT(navigate_to_inbox(s) == 0, "d-gone: navigate_to_inbox");

    /* Move cursor to the bottom of the list (GMAIL_BOT_MSG = "Message 1").
     * The End key sets cursor = show_count - 1. */
    pty_send_key(s, PTY_KEY_END);
    ASSERT(pty_wait_for(s, GMAIL_BOT_MSG, WAIT_MS) >= 0 ||
           find_row(s, GMAIL_BOT_MSG) >= 0,
           "d-gone: cursor moved to " GMAIL_BOT_MSG);
    pty_settle(s, SETTLE_MS);

    /* Verify cursor is on GMAIL_BOT_MSG */
    ASSERT(find_row(s, GMAIL_BOT_MSG) >= 0,
           "d-gone: " GMAIL_BOT_MSG " visible before d");

    /* Remove INBOX label from the cursor row (GMAIL_BOT_MSG) */
    gm_rm_label(s);
    pty_settle(s, SETTLE_MS);

    /* 'R' re-reads the list from INBOX.idx — the UID is no longer there */
    pty_send_str(s, "U");

    /* Wait for the refreshed list to appear (another message still present) */
    pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* GMAIL_BOT_MSG was in INBOX (the current label).  After removing the
     * INBOX label and refreshing, it must no longer appear in this view. */
    ASSERT(!pty_screen_contains(s, GMAIL_BOT_MSG),
           "d-gone: " GMAIL_BOT_MSG " absent from view after refresh");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'd' key → yellow foreground pending marker (US-43 criterion 1)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Label removal ('d') must render with a yellow foreground 'd' marker,
 * not red, so it is visually distinct from destructive trash ('D').
 */
static void test_d_row_yellow_fg(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "d-yellow: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "d-yellow: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "d-yellow: navigate_to_inbox");

    int row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "d-yellow: " GMAIL_TOP_MSG " present");

    gm_rm_label(s);
    pty_settle(s, SETTLE_MS);

    row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "d-yellow: row still visible after d");
    if (row >= 0) {
        int fg = pty_cell_fg(s, row, 1);
        ASSERT(strcmp(pty_cell_text(s, row, 1), "d") == 0, "d-yellow: 'd' marker set");
        ASSERT(fg_is(fg, PTY_FG_YELLOW), "d-yellow: fg is yellow (33), not red");
    }

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'D' key → red foreground pending marker (US-43 criterion 2)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Trash ('D') must render with a red foreground 'D' marker to distinguish
 * it from the yellow label-removal feedback.
 */
static void test_D_row_red_fg(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "D-red: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "D-red: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "D-red: navigate_to_inbox");

    int row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "D-red: " GMAIL_TOP_MSG " present");

    gm_trash(s);
    pty_settle(s, SETTLE_MS);

    row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "D-red: row still visible after D");
    if (row >= 0) {
        int fg = pty_cell_fg(s, row, 1);
        ASSERT(strcmp(pty_cell_text(s, row, 1), "D") == 0, "D-red: 'D' marker set");
        ASSERT(fg_is(fg, PTY_FG_RED), "D-red: fg is red (31)");
    }

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'a' key (archive from INBOX) → yellow foreground pending marker
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Archiving ('a') must render with a yellow foreground 'd' marker —
 * same as label removal ('d'), distinct from red destructive trash ('D').
 */
static void test_a_row_yellow_fg(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "a-yellow: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "a-yellow: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "a-yellow: navigate_to_inbox");

    int row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "a-yellow: " GMAIL_TOP_MSG " present");

    gm_archive(s);
    pty_settle(s, SETTLE_MS);

    row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "a-yellow: row still visible after a");
    if (row >= 0) {
        int fg = pty_cell_fg(s, row, 1);
        ASSERT(strcmp(pty_cell_text(s, row, 1), "d") == 0, "a-yellow: 'd' marker set");
        ASSERT(fg_is(fg, PTY_FG_YELLOW), "a-yellow: fg is yellow (33), not red");
    }

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'a' in Archive view → no visual change (US-43 criterion 3)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Pressing 'a' while already in the Archive (_nolabel) view is a no-op:
 * the message is already archived, so no pending marker must appear.
 */
static void test_a_in_archive_no_strikethrough(void) {
    const char *args[] = { g_tui_bin, NULL };

    /* Session 1: archive a message so Archive view has content */
    {
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "a-noop: pty_open s1");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "a-noop: pty_run s1");
        ASSERT(navigate_to_inbox(s) == 0, "a-noop: navigate_to_inbox");
        gm_archive(s);   /* archive top message */
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }

    /* Session 2: open Archive, press 'a' → no pending marker */
    {
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "a-noop: pty_open s2");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "a-noop: pty_run s2");

        /* Navigate to Archive view */
        pty_wait_for(s, "Accounts", WAIT_MS);
        pty_send_key(s, PTY_KEY_ENTER); /* into Labels */
        pty_wait_for(s, "Labels", WAIT_MS);
        pty_settle(s, SETTLE_MS / 2);
        navigate_labels_to_archive(s);
        pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
        pty_settle(s, SETTLE_MS);

        int row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "a-noop: " GMAIL_TOP_MSG " visible in Archive");

        /* Press 'a' — message is already archived, must be a no-op */
        gm_archive(s);
        pty_settle(s, SETTLE_MS);

        row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "a-noop: row still present after a");
        if (row >= 0) {
            ASSERT(strcmp(pty_cell_text(s, row, 1), " ") == 0,
                   "a-noop: no pending marker in Archive view (already archived)");
        }

        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: label picker unarchive → green '↩' pending marker (US-43 criterion 5)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * When a real label is added via the label picker in Archive view (which
 * removes the message from _nolabel), the row must show a green '↩' pending
 * marker — restorative, not destructive.
 */
static void test_label_picker_unarchive_green_fg(void) {
    const char *args[] = { g_tui_bin, NULL };

    /* Session 1: archive the top message */
    {
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "unarch-green: pty_open s1");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "unarch-green: pty_run s1");
        ASSERT(navigate_to_inbox(s) == 0, "unarch-green: nav inbox");
        gm_archive(s);
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }

    /* Session 2: navigate to Archive, open label picker, add INBOX */
    {
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "unarch-green: pty_open s2");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "unarch-green: pty_run s2");

        pty_wait_for(s, "Accounts", WAIT_MS);
        pty_send_key(s, PTY_KEY_ENTER);
        pty_wait_for(s, "Labels", WAIT_MS);
        pty_settle(s, SETTLE_MS / 2);
        navigate_labels_to_archive(s);
        pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
        pty_settle(s, SETTLE_MS);

        int row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "unarch-green: " GMAIL_TOP_MSG " in Archive");

        /* Open label picker, Down → INBOX, Space to tick, Enter to apply */
        pty_send_str(s, "t");
        pty_wait_for(s, "Toggle Labels", WAIT_MS);
        pty_settle(s, SETTLE_MS / 2);
        pty_send_key(s, PTY_KEY_DOWN);   /* UNREAD → INBOX */
        pty_send_str(s, " ");  /* add INBOX (Space ticks) */
        pty_settle(s, SETTLE_MS / 2);
        pty_send_key(s, PTY_KEY_ENTER);    /* Enter applies the ticked set */
        pty_settle(s, SETTLE_MS);

        /* Row must now show green 'u' pending marker (message left _nolabel) */
        row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "unarch-green: row still visible after picker");
        if (row >= 0) {
            int fg = pty_cell_fg(s, row, 1);
            ASSERT(strcmp(pty_cell_text(s, row, 1), "u") == 0,
                   "unarch-green: 'u' pending marker at col 0");
            ASSERT(fg_is(fg, PTY_FG_GREEN), "unarch-green: fg is green (32)");
        }

        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: label picker adds INBOX to trashed message → green 'u' marker
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * After 'D' (trash), open label picker via 't', navigate to INBOX and add it.
 * The row must immediately show the green 'u' pending marker (restored from Trash).
 */
static void test_label_picker_untrash_green_fg(void) {
    const char *args[] = { g_tui_bin, NULL };

    /* Session 1: trash the top message */
    {
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "untrash-green: pty_open s1");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "untrash-green: pty_run s1");
        ASSERT(navigate_to_inbox(s) == 0, "untrash-green: nav inbox");
        gm_trash(s);   /* trash Message 5 */
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }

    /* Session 2: open Trash, open label picker, add INBOX */
    {
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "untrash-green: pty_open s2");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "untrash-green: pty_run s2");
        ASSERT(navigate_to_trash(s) == 0, "untrash-green: navigate_to_trash");
        pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
        pty_settle(s, SETTLE_MS);

        int row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "untrash-green: " GMAIL_TOP_MSG " in Trash");

        /* Open label picker, Down → INBOX, Space to tick, Enter to apply */
        pty_send_str(s, "t");
        pty_wait_for(s, "Toggle Labels", WAIT_MS);
        pty_settle(s, SETTLE_MS / 2);
        pty_send_key(s, PTY_KEY_DOWN);   /* UNREAD → INBOX */
        pty_send_str(s, " ");  /* add INBOX (Space ticks) */
        pty_settle(s, SETTLE_MS / 2);
        pty_send_key(s, PTY_KEY_ENTER);    /* Enter applies the ticked set */
        pty_settle(s, SETTLE_MS);

        /* Row must show green 'u' pending marker (message left _trash) */
        row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "untrash-green: row still visible after picker");
        if (row >= 0) {
            ASSERT(strcmp(pty_cell_text(s, row, 1), "u") == 0,
                   "untrash-green: 'u' pending marker at col 0");
            ASSERT(fg_is(pty_cell_fg(s, row, 1), PTY_FG_GREEN),
                   "untrash-green: fg is green (32)");
        }

        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'D','D' undo — second D restores row to normal (no pending marker)
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * In Trash view, pressing 'D' permanently deletes the message (US-83).
 * The row gets the red pending marker and the feedback line reports
 * "Permanently deleted".
 *
 * This replaced the earlier no-op ("Already in Trash") behaviour when
 * D=trash was introduced for both IMAP and Gmail.
 */
static void test_D_permanent_delete_in_trash_view(void) {
    /* Session 1: trash a message */
    reset_and_sync();
    {
        const char *args[] = { g_tui_bin, NULL };
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "D-noop: pty_open s1");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "D-noop: pty_run s1");
        ASSERT(navigate_to_inbox(s) == 0, "D-noop: nav inbox s1");
        gm_trash(s);
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
    /* Session 2: open Trash, press 'D' — permanently deletes */
    {
        const char *args[] = { g_tui_bin, NULL };
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "D-perm: pty_open s2");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "D-perm: pty_run s2");
        ASSERT(navigate_to_trash(s) == 0, "D-perm: navigate_to_trash");
        pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
        pty_settle(s, SETTLE_MS);

        int row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "D-perm: " GMAIL_TOP_MSG " visible in Trash");

        gm_trash(s);
        pty_settle(s, SETTLE_MS);

        /* The row stays visible with a red 'D' marker until the next refresh,
         * so the user can see what was deleted. */
        row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "D-perm: row still present after D");
        if (row >= 0)
            ASSERT(strcmp(pty_cell_text(s, row, 1), "D") == 0,
                   "D-perm: 'D' pending marker in Trash view after D");
        ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Permanently deleted"),
               "D-perm: feedback shows 'Permanently deleted'");

        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}



/**
 * After 'D' (trash) + 'd' (label remove), row shows yellow 'd' marker — not red 'D'.
 * 'd' overrides the pending-remove state.
 */
static void test_D_then_d_shows_yellow(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "D-d-yellow: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "D-d-yellow: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "D-d-yellow: navigate_to_inbox");
    gm_trash(s);   /* trash first */
    pty_settle(s, SETTLE_MS);
    gm_rm_label(s);   /* remove label: should override red 'D' → yellow 'd' */
    pty_settle(s, SETTLE_MS);
    int row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "D-d-yellow: row still visible");
    if (row >= 0) {
        int fg = pty_cell_fg(s, row, 1);
        ASSERT(strcmp(pty_cell_text(s, row, 1), "d") == 0,
               "D-d-yellow: 'd' marker set after d overrides D");
        ASSERT(fg_is(fg, PTY_FG_YELLOW), "D-d-yellow: fg is yellow after d overrides D");
    }
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  FEEDBACK LINE TESTS  (US-44 / US-52-56)
 *
 *  The feedback line is the second-to-last terminal row (ROWS-2, 0-indexed).
 *  Each test checks that pty_row_contains(s, ROWS-2, expected) returns 1.
 * ══════════════════════════════════════════════════════════════════════ */

/** Feedback row must be blank (no feedback text) when the list is first opened. */
static void test_feedback_empty_on_open(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "fb-open: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "fb-open: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "fb-open: navigate_to_inbox");
    /* No key pressed yet — feedback row must not contain any op-result text */
    ASSERT(!pty_row_contains(s, FEEDBACK_ROW, "Label removed"),
           "fb-open: no 'Label removed' on feedback row at open");
    ASSERT(!pty_row_contains(s, FEEDBACK_ROW, "Archived"),
           "fb-open: no 'Archived' on feedback row at open");
    ASSERT(!pty_row_contains(s, FEEDBACK_ROW, "Moved to Trash"),
           "fb-open: no 'Moved to Trash' on feedback row at open");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** After 'D' (trash), feedback line shows "Moved to Trash". */
static void test_feedback_D_trash(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "fb-D: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "fb-D: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "fb-D: navigate_to_inbox");
    gm_trash(s);
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Moved to Trash"),
           "fb-D: feedback row shows 'Moved to Trash'");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** After 'd' (remove label), feedback line shows "Label removed: INBOX". */
static void test_feedback_d_label_removed(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "fb-d: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "fb-d: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "fb-d: navigate_to_inbox");
    gm_rm_label(s);
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Label removed: INBOX"),
           "fb-d: feedback row shows 'Label removed: INBOX'");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** After 'd','d' (remove then undo), feedback line shows "Undo: INBOX restored". */
static void test_feedback_d_undo(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "fb-dundo: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "fb-dundo: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "fb-dundo: navigate_to_inbox");
    gm_rm_label(s);
    pty_settle(s, SETTLE_MS);
    gm_rm_label(s);  /* undo */
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Undo: INBOX restored"),
           "fb-dundo: feedback row shows 'Undo: INBOX restored'");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** After 'a' (archive) in INBOX, feedback line shows "Archived". */
static void test_feedback_a_archive(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "fb-arch: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "fb-arch: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "fb-arch: navigate_to_inbox");
    gm_archive(s);
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Archived"),
           "fb-arch: feedback row shows 'Archived'");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** After 'a' in Archive view (already archived), feedback shows "Already in Archive". */
static void test_feedback_a_noop(void) {
    /* First archive a message */
    reset_and_sync();
    {
        const char *args[] = { g_tui_bin, NULL };
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "fb-anoop: pty_open s1");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "fb-anoop: pty_run s1");
        ASSERT(navigate_to_inbox(s) == 0, "fb-anoop: nav inbox s1");
        gm_archive(s);   /* archive Message 5 */
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
    /* Now open Archive view and press 'a' again */
    {
        const char *args[] = { g_tui_bin, NULL };
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "fb-anoop: pty_open s2");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "fb-anoop: pty_run s2");
        pty_wait_for(s, "Accounts", WAIT_MS);
        pty_send_key(s, PTY_KEY_ENTER);
        pty_wait_for(s, "Labels", WAIT_MS);
        pty_settle(s, SETTLE_MS / 2);
        navigate_labels_to_archive(s);
        pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
        pty_settle(s, SETTLE_MS);
        gm_archive(s);   /* no-op: already in Archive */
        pty_settle(s, SETTLE_MS);
        ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Already in Archive"),
               "fb-anoop: feedback shows 'Already in Archive'");
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}

/* ── Pending label queue (EMAIL-160) ───────────────────────────────────
 * The Gmail list works on the local cache; trash and archive must reach the
 * server through the pending-label queue the next sync flushes. */

/** Reads the account's pending_labels.tsv into @p buf ("" when absent). */
static void read_pending_labels(char *buf, size_t sz) {
    char path[700];
    snprintf(path, sizeof(path),
             "%s/.local/share/email-cli/accounts/%s/pending_labels.tsv",
             g_test_home, GMAIL_TEST_EMAIL);
    buf[0] = '\0';
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    size_t n = fread(buf, 1, sz - 1, fp);
    buf[n] = '\0';
    fclose(fp);
}

/** 'D' on a message queues "_trash" (add) for the next sync. */
static void test_D_queues_trash_for_sync(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "queue-D: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "queue-D: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "queue-D: navigate_to_inbox");
    gm_trash(s);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    char q[4096];
    read_pending_labels(q, sizeof(q));
    ASSERT(strstr(q, "\t_trash\t1") != NULL,
           "queue-D: the trash is queued in pending_labels.tsv");
    ASSERT(strstr(q, "\tINBOX\t0") != NULL,
           "queue-D: the INBOX label removal is queued");
}

/** 'a' queues the label removals and the read mark for the next sync. */
static void test_a_queues_archive_for_sync(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "queue-a: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "queue-a: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "queue-a: navigate_to_inbox");
    gm_archive(s);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    char q[4096];
    read_pending_labels(q, sizeof(q));
    ASSERT(strstr(q, "\tINBOX\t0") != NULL,
           "queue-a: the INBOX removal is queued");
    ASSERT(strstr(q, "\tUNREAD\t0") != NULL,
           "queue-a: the read mark is queued");
}

/** After 'f' (first press = star), feedback line shows "Starred". */
static void test_feedback_f_star(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "fb-f: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "fb-f: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "fb-f: navigate_to_inbox");
    pty_send_str(s, "*");   /* message 5 is not starred → becomes starred */
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Starred"),
           "fb-f: feedback row shows 'Starred'");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** After 'n' (first press: unread→read), feedback line shows "Marked as read". */
static void test_feedback_n_read(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "fb-n: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "fb-n: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "fb-n: navigate_to_inbox");
    /* Message 5 starts with UNREAD label → pressing 'n' marks it as read */
    pty_send_str(s, "+");
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Marked as read"),
           "fb-n: feedback row shows 'Marked as read'");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** After 'u' (restore) in Trash view, feedback line shows "Restored to Inbox". */
static void test_feedback_u_restore(void) {
    /* Session 1: trash Message 5 */
    reset_and_sync();
    {
        const char *args[] = { g_tui_bin, NULL };
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "fb-u: pty_open s1");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "fb-u: pty_run s1");
        ASSERT(navigate_to_inbox(s) == 0, "fb-u: nav inbox s1");
        gm_trash(s);   /* trash Message 5 */
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
    /* Session 2: open Trash, press 'u', check feedback */
    {
        const char *args[] = { g_tui_bin, NULL };
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "fb-u: pty_open s2");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "fb-u: pty_run s2");
        ASSERT(navigate_to_trash(s) == 0, "fb-u: navigate_to_trash");
        pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
        pty_settle(s, SETTLE_MS);
        gm_restore(s);   /* restore from trash */
        pty_settle(s, SETTLE_MS);
        ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Restored to Inbox"),
               "fb-u: feedback row shows 'Restored to Inbox'");
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}

/** After 't' + add label, feedback shows "Label added: <name>". */
static void test_feedback_t_label_added(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "fb-t: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "fb-t: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "fb-t: navigate_to_inbox");
    /* Open label picker, navigate to STARRED (Down×2), tick it, Enter applies */
    pty_send_str(s, "t");
    pty_wait_for(s, "Toggle Labels", WAIT_MS);
    pty_settle(s, SETTLE_MS / 2);
    pty_send_key(s, PTY_KEY_DOWN);  /* UNREAD → INBOX */
    pty_send_key(s, PTY_KEY_DOWN);  /* INBOX  → STARRED */
    pty_send_str(s, " "); /* add STARRED (currently off) (Space ticks) */
    pty_settle(s, SETTLE_MS / 2);
    pty_send_key(s, PTY_KEY_ENTER);   /* Enter applies the ticked set */
    pty_settle(s, SETTLE_MS);
    /* Feedback must mention the label that was added */
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Label added"),
           "fb-t: feedback row shows 'Label added'");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  US-72 helpers
 * ══════════════════════════════════════════════════════════════════════ */

/** Clear account local store without running a sync. */
static void clear_gmail_local_store(void) {
    char store[640], ui_ini[640];
    snprintf(store, sizeof(store), "%s/.local/share/email-cli/accounts/%s",
             g_test_home, GMAIL_TEST_EMAIL);
    snprintf(ui_ini, sizeof(ui_ini), "%s/.local/share/email-cli/ui.ini",
             g_test_home);
    if (test_rmtree(store) != 0 || (unlink(ui_ini) != 0 && errno != ENOENT)) {
        fprintf(stderr, "gmail-tui: cannot clear the local store\n");
        g_tests_failed++;
    }
}

/** Run email-sync and capture stdout + stderr into out_buf. */
static int run_gmail_sync_capture(char *out_buf, size_t out_size) {
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execl(g_sync_bin, "email-sync", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    size_t total = 0;
    ssize_t n;
    while (total < out_size - 1 &&
           (n = read(pfd[0], out_buf + total, out_size - total - 1)) > 0)
        total += (size_t)n;
    out_buf[total] = '\0';
    close(pfd[0]);
    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/** Build path to a file inside the account local store. */
static void account_store_path(char *out, size_t sz, const char *file) {
    snprintf(out, sz, "%s/.local/share/email-cli/accounts/%s/%s",
             g_test_home, GMAIL_TEST_EMAIL, file);
}

/* ══════════════════════════════════════════════════════════════════════
 *  US-72: Gmail Smart Sync Orchestration
 * ══════════════════════════════════════════════════════════════════════ */

static void test_gmail_sync_first_run_reconcile(void) {
    /* US-72 AC1: first sync (no historyId) triggers reconcile */
    clear_gmail_local_store();
    char out[8192] = {0};
    int rc = run_gmail_sync_capture(out, sizeof(out));
    ASSERT(rc == 0, "first run reconcile: sync exits 0");
    ASSERT(strstr(out, "No saved historyId") != NULL ||
           strstr(out, "Listing messages")   != NULL,
           "first run reconcile: reconcile path indicated in output");
}

static void test_gmail_sync_history_id_saved(void) {
    /* US-72 AC1: gmail_history_id file written after first sync */
    clear_gmail_local_store();
    ASSERT(run_gmail_sync() == 0, "history id saved: sync ok");
    char path[512];
    account_store_path(path, sizeof(path), "gmail_history_id");
    FILE *fp = fopen(path, "r");
    ASSERT(fp != NULL, "history id saved: file exists");
    if (fp) {
        char buf[64] = {0};
        char *line = fgets(buf, sizeof(buf), fp);
        fclose(fp);
        ASSERT(line != NULL && buf[0] != '\0', "history id saved: file non-empty");
    }
}

static void test_gmail_sync_pending_empty_after_sync(void) {
    /* US-72 AC2+AC8: pending_fetch.tsv empty (or absent) after a complete first sync */
    clear_gmail_local_store();
    ASSERT(run_gmail_sync() == 0, "pending empty: sync ok");
    char path[512];
    account_store_path(path, sizeof(path), "pending_fetch.tsv");
    FILE *fp = fopen(path, "r");
    if (fp) {
        char buf[64] = {0};
        char *line = fgets(buf, sizeof(buf), fp);
        fclose(fp);
        ASSERT(line == NULL || buf[0] == '\0',
               "pending empty: pending_fetch.tsv is empty after complete sync");
    }
    /* File absent entirely also satisfies the criterion */
}

static void test_gmail_sync_second_run_incremental(void) {
    /* US-72 AC3: second sync uses fast incremental path, no full reconcile */
    clear_gmail_local_store();
    ASSERT(run_gmail_sync() == 0, "second run incremental: first sync ok");
    char out[8192] = {0};
    int rc = run_gmail_sync_capture(out, sizeof(out));
    ASSERT(rc == 0, "second run incremental: second sync exits 0");
    ASSERT(strstr(out, "Incremental sync") != NULL,
           "second run incremental: incremental path taken");
    ASSERT(strstr(out, "Listing messages") == NULL,
           "second run incremental: no full reconcile on second sync");
}

static void test_gmail_sync_drain_then_incremental(void) {
    /* US-72 AC4: pending_fetch.tsv non-empty + valid historyId → drain + incremental */
    clear_gmail_local_store();
    ASSERT(run_gmail_sync() == 0, "drain+incremental: first sync ok");
    /* Re-queue one UID to simulate an interrupted previous sync */
    char path[512];
    account_store_path(path, sizeof(path), "pending_fetch.tsv");
    FILE *fp = fopen(path, "w");
    ASSERT(fp != NULL, "drain+incremental: write pending_fetch.tsv");
    if (fp) { fprintf(fp, "0000000000000001\n"); fclose(fp); }
    char out[8192] = {0};
    int rc = run_gmail_sync_capture(out, sizeof(out));
    ASSERT(rc == 0, "drain+incremental: sync with pending exits 0");
    ASSERT(strstr(out, "Listing messages") == NULL,
           "drain+incremental: no full reconcile when historyId valid");
    /* AC8: UID removed from pending_fetch.tsv after successful download */
    FILE *fp2 = fopen(path, "r");
    if (fp2) {
        char buf[64] = {0};
        char *line = fgets(buf, sizeof(buf), fp2);
        fclose(fp2);
        ASSERT(line == NULL || buf[0] == '\0',
               "drain+incremental: pending_fetch.tsv emptied after drain");
    }
}

static void test_gmail_sync_force_reconcile(void) {
    /* US-72 AC5: deleting gmail_history_id triggers full reconcile on next sync */
    clear_gmail_local_store();
    ASSERT(run_gmail_sync() == 0, "force reconcile: first sync ok");
    char path[512];
    account_store_path(path, sizeof(path), "gmail_history_id");
    unlink(path);
    char out[8192] = {0};
    int rc = run_gmail_sync_capture(out, sizeof(out));
    ASSERT(rc == 0, "force reconcile: re-sync exits 0");
    ASSERT(strstr(out, "No saved historyId") != NULL ||
           strstr(out, "Listing messages")   != NULL,
           "force reconcile: reconcile path taken after historyId deleted");
}

static void test_gmail_sync_pending_fetch_path(void) {
    /* US-72 AC6: account store directory exists at the expected path */
    clear_gmail_local_store();
    ASSERT(run_gmail_sync() == 0, "pending_fetch path: sync ok");
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path),
             "%s/.local/share/email-cli/accounts/%s",
             g_test_home, GMAIL_TEST_EMAIL);
    struct stat st;
    ASSERT(stat(dir_path, &st) == 0 && S_ISDIR(st.st_mode),
           "pending_fetch path: account store dir at expected path");
}

static void test_gmail_sync_pending_fetch_uid_format(void) {
    /* US-72 AC7: pending_fetch.tsv entries are 16-char hex UIDs, one per line.
     * Verified by writing a known-format UID and confirming the drain consumes it. */
    clear_gmail_local_store();
    ASSERT(run_gmail_sync() == 0, "pending_fetch format: first sync ok");
    char path[512];
    account_store_path(path, sizeof(path), "pending_fetch.tsv");
    /* Write one 16-char hex UID per AC7 spec */
    FILE *wp = fopen(path, "w");
    ASSERT(wp != NULL, "pending_fetch format: create test file");
    if (wp) { fprintf(wp, "0000000000000001\n"); fclose(wp); }
    /* Verify format: 16 chars, all hex digits */
    FILE *rp = fopen(path, "r");
    ASSERT(rp != NULL, "pending_fetch format: re-open");
    if (rp) {
        char line[64] = {0};
        char *got = fgets(line, sizeof(line), rp);
        fclose(rp);
        ASSERT(got != NULL, "pending_fetch format: entry readable");
        size_t len = strlen(line);
        while (len > 0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len] = '\0';
        ASSERT(len == 16, "pending_fetch format: entry is 16 characters");
        int all_hex = 1;
        for (size_t i = 0; i < len; i++) {
            char c = line[i];
            if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) all_hex = 0;
        }
        ASSERT(all_hex, "pending_fetch format: entry is hexadecimal");
    }
    /* Drain: proves the 16-char hex format is what the code reads (AC8) */
    ASSERT(run_gmail_sync() == 0, "pending_fetch format: drain sync ok");
    FILE *ep = fopen(path, "r");
    if (ep) {
        char buf[64] = {0};
        char *got = fgets(buf, sizeof(buf), ep);
        fclose(ep);
        ASSERT(got == NULL || buf[0] == '\0',
               "pending_fetch format: UID removed after successful download");
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════════
 *  EMAIL-175: the keyboard policy
 * ══════════════════════════════════════════════════════════════════════ */

/** 'j' and 'k' move the cursor on the message list; they change nothing else. */
static void test_keys_jk_move(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "jk: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "jk: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "jk: navigate_to_inbox");
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "[1/"), "jk: starts on the first row");
    pty_send_str(s, "j");
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "[2/"), "jk: j moves down");
    pty_send_str(s, "j");
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "[3/"), "jk: j moves down again");
    pty_send_str(s, "k");
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "[2/"), "jk: k moves up");
    pty_send_str(s, "k");
    pty_send_str(s, "k");
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "[1/"), "jk: no wrap above the first row");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** 'd' asks, naming the message; No (the default) leaves everything as it was. */
static void test_keys_d_asks_default_no(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "d-asks: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "d-asks: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "d-asks: navigate_to_inbox");
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, GMAIL_TOP_MSG);       /* the subject is named */
    pty_send_key(s, PTY_KEY_ENTER);                 /* Enter on the default */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash");
    int row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "d-asks: message still listed");
    if (row >= 0)
        ASSERT(strcmp(pty_cell_text(s, row, 1), " ") == 0, "d-asks: no pending mark");
    ASSERT(!pty_row_contains(s, FEEDBACK_ROW, "Moved to Trash"), "d-asks: nothing moved");
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Moved to Trash", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** The old direct keys that wrote to the server do nothing now. */
static void test_keys_old_letters_do_nothing(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "old: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "old: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "old: navigate_to_inbox");
    const char *old = "fnDaus";
    for (const char *c = old; *c; c++) {
        char k[2] = { *c, 0 };
        pty_send_str(s, k);
        pty_settle(s, 300);
        int row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0 && strcmp(pty_cell_text(s, row, 1), " ") == 0,
               "old: no pending mark after an old letter");
        ASSERT(!pty_row_contains(s, FEEDBACK_ROW, "Archived") &&
               !pty_row_contains(s, FEEDBACK_ROW, "Moved") &&
               !pty_row_contains(s, FEEDBACK_ROW, "Marked") &&
               !pty_row_contains(s, FEEDBACK_ROW, "tarred") &&
               !pty_row_contains(s, FEEDBACK_ROW, "Restored"),
               "old: no feedback after an old letter");
    }
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** The Gmail menu lists archive, restore, labels and the sync, with the keys. */
static void test_keys_gmail_menu(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, 40);
    ASSERT(s != NULL, "menu: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "menu: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "menu: navigate_to_inbox");
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Actions", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Archive");
    ASSERT_SCREEN_CONTAINS(s, "Restore from Trash");
    ASSERT_SCREEN_CONTAINS(s, "Remove this label");
    ASSERT_SCREEN_CONTAINS(s, "Start a sync");
    ASSERT_SCREEN_CONTAINS(s, "Labels");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** Deleting a label is in the menu of the label browser, and asks first. */
static void test_keys_label_delete_asks(void) {
    PtySession *s = open_label_browser();
    ASSERT(s != NULL, "label delete: reaches the label list");
    if (!s) return;
    pty_send_str(s, "c");                     /* no longer creates a label */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "New label name:");
    pty_send_str(s, "d");                     /* no longer deletes */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Delete the label");
    gm_menu_pick(s, 1);                       /* Delete the label */
    ASSERT_WAIT_FOR(s, "on the server?", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);           /* the default is No */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "on the server?");
    ASSERT_SCREEN_CONTAINS(s, "Labels");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/** Gmail's reader: '*' stars, '+'/'-' mark, 'd' asks; no letter writes. */
static void test_keys_reader(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "reader: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "reader: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "reader: navigate_to_inbox");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "*");
    ASSERT_WAIT_FOR(s, "tarred", WAIT_MS);
    pty_send_str(s, "n");
    pty_settle(s, 300);
    pty_send_str(s, "a");
    pty_settle(s, 300);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Archived");
    pty_send_str(s, "d");
    ASSERT_WAIT_FOR(s, "to Trash", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);                 /* No */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "to Trash");
    ASSERT_SCREEN_CONTAINS(s, "From:");             /* still reading */
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Message reader shortcuts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}


/* ══════════════════════════════════════════════════════════════════════
 *  Renew a Gmail authorization from the accounts screen (EMAIL-130)
 * ══════════════════════════════════════════════════════════════════════ */

static char g_config_ini[600];

/* The account of the other tests, with an OAuth2 client of its own and a
 * refresh token that "has been revoked". */
static void reauth_account(void) {
    write_gmail_config();
    snprintf(g_config_ini, sizeof(g_config_ini),
             "%s/.config/email-cli/accounts/%s/config.ini", g_test_home, GMAIL_TEST_EMAIL);
    FILE *fp = fopen(g_config_ini, "a");
    if (!fp) { perror("fopen config.ini"); g_tests_failed++; return; }
    fprintf(fp, "GMAIL_CLIENT_ID=pty.apps.googleusercontent.com\n"
                "GMAIL_CLIENT_SECRET=pty-secret\n");
    fclose(fp);
    chmod(g_config_ini, 0600);
    /* A readable config.ini: the test looks into it. */
    char sp[600];
    snprintf(sp, sizeof(sp), "%s/.config/email-cli/settings.ini", g_test_home);
    FILE *sf = fopen(sp, "w");
    if (sf) { fputs("credential_obfuscation=false\n", sf); fclose(sf); }
    char url[300];
    snprintf(url, sizeof(url), "http://localhost:%d/token", g_mock_port);
    setenv("GMAIL_TEST_TOKEN_URL", url, 1);
}

/* 1 when config.ini holds @p line. */
static int ini_has(const char *line) {
    FILE *fp = fopen(g_config_ini, "r");
    if (!fp) return 0;
    char buf[512];
    int found = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (strcmp(buf, line) == 0) found = 1;
    }
    fclose(fp);
    return found;
}

static PtySession *open_accounts(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    if (pty_wait_for(s, "Email Accounts", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static void test_reauth_menu_offers_the_renewal(void) {
    reauth_account();
    PtySession *s = open_accounts();
    ASSERT(s != NULL, "reauth: accounts screen");
    if (!s) return;
    ASSERT_SCREEN_CONTAINS(s, "actions");               /* the hint names the menu */
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Renew the authorization", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Actions");
    pty_send_key(s, PTY_KEY_ESC);                       /* closing it changes nothing */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Renew the authorization");
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    ASSERT(ini_has("GMAIL_REFRESH_TOKEN=fake_refresh"), "reauth: token untouched");
    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Actions of a Gmail account", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reauth_paste_renews_the_token(void) {
    reauth_account();
    PtySession *s = open_accounts();
    ASSERT(s != NULL, "reauth paste: accounts screen");
    if (!s) return;
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Renew the authorization", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Renew the Gmail authorization of", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "paste the redirect address");
    pty_send_str(s, "p");
    ASSERT_WAIT_FOR(s, "Open this address in a browser", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Paste the address", WAIT_MS);
    pty_send_str(s, "http://localhost:8089/callback?code=good&scope=x");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Gmail authorization renewed.", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    ASSERT(ini_has("GMAIL_REFRESH_TOKEN=mock-refresh"), "reauth paste: the new token is stored");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reauth_refused_code_changes_nothing(void) {
    reauth_account();
    PtySession *s = open_accounts();
    ASSERT(s != NULL, "reauth refused: accounts screen");
    if (!s) return;
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Renew the authorization", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Choose:", WAIT_MS);
    pty_send_str(s, "p");
    ASSERT_WAIT_FOR(s, "Paste the address", WAIT_MS);
    pty_send_str(s, "http://localhost:8089/callback?code=bad");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Press any key to return to accounts", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Error:");                 /* why, not hidden */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Authorization was not completed", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    ASSERT(ini_has("GMAIL_REFRESH_TOKEN=fake_refresh"), "reauth refused: token unchanged");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reauth_wait_is_announced_and_cancellable(void) {
    reauth_account();
    PtySession *s = open_accounts();
    ASSERT(s != NULL, "reauth wait: accounts screen");
    if (!s) return;
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Renew the authorization", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Choose:", WAIT_MS);
    pty_send_str(s, "l");                                /* no browser is started */
    ASSERT_WAIT_FOR(s, "Waiting for Google", WAIT_MS);   /* the screen says why it waits */
    ASSERT_WAIT_FOR(s, "Waiting for authorization", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Ctrl-C cancels");
    pty_send_key(s, PTY_KEY_CTRL_C);                    /* Ctrl-C: ends the wait, not the program */
    ASSERT_WAIT_FOR(s, "Press any key to return to accounts", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Cancelled", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    ASSERT(ini_has("GMAIL_REFRESH_TOKEN=fake_refresh"), "reauth wait: token unchanged");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reauth_choice_can_be_left(void) {
    reauth_account();
    PtySession *s = open_accounts();
    ASSERT(s != NULL, "reauth esc: accounts screen");
    if (!s) return;
    pty_send_str(s, ":");
    ASSERT_WAIT_FOR(s, "Renew the authorization", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Choose:", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Cancelled", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Email Accounts");
    ASSERT(ini_has("GMAIL_REFRESH_TOKEN=fake_refresh"), "reauth esc: token unchanged");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}


int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr,
                "Usage: %s <email-tui> <email-sync> <mock-gmail-server>\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(g_tui_bin,  sizeof(g_tui_bin),  "%s", argv[1]);
    snprintf(g_sync_bin, sizeof(g_sync_bin), "%s", argv[2]);
    snprintf(g_mock_bin, sizeof(g_mock_bin), "%s", argv[3]);

    /* Redirect the test to a private HOME so it never touches the real inbox */
    const char *old_home = getenv("HOME");
    snprintf(g_test_home, sizeof(g_test_home), "%s", TP("home"));

    setenv("HOME",               g_test_home,     1);
    setenv("GMAIL_TEST_TOKEN",   "test_fake_token", 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_DATA_HOME");

    write_gmail_config();

    printf("--- Starting mock Gmail API server (%d messages) ---\n",
           GMAIL_PTY_COUNT);
    if (start_gmail_mock() != 0) {
        fprintf(stderr, "FATAL: cannot start mock Gmail API server\n");
        g_tests_failed++;           /* a fixture that cannot start is a failure */
        goto done;
    }
    snprintf(g_api_url, sizeof(g_api_url),
             "http://localhost:%d/gmail/v1/users/me", g_mock_port);
    setenv("GMAIL_API_BASE_URL", g_api_url, 1);

    /* Reset and re-populate the cache before each test so every test
     * starts from the same clean state (all 5 messages in INBOX). */

    printf("\n--- Gmail TUI: 'd' key pending-remove feedback ---\n");

    printf("--- Fresh sync (test 1) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 1\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_d_key_row_stays_visible);
    RUN_TEST(test_label_browser_search_prompt_and_scopes);
    RUN_TEST(test_label_browser_search_runs);
    RUN_TEST(test_label_browser_lists_a_label_without_messages);
    RUN_TEST(test_label_browser_create_label_prompt);
    RUN_TEST(test_label_browser_create_label_confirmed);

    printf("--- Fresh sync (test 2) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 2\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_d_key_row_strikethrough);

    printf("--- Fresh sync (test 3) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 3\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_d_key_cursor_row_has_reverse);

    printf("--- Fresh sync (test 4) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 4\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_d_key_toggle_undo);

    printf("--- Fresh sync (test 5) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 5\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_d_key_row_gone_after_refresh);

    printf("\n--- Gmail TUI: label picker / archive / trash / restore ---\n");

    printf("--- Fresh sync (test 6) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 6\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_label_picker_shows_unread);
    RUN_TEST(test_label_picker_esc_discards_enter_applies);

    printf("--- Fresh sync (test 7) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 7\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_archive_row_has_strikethrough);

    printf("--- Fresh sync (test 8) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 8\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_archive_removes_from_inbox);

    printf("--- Fresh sync (test 9) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 9\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_archive_message_in_archive_view);

    printf("--- Fresh sync (test 10) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 10\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_label_picker_unarchive);

    printf("--- Fresh sync (test 11) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 11\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_trash_statusbar_restore_hint);
    RUN_TEST(test_labels_up_at_top_is_noop);

    printf("--- Fresh sync (test 12) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 12\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_D_key_row_has_strikethrough);

    /* Tests 13–14: two-session tests — reset_and_sync() covers session 1;
     * session 2 reuses the state left by session 1 (trashed message). */
    printf("--- Fresh sync (test 13) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 13\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_u_key_row_has_strikethrough);

    printf("--- Fresh sync (test 14) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 14\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_u_key_row_gone_after_refresh);

    /* ── US-43: pending-row colour semantics ──────────────────────── */

    printf("--- Fresh sync (test 15: d=yellow) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 15\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_d_row_yellow_fg);

    printf("--- Fresh sync (test 16: D=red) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 16\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_D_row_red_fg);

    printf("--- Fresh sync (test 17: a=yellow) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 17\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_a_row_yellow_fg);

    printf("--- Fresh sync (test 18: a in Archive = noop) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 18\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_a_in_archive_no_strikethrough);

    printf("--- Fresh sync (test 19: label picker unarchive = green) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 19\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_label_picker_unarchive_green_fg);

    printf("--- Fresh sync (test 20: label picker untrash = green) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 20\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_label_picker_untrash_green_fg);

    /* ── 'D' no-op in Trash, 'D','D' undo, 'D'+'d' colour tests ── */
    printf("--- Fresh sync (test 20: D noop in Trash) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 20\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_D_permanent_delete_in_trash_view);

    printf("--- Fresh sync (test 21: D+D undo) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 21\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_D_then_d_shows_yellow);

    /* ── Feedback line tests (US-44 / US-52-56) ── */
    RUN_TEST(test_feedback_empty_on_open);
    RUN_TEST(test_feedback_D_trash);
    RUN_TEST(test_feedback_d_label_removed);
    RUN_TEST(test_feedback_d_undo);
    RUN_TEST(test_feedback_a_archive);
    RUN_TEST(test_D_queues_trash_for_sync);
    RUN_TEST(test_a_queues_archive_for_sync);
    RUN_TEST(test_feedback_a_noop);
    RUN_TEST(test_feedback_f_star);
    RUN_TEST(test_feedback_n_read);
    RUN_TEST(test_feedback_u_restore);
    RUN_TEST(test_feedback_t_label_added);
    RUN_TEST(test_keys_jk_move);
    RUN_TEST(test_keys_d_asks_default_no);
    RUN_TEST(test_keys_old_letters_do_nothing);
    RUN_TEST(test_keys_gmail_menu);
    RUN_TEST(test_keys_label_delete_asks);
    RUN_TEST(test_keys_reader);

    /* ── US-72: Gmail Smart Sync Orchestration ── */
    printf("\n--- Gmail Smart Sync orchestration (US-72) ---\n");
    RUN_TEST(test_gmail_sync_first_run_reconcile);
    RUN_TEST(test_gmail_sync_history_id_saved);
    RUN_TEST(test_gmail_sync_pending_empty_after_sync);
    RUN_TEST(test_gmail_sync_second_run_incremental);
    RUN_TEST(test_gmail_sync_drain_then_incremental);
    RUN_TEST(test_gmail_sync_force_reconcile);
    RUN_TEST(test_gmail_sync_pending_fetch_path);
    RUN_TEST(test_gmail_sync_pending_fetch_uid_format);

    printf("\n--- Gmail TUI: renew the authorization (EMAIL-130) ---\n");
    RUN_TEST(test_reauth_menu_offers_the_renewal);
    RUN_TEST(test_reauth_paste_renews_the_token);
    RUN_TEST(test_reauth_refused_code_changes_nothing);
    RUN_TEST(test_reauth_wait_is_announced_and_cancellable);
    RUN_TEST(test_reauth_choice_can_be_left);

    stop_gmail_mock();

done:
    if (old_home) setenv("HOME", old_home, 1);

    printf("\n--- Gmail TUI PTY Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
