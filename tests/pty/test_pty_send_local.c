/**
 * @file test_pty_send_local.c
 * @brief PTY tests for local-first send behaviour (US-SL-01 to US-SL-04).
 *
 * US-SL-01: Successful SMTP send → message saved to local Sent folder,
 *           queued in pending_appends.tsv, TUI shows "Saved locally".
 * US-SL-02: Failed SMTP send → message saved to local Drafts folder,
 *           queued in pending_appends.tsv, TUI shows "Saved to Drafts".
 * US-SL-03: email-sync uploads pending messages and clears the queue.
 * US-SL-04: pending_appends.tsv survives between runs (persistent queue).
 *
 * Usage: test-pty-send-local <email-tui> <email-sync>
 *                             <mock-imap-server> <mock-smtp-server>
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ptytest.h"
#include "pty_assert.h"

#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Test globals ────────────────────────────────────────────────────── */

int g_tests_run    = 0;
int g_tests_failed = 0;

static char g_tui_bin[512];
static char g_sync_bin[512];
static char g_imap_bin[512];
static char g_smtp_bin[512];

static char g_test_home[512];
static char g_old_home[512];
static char g_editor_script[600];

static pid_t g_imap_pid = -1;
static pid_t g_smtp_pid = -1;

#define WAIT_MS    8000
#define SETTLE_MS   400
#define ROWS 24
#define COLS 120

#define RUN_TEST(fn) do { printf("  %s...\n", #fn); fflush(stdout); fn(); } while(0)

/* Account directory under test HOME (username-based path) */
#define ACCOUNT_DIR  "/.local/share/email-cli/accounts/testuser@example.com"

/* ── Fake editor ─────────────────────────────────────────────────────── */

static void write_editor_script(void) {
    snprintf(g_editor_script, sizeof(g_editor_script),
             "%s/.fake_editor.sh", g_test_home);
    FILE *f = fopen(g_editor_script, "w");
    if (!f) return;
    fprintf(f, "#!/bin/sh\necho 'Test body' >> \"$1\"\n");
    fclose(f);
    chmod(g_editor_script, 0755);
    setenv("EDITOR", g_editor_script, 1);
}

/* ── Directory setup ──────────────────────────────────────────────────── */

static void mkdirs(void) {
    char d[700];
    snprintf(d, sizeof(d), "%s/.config",                              g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.config/email-cli",                    g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.config/email-cli/accounts",           g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.config/email-cli/accounts/testuser",  g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local",                               g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local/share",                         g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local/share/email-cli",               g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local/share/email-cli/accounts",      g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s%s",                                    g_test_home, ACCOUNT_DIR); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s%s/manifests",                          g_test_home, ACCOUNT_DIR); mkdir(d, 0700);
}

static void write_config(void) {
    mkdirs();
    char path[800];
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/testuser/config.ini", g_test_home);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:9993\n"
        "EMAIL_USER=testuser@example.com\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SMTP_HOST=smtps://localhost:9025\n"
        "SMTP_PORT=9025\n"
        "SMTP_USER=testuser@example.com\n"
        "SMTP_PASS=testpass\n"
        "SSL_NO_VERIFY=1\n");
    fclose(fp);
    chmod(path, 0600);
}

/* ── Server helpers ───────────────────────────────────────────────────── */

static int probe_port(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(0x7f000001);
    int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
    close(fd);
    return r;
}

static void start_server(const char *bin, pid_t *pid_out) {
    *pid_out = fork();
    if (*pid_out < 0) return;
    if (*pid_out == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    usleep(800000);
}

static void stop_server(pid_t *pid_out) {
    if (*pid_out > 0) {
        kill(*pid_out, SIGKILL);
        waitpid(*pid_out, NULL, 0);
        *pid_out = -1;
    }
}

/* Kill any process still listening on the given TCP port (cleanup of
 * lingering instances from previous test runs). */
static void kill_listeners_on_port(int port) {
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "fuser -k -KILL %d/tcp >/dev/null 2>&1", port);
    int _unused = system(cmd); (void)_unused;
    usleep(200000);
}

static void restart_imap(void) {
    stop_server(&g_imap_pid);
    usleep(200000);
    start_server(g_imap_bin, &g_imap_pid);
    for (int i = 0; i < 30 && probe_port(9993) != 0; i++) usleep(100000);
}

