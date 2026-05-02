/**
 * @file test_pty_gmail_tui.c
 * @brief PTY tests for Gmail TUI interactions.
 *
 * Covers:
 *   - 'd' key pending-remove visual feedback (pending marker character)
 *
 * Usage: test-pty-gmail-tui <email-tui> <email-sync> <mock-gmail-server>
 *
 * The mock Gmail API server is started on GMAIL_PTY_PORT; the environment
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

#define RUN_TEST(fn) do { printf("  %s...\n", #fn); fn(); } while(0)

/* ── Configuration ───────────────────────────────────────────────────── */

#define GMAIL_PTY_PORT   9096
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
#define FEEDBACK_ROW (ROWS - 2)   /* 0-indexed PTY row for the infoline */

/* ── Global state ────────────────────────────────────────────────────── */

static pid_t g_mock_pid = -1;
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
    g_mock_pid = fork();
    if (g_mock_pid < 0) return -1;
    if (g_mock_pid == 0) {
        char port_str[16], count_str[16];
        snprintf(port_str,  sizeof(port_str),  "%d", GMAIL_PTY_PORT);
        snprintf(count_str, sizeof(count_str), "%d", GMAIL_PTY_COUNT);
        setenv("MOCK_GMAIL_PORT",           port_str,          1);
        setenv("MOCK_GMAIL_COUNT",          count_str,         1);
        setenv("MOCK_GMAIL_EMAIL",          GMAIL_TEST_EMAIL,  1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execl(g_mock_bin, "mock_gmail_api_server", (char *)NULL);
        _exit(127);
    }
    /* Poll until the server accepts connections (max 3 s) */
    for (int i = 0; i < 30; i++) {
        usleep(100000);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        struct sockaddr_in addr = {
            .sin_family      = AF_INET,
            .sin_port        = htons(GMAIL_PTY_PORT),
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
        };
        int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        close(fd);
        if (rc == 0) return 0;
    }
    return -1;
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
 * Reset account local data and re-populate from the mock server.
 *
 * Deletes the account's local store (labels/, headers/, history ID, etc.)
 * so the next email-sync performs a full fresh sync instead of an
 * incremental one.  This ensures every test starts from the same clean
 * state (all 5 messages in INBOX with their original labels).
 */
static int reset_and_sync(void) {
    /* Remove the per-account local store */
    char cmd[600];
    snprintf(cmd, sizeof(cmd),
             "rm -rf '/tmp/email-cli-gmail-pty-%d/.local/share/email-cli/accounts/%s'",
             (int)getpid(), GMAIL_TEST_EMAIL);
    int _rc1 = system(cmd);  (void)_rc1;  /* test-only, path is controlled */

    /* Also clear the shared UI prefs file (it remembers the last-used folder
     * cursor; stale entries would make subsequent tests open the wrong label). */
    char cmd2[600];
    snprintf(cmd2, sizeof(cmd2),
             "rm -f '%s/.local/share/email-cli/ui.ini'",
             g_test_home);
    int _rc2 = system(cmd2);  (void)_rc2;

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
    pty_send_str(s, "d");
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
        ASSERT(strcmp(pty_cell_text(s, row_before, 0), " ") == 0,
               "d-strike: no marker before d");
    }

    /* Press 'd' — removes INBOX label from the cursor row */
    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);

    /* The cursor row must still be visible (pending-remove keeps it) */
    int row_after = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_after >= 0, "d-strike: " GMAIL_TOP_MSG " still present after d");

    if (row_after >= 0) {
        /* The pending-remove row is rendered with a yellow 'd' marker at col 0. */
        ASSERT(strcmp(pty_cell_text(s, row_after, 0), "d") == 0,
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
 * PTY_ATTR_REVERSE must be set on the row's cells alongside the pending marker.
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

    /* Before 'd': cursor row must have REVERSE, must NOT have a pending marker */
    if (row_before >= 0) {
        int attr = pty_cell_attr(s, row_before, 4);
        ASSERT(attr & PTY_ATTR_REVERSE,  "d-rev: cursor row has REVERSE before d");
        ASSERT(strcmp(pty_cell_text(s, row_before, 0), " ") == 0,
               "d-rev: no marker before d");
    }

    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);

    int row_after = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_after >= 0, "d-rev: " GMAIL_TOP_MSG " still present after d");

    /* After 'd': cursor row must have REVERSE and the 'd' pending marker */
    if (row_after >= 0) {
        int attr = pty_cell_attr(s, row_after, 1);
        ASSERT(attr & PTY_ATTR_REVERSE, "d-rev: cursor row has REVERSE after d");
        ASSERT(strcmp(pty_cell_text(s, row_after, 0), "d") == 0,
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
    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);

    int row1 = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row1 >= 0, "d-undo: " GMAIL_TOP_MSG " present after first d");
    if (row1 >= 0)
        ASSERT(strcmp(pty_cell_text(s, row1, 0), "d") == 0,
               "d-undo: 'd' marker set after first d");

    /* Second 'd': undo */
    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);

    int row2 = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row2 >= 0, "d-undo: " GMAIL_TOP_MSG " still present after second d");
    if (row2 >= 0)
        ASSERT(strcmp(pty_cell_text(s, row2, 0), " ") == 0,
               "d-undo: marker cleared after second d");

    /* Refresh — row must survive (label was restored) */
    pty_send_str(s, "R");
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
        ASSERT(strcmp(pty_cell_text(s, row_before, 0), " ") == 0,
               "arch-strike: no pending marker before a");

    pty_send_str(s, "a");
    pty_settle(s, SETTLE_MS);

    int row_after = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_after >= 0, "arch-strike: " GMAIL_TOP_MSG " still visible after a");
    if (row_after >= 0)
        ASSERT(strcmp(pty_cell_text(s, row_after, 0), "d") == 0,
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

    pty_send_str(s, "a");
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "R");
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
    pty_send_str(s, "a");
    pty_settle(s, SETTLE_MS);
    /* Press 'R' so the inbox index is refreshed and the row is removed */
    pty_send_str(s, "R");
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
    pty_send_str(s, "a");
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "R");
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
    pty_send_key(s, PTY_KEY_ENTER); /* toggle INBOX on */
    pty_settle(s, SETTLE_MS / 2);
    pty_send_key(s, PTY_KEY_ESC);   /* close picker */
    pty_settle(s, SETTLE_MS);

    /* Refresh Archive view — message must be gone (now in INBOX) */
    pty_send_str(s, "R");
    pty_settle(s, WAIT_MS / 4);
    pty_settle(s, SETTLE_MS);

    ASSERT(!pty_screen_contains(s, GMAIL_TOP_MSG),
           "unarchive: " GMAIL_TOP_MSG " gone from Archive after INBOX added");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: Trash statusbar shows u=restore hint
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * When viewing the Trash folder, the status bar must show "u=restore" to
 * guide the user.  This test works even with an empty Trash.
 */
