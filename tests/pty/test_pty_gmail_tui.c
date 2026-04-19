/**
 * @file test_pty_gmail_tui.c
 * @brief PTY tests for Gmail TUI interactions.
 *
 * Covers:
 *   - 'd' key pending-remove visual feedback (red strikethrough)
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
    /* Remove the entire local store for this account */
    char cmd[600];
    snprintf(cmd, sizeof(cmd),
             "rm -rf '%s/.local/share/email-cli/accounts/%s'",
             g_test_home, GMAIL_TEST_EMAIL);
    system(cmd);  /* NOLINT — test-only code, path is controlled */

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
 *  TEST: 'd' key → pending-remove row has strikethrough attribute
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * The pending-remove row must be rendered with PTY_ATTR_STRIKE (SGR 9)
 * so the user immediately sees that the label removal is queued.
 *
 * The cursor row (GMAIL_TOP_MSG = "Message 5") is the one that gets the
 * strikethrough after 'd' is pressed.
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

    /* Check that the cursor row does NOT have strikethrough BEFORE pressing 'd' */
    if (row_before >= 0) {
        int attr_before = pty_cell_attr(s, row_before, 4);
        ASSERT(!(attr_before & PTY_ATTR_STRIKE),
               "d-strike: no strikethrough before d");
    }

    /* Press 'd' — removes INBOX label from the cursor row */
    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);

    /* The cursor row must still be visible (pending-remove keeps it) */
    int row_after = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_after >= 0, "d-strike: " GMAIL_TOP_MSG " still present after d");

    if (row_after >= 0) {
        /* The pending-remove row is rendered with \033[31m\033[9m (red +
         * strikethrough).  PTY_ATTR_STRIKE must be set on its cells. */
        int attr_after = pty_cell_attr(s, row_after, 4);
        ASSERT(attr_after & PTY_ATTR_STRIKE,
               "d-strike: cursor row has strikethrough after d");
    }

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST: 'd' key → pending-remove row retains inverse cursor highlight
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * After pressing 'd', the cursor row is rendered with red + strikethrough
 * AND inverse-video so the cursor position remains clearly visible.
 * PTY_ATTR_REVERSE must be set on the row's cells alongside PTY_ATTR_STRIKE.
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

    /* Before 'd': cursor row must have REVERSE, must NOT have STRIKE */
    if (row_before >= 0) {
        int attr = pty_cell_attr(s, row_before, 4);
        ASSERT(attr & PTY_ATTR_REVERSE,  "d-rev: cursor row has REVERSE before d");
        ASSERT(!(attr & PTY_ATTR_STRIKE), "d-rev: no STRIKE before d");
    }

    pty_send_str(s, "d");
    pty_settle(s, SETTLE_MS);

    int row_after = find_row(s, GMAIL_TOP_MSG);
    ASSERT(row_after >= 0, "d-rev: " GMAIL_TOP_MSG " still present after d");

    /* After 'd': cursor row must have BOTH REVERSE and STRIKE */
    if (row_after >= 0) {
        int attr = pty_cell_attr(s, row_after, 4);
        ASSERT(attr & PTY_ATTR_REVERSE, "d-rev: cursor row has REVERSE after d");
        ASSERT(attr & PTY_ATTR_STRIKE,  "d-rev: cursor row has STRIKE after d");
    }

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
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
    RUN_TEST(test_d_key_row_gone_after_refresh);

    stop_gmail_mock();

done:
    if (old_home) setenv("HOME", old_home, 1);

    printf("\n--- Gmail TUI PTY Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