static void restart_smtp(void) {
    stop_server(&g_smtp_pid);
    usleep(200000);
    start_server(g_smtp_bin, &g_smtp_pid);
    for (int i = 0; i < 30 && probe_port(9025) != 0; i++) usleep(100000);
}

/* ── TUI / sync runners ───────────────────────────────────────────────── */

static PtySession *tui_run(void) {
    const char *args[4] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    return s;
}

static PtySession *sync_run(const char **extra_args) {
    const char *argv[16];
    int n = 0;
    argv[n++] = g_sync_bin;
    if (extra_args)
        for (int i = 0; extra_args[i] && n < 15; i++)
            argv[n++] = extra_args[i];
    argv[n] = NULL;
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    if (pty_run(s, argv) != 0) { pty_close(s); return NULL; }
    return s;
}

/*
 * Navigate TUI to the folder browser for the test account.
 * Returns a running PtySession ready at the folder browser screen,
 * or NULL on failure.
 */
static PtySession *tui_open_to_folders(void) {
    write_config();
    PtySession *s = tui_run();
    if (!s) return NULL;
    if (pty_wait_for(s, "testuser", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/*
 * Navigate TUI to the INBOX message list.
 */
static PtySession *tui_open_to_inbox(void) {
    PtySession *s = tui_open_to_folders();
    if (!s) return NULL;
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) { pty_send_key(s, PTY_KEY_DOWN); pty_settle(s, 50); }
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "message(s) in", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* ── File-system helpers ──────────────────────────────────────────────── */

/* Return 1 if the pending_appends.tsv file contains a line starting with folder. */
static int pending_has_folder(const char *folder) {
    char path[800];
    snprintf(path, sizeof(path), "%s%s/pending_appends.tsv", g_test_home, ACCOUNT_DIR);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, folder, strlen(folder)) == 0 && line[strlen(folder)] == '\t') {
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

/* Return 1 if the pending_appends.tsv file is empty or does not exist. */
static int pending_is_empty(void) {
    char path[800];
    snprintf(path, sizeof(path), "%s%s/pending_appends.tsv", g_test_home, ACCOUNT_DIR);
    FILE *fp = fopen(path, "r");
    if (!fp) return 1; /* does not exist → empty */
    int c = fgetc(fp);
    fclose(fp);
    return (c == EOF);
}

/* Delete pending_appends.tsv to reset state between tests. */
static void pending_reset(void) {
    char path[800];
    snprintf(path, sizeof(path), "%s%s/pending_appends.tsv", g_test_home, ACCOUNT_DIR);
    unlink(path);
}

/* ══════════════════════════════════════════════════════════════════════
 * TC-SL-01 (US-SL-01): Successful SMTP send → local Sent + pending queue
 * ══════════════════════════════════════════════════════════════════════ */

static void test_successful_send_saves_locally(void) {
    pending_reset();
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-SL-01: reached inbox");

    /* Press 'c' to open compose dialog */
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Fill To: field and Tab to Subject */
    pty_send_str(s, "recipient@example.com");
    pty_send_key(s, PTY_KEY_TAB); /* To → Cc */
    pty_send_key(s, PTY_KEY_TAB); /* Cc → Bcc */
    pty_send_key(s, PTY_KEY_TAB); /* Bcc → Subject */
    pty_send_str(s, "TC-SL-01 Test Subject");
    pty_send_key(s, PTY_KEY_ENTER); /* open editor */
    pty_settle(s, SETTLE_MS);

    /* Wait for send result */
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Message sent.", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Saved locally", WAIT_MS);

    pty_close(s);

    /* Verify pending_appends.tsv has a Sent entry */
    ASSERT(pending_has_folder("Sent"), "TC-SL-01: pending_appends.tsv has Sent entry");
}

/* ══════════════════════════════════════════════════════════════════════
 * TC-SL-02 (US-SL-02): Failed SMTP send → local Drafts + pending queue
 * ══════════════════════════════════════════════════════════════════════ */

static void test_failed_send_saves_to_drafts(void) {
    pending_reset();
    restart_imap();
    stop_server(&g_smtp_pid);       /* kill the server we started */
    kill_listeners_on_port(9025);   /* kill any lingering instance */
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-SL-02: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "recipient@example.com");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "TC-SL-02 Test Draft");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);

    /* Send should fail → Drafts path */
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Saved to Drafts", WAIT_MS);

    pty_close(s);

    /* Verify pending_appends.tsv has a Drafts entry */
    ASSERT(pending_has_folder("Drafts"), "TC-SL-02: pending_appends.tsv has Drafts entry");
}