static void test_trash_statusbar_restore_hint(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "trash-sb: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "trash-sb: pty_run");
    ASSERT(navigate_to_trash(s) == 0, "trash-sb: navigate_to_trash");

    /* The status bar (last row) must contain "u=restore" */
    ASSERT_SCREEN_CONTAINS(s, "u=restore");

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
        ASSERT(strcmp(pty_cell_text(s, row_before, 0), " ") == 0,
               "D-strike: no pending marker before D");

    pty_send_str(s, "D");
    pty_settle(s, SETTLE_MS);

    int row_after = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_after >= 0, "D-strike: " GMAIL_TOP_MSG " still visible after D");
    if (row_after >= 0)
        ASSERT(strcmp(pty_cell_text(s, row_after, 0), "D") == 0,
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
        pty_send_str(s, "D");   /* trash Message 5 */
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
            ASSERT(strcmp(pty_cell_text(s, row_before, 0), " ") == 0,
                   "u-strike: no pending marker before u");

        pty_send_str(s, "u");   /* restore from trash */
        pty_settle(s, SETTLE_MS);

        int row_after = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row_after >= 0, "u-strike: " GMAIL_TOP_MSG " still visible after u");
        if (row_after >= 0)
            ASSERT(strcmp(pty_cell_text(s, row_after, 0), "u") == 0,
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
        pty_send_str(s, "D");
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

        pty_send_str(s, "u");
        pty_settle(s, SETTLE_MS);
        pty_send_str(s, "R");
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
    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);

    /* 'R' re-reads the list from INBOX.idx — the UID is no longer there */
    pty_send_str(s, "R");

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

    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);

    row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "d-yellow: row still visible after d");
    if (row >= 0) {
        int fg = pty_cell_fg(s, row, 0);
        ASSERT(strcmp(pty_cell_text(s, row, 0), "d") == 0, "d-yellow: 'd' marker set");
        ASSERT(fg == PTY_FG_YELLOW, "d-yellow: fg is yellow (33), not red");
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

    pty_send_str(s, "D");
    pty_settle(s, SETTLE_MS);

    row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "D-red: row still visible after D");
    if (row >= 0) {
        int fg = pty_cell_fg(s, row, 0);
        ASSERT(strcmp(pty_cell_text(s, row, 0), "D") == 0, "D-red: 'D' marker set");
        ASSERT(fg == PTY_FG_RED, "D-red: fg is red (31)");
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

    pty_send_str(s, "a");
    pty_settle(s, SETTLE_MS);

    row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "a-yellow: row still visible after a");
    if (row >= 0) {
        int fg = pty_cell_fg(s, row, 0);
        ASSERT(strcmp(pty_cell_text(s, row, 0), "d") == 0, "a-yellow: 'd' marker set");
        ASSERT(fg == PTY_FG_YELLOW, "a-yellow: fg is yellow (33), not red");
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
        pty_send_str(s, "a");   /* archive top message */
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
        pty_send_str(s, "a");
        pty_settle(s, SETTLE_MS);

        row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "a-noop: row still present after a");
        if (row >= 0) {
            ASSERT(strcmp(pty_cell_text(s, row, 0), " ") == 0,
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
        pty_send_str(s, "a");
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

        /* Open label picker, Down → INBOX, Enter to toggle on, ESC close */
        pty_send_str(s, "t");
        pty_wait_for(s, "Toggle Labels", WAIT_MS);
        pty_settle(s, SETTLE_MS / 2);
        pty_send_key(s, PTY_KEY_DOWN);   /* UNREAD → INBOX */
        pty_send_key(s, PTY_KEY_ENTER);  /* add INBOX */
        pty_settle(s, SETTLE_MS / 2);
        pty_send_key(s, PTY_KEY_ESC);    /* close picker */
        pty_settle(s, SETTLE_MS);

        /* Row must now show green 'u' pending marker (message left _nolabel) */
        row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "unarch-green: row still visible after picker");
        if (row >= 0) {
            int fg = pty_cell_fg(s, row, 0);
            ASSERT(strcmp(pty_cell_text(s, row, 0), "u") == 0,
                   "unarch-green: 'u' pending marker at col 0");
            ASSERT(fg == PTY_FG_GREEN, "unarch-green: fg is green (32)");
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
        pty_send_str(s, "D");   /* trash Message 5 */
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

        /* Open label picker, Down → INBOX, Enter to toggle on, ESC close */
        pty_send_str(s, "t");
        pty_wait_for(s, "Toggle Labels", WAIT_MS);
        pty_settle(s, SETTLE_MS / 2);
        pty_send_key(s, PTY_KEY_DOWN);   /* UNREAD → INBOX */
        pty_send_key(s, PTY_KEY_ENTER);  /* add INBOX */
        pty_settle(s, SETTLE_MS / 2);
        pty_send_key(s, PTY_KEY_ESC);    /* close picker */
        pty_settle(s, SETTLE_MS);

        /* Row must show green 'u' pending marker (message left _trash) */
        row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "untrash-green: row still visible after picker");
        if (row >= 0) {
            ASSERT(strcmp(pty_cell_text(s, row, 0), "u") == 0,
                   "untrash-green: 'u' pending marker at col 0");
            ASSERT(pty_cell_fg(s, row, 0) == PTY_FG_GREEN,
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
 * In Trash view, pressing 'D' is a no-op: message is already in Trash.
 * No pending marker must appear and feedback shows "Already in Trash".
 */
static void test_D_noop_in_trash_view(void) {
    /* Session 1: trash a message */
    reset_and_sync();
    {
        const char *args[] = { g_tui_bin, NULL };
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "D-noop: pty_open s1");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "D-noop: pty_run s1");
        ASSERT(navigate_to_inbox(s) == 0, "D-noop: nav inbox s1");
        pty_send_str(s, "D");
        pty_settle(s, SETTLE_MS);
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
    /* Session 2: open Trash, press 'D' — must be no-op */
    {
        const char *args[] = { g_tui_bin, NULL };
        PtySession *s = pty_open(COLS, ROWS);
        ASSERT(s != NULL, "D-noop: pty_open s2");
        if (!s) return;
        ASSERT(pty_run(s, args) == 0, "D-noop: pty_run s2");
        ASSERT(navigate_to_trash(s) == 0, "D-noop: navigate_to_trash");
        pty_wait_for(s, GMAIL_TOP_MSG, WAIT_MS);
        pty_settle(s, SETTLE_MS);

        int row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "D-noop: " GMAIL_TOP_MSG " visible in Trash");

        pty_send_str(s, "D");
        pty_settle(s, SETTLE_MS);

        row = find_row(s, GMAIL_TOP_MSG);
        ASSERT(row >= 0, "D-noop: row still present after D (no-op)");
        if (row >= 0)
            ASSERT(strcmp(pty_cell_text(s, row, 0), " ") == 0,
                   "D-noop: no pending marker in Trash view after D");
        ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Already in Trash"),
               "D-noop: feedback shows 'Already in Trash'");

        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
}

/**
 * First 'D' moves to Trash (red 'D' pending marker).
 * Second 'D' undoes the trash: row returns to normal, no pending marker.
 */
static void test_D_D_undo_clears_strikethrough(void) {
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "D-undo: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "D-undo: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "D-undo: navigate_to_inbox");

    int row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "D-undo: " GMAIL_TOP_MSG " present");

    /* First D: trash */
    pty_send_str(s, "D");
    pty_settle(s, SETTLE_MS);
    row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "D-undo: row visible after first D");
    if (row >= 0)
        ASSERT(strcmp(pty_cell_text(s, row, 0), "D") == 0,
               "D-undo: 'D' marker after first D");

    /* Second D: undo trash */
    pty_send_str(s, "D");
    pty_settle(s, SETTLE_MS);
    row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "D-undo: row still visible after undo");
    if (row >= 0)
        ASSERT(strcmp(pty_cell_text(s, row, 0), " ") == 0,
               "D-undo: no pending marker after second D (undo)");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/**
 * After 'D','D' (undo trash), feedback shows "Undo: INBOX restored".
 */