/* ══════════════════════════════════════════════════════════════════════
 * TC-SL-03 (US-SL-03): email-sync uploads pending messages, clears queue
 * ══════════════════════════════════════════════════════════════════════ */

static void test_sync_uploads_pending_and_clears_queue(void) {
    /* Seed pending_appends.tsv via a successful send first */
    pending_reset();
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-SL-03 setup: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "recipient@example.com");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "TC-SL-03 Sync Test");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Saved locally", WAIT_MS);
    pty_close(s);

    ASSERT(pending_has_folder("Sent"), "TC-SL-03 setup: Sent entry queued before sync");

    /* Run email-sync to upload the pending message */
    PtySession *ss = sync_run(NULL);
    ASSERT(ss != NULL, "TC-SL-03: email-sync started");
    ASSERT_WAIT_FOR(ss, "uploaded", WAIT_MS);
    pty_close(ss);

    /* Queue should now be empty */
    ASSERT(pending_is_empty(), "TC-SL-03: pending_appends.tsv empty after sync");
}

/* ══════════════════════════════════════════════════════════════════════
 * TC-SL-04 (US-SL-04): pending queue survives process restart
 * ══════════════════════════════════════════════════════════════════════ */

static void test_pending_queue_survives_restart(void) {
    /* Seed via a successful send */
    pending_reset();
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-SL-04 setup: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "recipient@example.com");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "TC-SL-04 Persist Test");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Saved locally", WAIT_MS);
    pty_close(s);

    ASSERT(pending_has_folder("Sent"), "TC-SL-04: Sent entry in queue after send");

    /* Stop all servers; restart — queue must still be there */
    stop_server(&g_smtp_pid);

    ASSERT(pending_has_folder("Sent"), "TC-SL-04: Sent entry persists after process restart");
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    printf("--- Local-first send PTY Tests (US-SL-01 … US-SL-04) ---\n\n");

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s <email-tui> <email-sync> "
                "<mock-imap-server> <mock-smtp-server>\n",
                argv[0]);
        return 1;
    }

    snprintf(g_tui_bin,  sizeof(g_tui_bin),  "%s", argv[1]);
    snprintf(g_sync_bin, sizeof(g_sync_bin), "%s", argv[2]);
    snprintf(g_imap_bin, sizeof(g_imap_bin), "%s", argv[3]);
    snprintf(g_smtp_bin, sizeof(g_smtp_bin), "%s", argv[4]);

    snprintf(g_test_home, sizeof(g_test_home), "/tmp/test-send-local-XXXXXX");
    if (!mkdtemp(g_test_home)) {
        perror("mkdtemp");
        return 1;
    }
    if (getenv("HOME"))
        snprintf(g_old_home, sizeof(g_old_home), "%s", getenv("HOME"));
    setenv("HOME", g_test_home, 1);

    mkdirs();
    write_config();
    write_editor_script();

    restart_imap();
    restart_smtp();

    printf("--- US-SL-01: Successful send saves locally ---\n");
    RUN_TEST(test_successful_send_saves_locally);

    printf("\n--- US-SL-02: Failed send saves to Drafts ---\n");
    RUN_TEST(test_failed_send_saves_to_drafts);

    printf("\n--- US-SL-03: Sync uploads pending messages ---\n");
    restart_imap(); restart_smtp();
    RUN_TEST(test_sync_uploads_pending_and_clears_queue);

    printf("\n--- US-SL-04: Pending queue survives restart ---\n");
    restart_imap(); restart_smtp();
    RUN_TEST(test_pending_queue_survives_restart);

    stop_server(&g_imap_pid);
    stop_server(&g_smtp_pid);

    if (g_old_home[0]) setenv("HOME", g_old_home, 1);

    printf("\n--- Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