static void test_D_D_undo_feedback(void) {
    reset_and_sync();
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    ASSERT(s != NULL, "D-undo-fb: pty_open");
    if (!s) return;
    ASSERT(pty_run(s, args) == 0, "D-undo-fb: pty_run");
    ASSERT(navigate_to_inbox(s) == 0, "D-undo-fb: navigate_to_inbox");
    pty_send_str(s, "D");
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "D");   /* undo */
    pty_settle(s, SETTLE_MS);
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Undo: INBOX restored"),
           "D-undo-fb: feedback shows 'Undo: INBOX restored'");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
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
    pty_send_str(s, "D");   /* trash first */
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "d");   /* remove label: should override red 'D' → yellow 'd' */
    pty_settle(s, SETTLE_MS);
    int row = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row >= 0, "D-d-yellow: row still visible");
    if (row >= 0) {
        int fg = pty_cell_fg(s, row, 0);
        ASSERT(strcmp(pty_cell_text(s, row, 0), "d") == 0,
               "D-d-yellow: 'd' marker set after d overrides D");
        ASSERT(fg == PTY_FG_YELLOW, "D-d-yellow: fg is yellow after d overrides D");
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
    pty_send_str(s, "D");
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
    pty_send_str(s, "d");
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
    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "d");  /* undo */
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
    pty_send_str(s, "a");
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
        pty_send_str(s, "a");   /* archive Message 5 */
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
        pty_send_str(s, "a");   /* no-op: already in Archive */
        pty_settle(s, SETTLE_MS);
        ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Already in Archive"),
               "fb-anoop: feedback shows 'Already in Archive'");
        pty_send_key(s, PTY_KEY_ESC);
        pty_close(s);
    }
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
    pty_send_str(s, "f");   /* message 5 is not starred → becomes starred */
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
    pty_send_str(s, "n");
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
        pty_send_str(s, "D");   /* trash Message 5 */
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
        pty_send_str(s, "u");   /* restore from trash */
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
    /* Open label picker, navigate to STARRED (Down×2), add it, ESC close */
    pty_send_str(s, "t");
    pty_wait_for(s, "Toggle Labels", WAIT_MS);
    pty_settle(s, SETTLE_MS / 2);
    pty_send_key(s, PTY_KEY_DOWN);  /* UNREAD → INBOX */
    pty_send_key(s, PTY_KEY_DOWN);  /* INBOX  → STARRED */
    pty_send_key(s, PTY_KEY_ENTER); /* add STARRED (currently off) */
    pty_settle(s, SETTLE_MS / 2);
    pty_send_key(s, PTY_KEY_ESC);   /* close picker */
    pty_settle(s, SETTLE_MS);
    /* Feedback must mention the label that was added */
    ASSERT(pty_row_contains(s, FEEDBACK_ROW, "Label added"),
           "fb-t: feedback row shows 'Label added'");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════ */

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
    snprintf(g_api_url,  sizeof(g_api_url),
             "http://localhost:%d/gmail/v1/users/me", GMAIL_PTY_PORT);

    /* Redirect the test to a private HOME so it never touches the real inbox */
    const char *old_home = getenv("HOME");
    snprintf(g_test_home, sizeof(g_test_home),
             "/tmp/email-cli-gmail-pty-%d", getpid());

    setenv("HOME",               g_test_home,     1);
    setenv("GMAIL_API_BASE_URL", g_api_url,        1);
    setenv("GMAIL_TEST_TOKEN",   "test_fake_token", 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_DATA_HOME");

    write_gmail_config();

    printf("--- Starting mock Gmail API server (port %d, %d messages) ---\n",
           GMAIL_PTY_PORT, GMAIL_PTY_COUNT);
    if (start_gmail_mock() != 0) {
        fprintf(stderr, "FATAL: cannot start mock Gmail API server\n");
        goto done;
    }

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
    RUN_TEST(test_D_noop_in_trash_view);

    printf("--- Fresh sync (test 21: D+D undo) ---\n");
    if (reset_and_sync() != 0) {
        fprintf(stderr, "FATAL: reset_and_sync failed for test 21\n");
        stop_gmail_mock();
        goto done;
    }
    RUN_TEST(test_D_D_undo_clears_strikethrough);
    RUN_TEST(test_D_D_undo_feedback);
    RUN_TEST(test_D_then_d_shows_yellow);

    /* ── Feedback line tests (US-44 / US-52-56) ── */
    RUN_TEST(test_feedback_empty_on_open);
    RUN_TEST(test_feedback_D_trash);
    RUN_TEST(test_feedback_d_label_removed);
    RUN_TEST(test_feedback_d_undo);
    RUN_TEST(test_feedback_a_archive);
    RUN_TEST(test_feedback_a_noop);
    RUN_TEST(test_feedback_f_star);
    RUN_TEST(test_feedback_n_read);
    RUN_TEST(test_feedback_u_restore);
    RUN_TEST(test_feedback_t_label_added);

    stop_gmail_mock();

done:
    if (old_home) setenv("HOME", old_home, 1);

    printf("\n--- Gmail TUI PTY Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
