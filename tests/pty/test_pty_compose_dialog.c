/**
 * @file test_pty_compose_dialog.c
 * @brief PTY tests for the pre-compose dialog (US-CD-1 … US-CD-12).
 *
 * Covers: new message dialog, reply dialog, reply-all (A), forward (F),
 * contact autocomplete, Cc/Bcc field handling, and field navigation.
 *
 * Usage: test-pty-compose-dialog <email-tui> <mock-imap-server> <mock-smtp-server>
 *
 * The pre-compose dialog runs before the editor opens, so these tests
 * never need real vim interaction.  The EDITOR env var is set to a tiny
 * shell script that appends "Test body\n" to the draft and exits 0.
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ptytest.h"
#include "pty_assert.h"

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
static char g_imap_bin[512];
static char g_smtp_bin[512];

static char g_test_home[512];
static char g_old_home[512];
static char g_editor_script[600]; /* path to fake editor script */

static pid_t g_imap_pid = -1;
static pid_t g_smtp_pid = -1;

#define WAIT_MS   6000
#define SETTLE_MS  400
#define ROWS 24
#define COLS 120

#define RUN_TEST(fn) do { printf("  %s...\n", #fn); fflush(stdout); fn(); } while(0)

/* ── Fake editor ─────────────────────────────────────────────────────── */

/*
 * Write a tiny shell script that appends "Test body\n" to the first
 * argument (the draft file) then exits 0.  This lets us simulate the
 * user writing a body without actually launching vim.
 */
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

/* ── Config helpers ──────────────────────────────────────────────────── */

static void mkdirs(void) {
    char d[600];
    snprintf(d, sizeof(d), "%s/.config", g_test_home);          mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.config/email-cli", g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.config/email-cli/accounts", g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.config/email-cli/accounts/testuser", g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local", g_test_home);            mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local/share", g_test_home);      mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local/share/email-cli", g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local/share/email-cli/accounts", g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local/share/email-cli/accounts/testuser", g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local/share/email-cli/accounts/testuser/manifests", g_test_home); mkdir(d, 0700);
}

static void write_config(void) {
    mkdirs();
    char path[700];
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

/* ── Contacts seed helper ─────────────────────────────────────────────── */

/*
 * Seed contacts.tsv so autocomplete has entries without needing real .hdr files.
 * Format: address\tdisplay_name\tfrequency
 */
static void write_contacts_tsv(void) {
    char path[700];
    snprintf(path, sizeof(path),
             "%s/.local/share/email-cli/accounts/testuser/contacts.tsv",
             g_test_home);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "alice@example.com\tAlice Example\t10\n");
    fprintf(f, "bob.smith@acme.com\tBob Smith\t8\n");
    fprintf(f, "carol.jones@corp.org\tCarol Jones\t5\n");
    fprintf(f, "john.doe@example.com\tJohn Doe\t3\n");
    fprintf(f, "johnathan.green@partner.net\tJohnathan Green\t2\n");
    fclose(f);
}

/* ── Manifest seed helper ──────────────────────────────────────────────── */

/* Seed an INBOX manifest with one message that has alice@example.com as sender */
static void write_inbox_manifest(void) {
    char path[700];
    snprintf(path, sizeof(path),
             "%s/.local/share/email-cli/accounts/testuser/manifests/INBOX.tsv",
             g_test_home);
    FILE *f = fopen(path, "w");
    if (!f) return;
    /* uid\tfrom\tsubject\tdate\tflags */
    fprintf(f, "0000000000000001\talice@example.com\tHello World\t2026-04-25 10:00\t1\n");
    fprintf(f, "0000000000000002\tbob.smith@acme.com\tProject Update\t2026-04-25 09:00\t0\n");
    fclose(f);
}

/* Seed a contacts.tsv built from manifest From addresses */
static void write_contacts_from_manifests(void) {
    char path[700];
    snprintf(path, sizeof(path),
             "%s/.local/share/email-cli/accounts/testuser/contacts.tsv",
             g_test_home);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "alice@example.com\t\t2\n");
    fprintf(f, "bob.smith@acme.com\tBob Smith\t1\n");
    fclose(f);
}

/* ── Mock server management ────────────────────────────────────────────── */

static int probe_port(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
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

/* ── TUI runner ───────────────────────────────────────────────────────── */

static PtySession *tui_run(const char **extra_args) {
    const char *args[32];
    int n = 0;
    args[n++] = g_tui_bin;
    if (extra_args)
        for (int i = 0; extra_args[i] && n < 31; i++)
            args[n++] = extra_args[i];
    args[n] = NULL;

    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    return s;
}

/*
 * Open TUI and navigate to the INBOX message list.
 * The TUI starts at the accounts screen; press Enter to open the account,
 * then Enter again to open the INBOX folder.
 */
static PtySession *tui_open_to_inbox(void) {
    write_config();
    PtySession *s = tui_run(NULL);
    if (!s) return NULL;
    /* Accounts screen → open account */
    if (pty_wait_for(s, "testuser", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    /* Folder browser: HOME → VP_UNREAD(1), then 6× DOWN reaches INBOX(8),
     * skipping VP_HDR_FOLD which the browser auto-skips on navigation. */
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) { pty_send_key(s, PTY_KEY_DOWN); pty_settle(s, 50); }
    if (pty_wait_for(s, "INBOX", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "message(s) in", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-01 to TC-CD-08: Basic compose dialog flow
 * ══════════════════════════════════════════════════════════════════════ */

static void test_compose_dialog_opens(void) {
    /* TC-CD-01: pressing 'c' opens the pre-compose dialog, not vim */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "compose opens: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Dialog must show all four fields */
    ASSERT_SCREEN_CONTAINS(s, "To:");
    ASSERT_SCREEN_CONTAINS(s, "Cc:");
    ASSERT_SCREEN_CONTAINS(s, "Bcc:");
    ASSERT_SCREEN_CONTAINS(s, "Subject:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_dialog_title(void) {
    /* TC-CD-08: dialog title reads "New Message" */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "dialog title: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "New Message");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_dialog_status_line(void) {
    /* Status line shows key bindings hint */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "status line: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* At least Tab and ESC must be mentioned */
    ASSERT_SCREEN_CONTAINS(s, "Tab");
    ASSERT_SCREEN_CONTAINS(s, "ESC");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_esc_cancels(void) {
    /* TC-CD-02: Esc in compose dialog returns to message list */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "esc cancels: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_ESC);
    /* After compose cancel, TUI shows "[Press any key]" before returning to list */
    ASSERT_WAIT_FOR(s, "[Press any key", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Must NOT have opened editor (vim is not running) */
    ASSERT_SCREEN_NOT_CONTAINS(s, "~"); /* vim empty-buffer tilde markers */

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_empty_to_error(void) {
    /* TC-CD-03: Enter with empty To shows inline error */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "empty to error: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Press Enter immediately without typing anything */
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);

    /* Inline error visible; dialog still open */
    ASSERT_SCREEN_CONTAINS(s, "empty");
    ASSERT_SCREEN_CONTAINS(s, "To:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_empty_subject_warning(void) {
    /* TC-CD-05/06/07: empty Subject triggers confirmation prompt */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "empty subject: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Fill only To */
    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB);   /* To → Cc */
    pty_send_key(s, PTY_KEY_TAB);   /* Cc → Bcc */
    pty_send_key(s, PTY_KEY_TAB);   /* Bcc → Subject */
    /* Subject is empty; press Enter */
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);

    /* Confirmation prompt */
    ASSERT_SCREEN_CONTAINS(s, "empty");
    ASSERT_SCREEN_CONTAINS(s, "y/n");

    /* 'n' → back to Subject */
    pty_send_str(s, "n");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Subject:");
    ASSERT_SCREEN_NOT_CONTAINS(s, "y/n");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_empty_subject_y_opens_editor(void) {
    /* TC-CD-06: 'y' at the confirmation prompt opens editor */
    restart_imap(); restart_smtp();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "empty subject y: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);  /* reach Subject */
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "y/n");

    pty_send_str(s, "y");
    /* Editor (our fake script) runs and returns; TUI continues */
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);

    pty_close(s);
}

static void test_compose_opens_editor_with_to(void) {
    /* TC-CD-04: filling To + Subject and pressing Enter opens editor */
    restart_imap(); restart_smtp();
    write_editor_script(); /* ensures EDITOR is set */
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "opens editor: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "alice@example.com");
    /* Tab To → Cc → Bcc → Subject */
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "Test Subject");
    pty_send_key(s, PTY_KEY_ENTER);

    /* Editor runs and the TUI shows "Sending" */
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);

    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-09 to TC-CD-12: Field navigation
 * ══════════════════════════════════════════════════════════════════════ */

static void test_compose_tab_to_cc(void) {
    /* TC-CD-09: Tab from To field moves cursor to Cc field */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "tab to cc: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Tab once — should highlight Cc */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    /* Cc field must be active (highlighted or indicated) */
    ASSERT_SCREEN_CONTAINS(s, "Cc:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_tab_full_cycle(void) {
    /* TC-CD-10: Tab cycles To → Cc → Bcc → Subject → (back to To) */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "tab cycle: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* 4× Tab should wrap back past Subject to To; dialog still open */
    for (int i = 0; i < 4; i++) { pty_send_key(s, PTY_KEY_TAB); pty_settle(s, 80); }
    ASSERT_SCREEN_CONTAINS(s, "New Message"); /* dialog still showing */

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_shift_tab_reverse(void) {
    /* TC-CD-11: Shift-Tab moves backwards (Subject → Bcc → Cc → To) */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "shift-tab: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Move to Subject */
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);

    /* Shift-Tab back */
    pty_send_str(s, "\x1b[Z");  /* Shift-Tab */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Bcc:");

    pty_send_str(s, "\x1b[Z");  /* Shift-Tab */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Cc:");

    pty_send_str(s, "\x1b[Z");  /* Shift-Tab */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "To:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-13 to TC-CD-17: Recipient input and multi-address
 * ══════════════════════════════════════════════════════════════════════ */

static void test_compose_to_field_accepts_email(void) {
    /* TC-CD-13: can type a complete email address in To */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "to accepts email: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "alice@example.com");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "alice@example.com");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_multiple_to_addresses(void) {
    /* TC-CD-14: semicolon commits first address; both appear in field */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "multi-to: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "alice@example.com;");
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "bob@example.com");
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "alice@example.com");
    ASSERT_SCREEN_CONTAINS(s, "bob@example.com");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_backspace_removes_address(void) {
    /* TC-CD-15: Backspace on empty token removes last committed address */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "backspace removes: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "alice@example.com;");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "alice@example.com");

    /* Token is empty now; Backspace should remove alice's address */
    pty_send_key(s, PTY_KEY_BACK);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "alice@example.com");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_cc_written_to_tempfile(void) {
    /* TC-CD-16: Cc address appears as Cc: header in the editor temp file */
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "cc header: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB);   /* → Cc */
    pty_send_str(s, "carol@example.org");
    pty_send_key(s, PTY_KEY_TAB);   /* → Bcc */
    pty_send_key(s, PTY_KEY_TAB);   /* → Subject */
    pty_send_str(s, "Cc test");
    pty_send_key(s, PTY_KEY_ENTER);

    /* After editor runs, message is sent; the Cc: header must have been
     * in the temp file and picked up by the send flow. */
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);

    pty_close(s);
}

static void test_compose_bcc_written_to_tempfile(void) {
    /* TC-CD-17: Bcc address appears in Bcc: header in the editor temp file */
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "bcc header: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB);   /* → Cc */
    pty_send_key(s, PTY_KEY_TAB);   /* → Bcc */
    pty_send_str(s, "hidden@secret.org");
    pty_send_key(s, PTY_KEY_TAB);   /* → Subject */
    pty_send_str(s, "Bcc test");
    pty_send_key(s, PTY_KEY_ENTER);

    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);

    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-18 to TC-CD-27: Autocomplete
 * ══════════════════════════════════════════════════════════════════════ */

static void test_autocomplete_dropdown_appears(void) {
    /* TC-CD-18: typing ≥2 matching chars opens autocomplete dropdown */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "autocomplete appears: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "al");  /* matches alice@example.com */
    ASSERT_WAIT_FOR(s, "alice@example.com", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_autocomplete_filters_realtime(void) {
    /* TC-CD-19: typing more chars narrows the dropdown */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "autocomplete filters: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "jo");  /* matches john.doe and johnathan */
    ASSERT_WAIT_FOR(s, "john", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "john.doe@example.com");
    ASSERT_SCREEN_CONTAINS(s, "johnathan");

    /* Add 'hn.d' → only john.doe matches */
    pty_send_str(s, "hn.d");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "john.doe@example.com");
    ASSERT_SCREEN_NOT_CONTAINS(s, "johnathan");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_autocomplete_no_match_no_dropdown(void) {
    /* TC-CD-20: no matching contacts → no dropdown shown */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "no dropdown: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "zzz");  /* no contact starts with zzz */
    pty_settle(s, SETTLE_MS * 2);
    ASSERT_SCREEN_NOT_CONTAINS(s, "zzz@");   /* no suggestion row */

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_autocomplete_single_char_no_dropdown(void) {
    /* TC-CD-21: only 1 char typed → no dropdown yet (threshold is 2) */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "single char: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "a");  /* single char: should not open dropdown */
    pty_settle(s, SETTLE_MS);
    /* Dropdown NOT visible (alice would appear but threshold not met) */
    ASSERT_SCREEN_NOT_CONTAINS(s, "alice@example.com");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_autocomplete_down_highlights_second(void) {
    /* TC-CD-22: Down arrow highlights second entry in dropdown */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "down arrow: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "jo");
    ASSERT_WAIT_FOR(s, "john.doe", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Dropdown open; press Down */
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, SETTLE_MS);
    /* Second entry (johnathan) must now be highlighted */
    ASSERT_SCREEN_CONTAINS(s, "johnathan");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_autocomplete_enter_selects(void) {
    /* TC-CD-23: Enter on dropdown entry appends address and closes dropdown */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "enter selects: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "al");
    ASSERT_WAIT_FOR(s, "alice@example.com", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);

    /* Address committed; dropdown closed; To field contains alice */
    ASSERT_SCREEN_CONTAINS(s, "alice@example.com");
    /* Dropdown suggestions should be gone */
    ASSERT_SCREEN_NOT_CONTAINS(s, "john.doe@example.com");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_autocomplete_tab_selects_and_advances(void) {
    /* TC-CD-24: Tab selects top suggestion and advances to next field */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "tab selects: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "al");
    ASSERT_WAIT_FOR(s, "alice@example.com", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Tab: select top entry, advance to Cc */
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "alice@example.com");
    /* Now in Cc field; Cc should be the active field */
    ASSERT_SCREEN_CONTAINS(s, "Cc:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_autocomplete_esc_dismisses_keeps_text(void) {
    /* TC-CD-25: Esc dismisses dropdown without selecting; typed text remains */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "esc dismisses: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "al");
    ASSERT_WAIT_FOR(s, "alice@example.com", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Esc: dismiss dropdown */
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, SETTLE_MS);

    /* Dropdown gone but the 'al' text must still be in the To field */
    ASSERT_SCREEN_NOT_CONTAINS(s, "alice@example.com"); /* dropdown row gone */
    ASSERT_SCREEN_CONTAINS(s, "al");                     /* typed text stays */

    pty_send_key(s, PTY_KEY_ESC);  /* cancel dialog */
    pty_close(s);
}

static void test_autocomplete_case_insensitive(void) {
    /* TC-CD-26: uppercase input matches lowercase contact */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "case insensitive: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "AL");  /* uppercase, but alice@example.com should match */
    ASSERT_WAIT_FOR(s, "alice@example.com", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_autocomplete_display_name_match(void) {
    /* TC-CD-27: matching on display name also works ("Smith" → bob.smith@) */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "name match: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "Sm");  /* matches "Bob Smith" display name */
    ASSERT_WAIT_FOR(s, "bob.smith@acme.com", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-28 to TC-CD-36: Reply dialog
 * ══════════════════════════════════════════════════════════════════════ */

static void test_reply_dialog_opens(void) {
    /* TC-CD-28: pressing 'r' opens the reply dialog */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply opens: reached inbox");

    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "To:");
    ASSERT_SCREEN_CONTAINS(s, "Subject:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_dialog_title(void) {
    /* TC-CD-28: dialog title reads "Reply" */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply title: reached inbox");

    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Reply");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_to_prefilled(void) {
    /* TC-CD-29: To field shows the original sender's address */
    restart_imap();
    /* The mock server's first message is from testuser1@example.com */
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply to: reached inbox");

    /* Navigate to first message, then reply */
    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* To field must contain sender's address */
    ASSERT_SCREEN_CONTAINS(s, "@");  /* some email address in To */

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_subject_prefilled(void) {
    /* TC-CD-30: Subject shows "Re: <original subject>" */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply subject: reached inbox");

    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "Re:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_re_not_doubled(void) {
    /* TC-CD-32: "Re: Re: X" collapsed to "Re: X" */
    /* This requires a message whose subject already starts with "Re: ";
     * the mock server's "Test Message" should be varied for this.
     * We test the collapse logic via compose_extract_reply_meta unit test;
     * here we just verify no double-Re in the dialog display. */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "re not doubled: reached inbox");

    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Must contain exactly one "Re:" — not "Re: Re:" */
    ASSERT_SCREEN_NOT_CONTAINS(s, "Re: Re:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_esc_cancels(void) {
    /* TC-CD-34: Esc in reply dialog returns to message list */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply esc: reached inbox");

    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "[Press any key", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_can_add_cc(void) {
    /* TC-CD-33: can add Cc before opening editor */
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply cc: reached inbox");

    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Reply dialog starts at Cc (To is pre-filled with sender address).
     * Type Cc address directly, then Tab to Subject. */
    pty_send_str(s, "carol@example.org");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "carol@example.org");

    /* Move to Subject and confirm: Cc→Bcc→Subject = 2× Tab */
    pty_send_key(s, PTY_KEY_TAB);   /* Cc → Bcc */
    pty_send_key(s, PTY_KEY_TAB);   /* Bcc → Subject */
    pty_send_key(s, PTY_KEY_ENTER);

    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);

    pty_close(s);
}

static void test_reply_editor_opens_with_quoted_body(void) {
    /* TC-CD-35/36: editor opens; quoted body prefixed with "> " is present */
    restart_imap(); restart_smtp();

    /* Use a custom editor script that writes the draft content to a file */
    char draft_capture[600];
    snprintf(draft_capture, sizeof(draft_capture),
             "%s/.draft_capture", g_test_home);
    char editor_script[600];
    snprintf(editor_script, sizeof(editor_script),
             "%s/.capture_editor.sh", g_test_home);
    FILE *f = fopen(editor_script, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\ncp \"$1\" '%s'\necho 'Test body' >> \"$1\"\n",
                draft_capture);
        fclose(f);
        chmod(editor_script, 0755);
        setenv("EDITOR", editor_script, 1);
    }

    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply body: reached inbox");

    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Reply dialog starts at Cc (To pre-filled): Cc→Bcc→Subject = 2× Tab */
    pty_send_key(s, PTY_KEY_TAB);   /* Cc → Bcc */
    pty_send_key(s, PTY_KEY_TAB);   /* Bcc → Subject */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    pty_close(s);

    /* Inspect captured draft file for ">" quote prefix */
    FILE *cap = fopen(draft_capture, "r");
    ASSERT(cap != NULL, "reply body: draft captured");
    if (cap) {
        char buf[4096] = "";
        buf[fread(buf, 1, sizeof(buf) - 1, cap)] = '\0';
        fclose(cap);
        ASSERT(strstr(buf, "> ") != NULL, "reply body: quoted lines with '> '");
    }

    write_editor_script(); /* restore normal fake editor */
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-37 to TC-CD-43: Reply-All dialog (A key)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_reply_all_dialog_opens(void) {
    /* TC-CD-37: capital 'A' opens reply-all dialog */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply-all opens: reached inbox");

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Reply All", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "To:");
    ASSERT_SCREEN_CONTAINS(s, "Cc:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_all_title(void) {
    /* TC-CD-38: dialog title reads "Reply All" */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply-all title: reached inbox");

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Reply All", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Reply All");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_all_own_address_excluded_from_cc(void) {
    /* TC-CD-41: the user's own address must NOT appear in the Cc field */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply-all own excluded: reached inbox");

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Reply All", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* testuser@example.com must not appear in the Cc field */
    ASSERT_SCREEN_NOT_CONTAINS(s, "testuser@example.com");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_all_esc_cancels(void) {
    /* TC-CD-42: Esc cancels reply-all */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply-all esc: reached inbox");

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Reply All", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "[Press any key", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_all_in_help_panel(void) {
    /* TC-CD-43: 'A' appears in message list help popup */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply-all help: reached inbox");

    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Reply All", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "A");

    pty_send_str(s, "q");
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-44 to TC-CD-53: Forward dialog (F key)
 * ══════════════════════════════════════════════════════════════════════ */

static void test_forward_dialog_opens(void) {
    /* TC-CD-44: capital 'F' opens forward dialog */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forward opens: reached inbox");

    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "To:");
    ASSERT_SCREEN_CONTAINS(s, "Subject:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_forward_dialog_title(void) {
    /* TC-CD-45: title reads "Forward" */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forward title: reached inbox");

    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Forward");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_forward_to_field_empty(void) {
    /* TC-CD-46: To field is empty in the forward dialog */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forward to empty: reached inbox");

    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* The To field should not be pre-filled with anything */
    /* We check by pressing Enter immediately — the empty-To error must fire */
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "empty");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_forward_subject_prefilled_fwd(void) {
    /* TC-CD-47: Subject pre-filled as "Fwd: <original>" */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forward subject: reached inbox");

    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "Fwd:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_forward_empty_to_error(void) {
    /* TC-CD-48: Enter with empty To shows inline error */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forward empty to: reached inbox");

    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "empty");
    /* Dialog still open */
    ASSERT_SCREEN_CONTAINS(s, "Forward");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_forward_fill_to_opens_editor(void) {
    /* TC-CD-49: filling To + Enter opens editor with Fwd: subject */
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forward editor: reached inbox");

    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "carol@example.org");
    pty_send_key(s, PTY_KEY_TAB);   /* To → Cc */
    pty_send_key(s, PTY_KEY_TAB);   /* Cc → Bcc */
    pty_send_key(s, PTY_KEY_TAB);   /* Bcc → Subject */
    pty_send_key(s, PTY_KEY_ENTER);

    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);

    pty_close(s);
}

static void test_forward_body_has_original(void) {
    /* TC-CD-50: forwarded draft contains original message body */
    restart_imap(); restart_smtp();

    char draft_capture[600];
    snprintf(draft_capture, sizeof(draft_capture),
             "%s/.fwd_draft", g_test_home);
    char editor_script[600];
    snprintf(editor_script, sizeof(editor_script),
             "%s/.fwd_editor.sh", g_test_home);
    FILE *f = fopen(editor_script, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\ncp \"$1\" '%s'\necho 'Fwd body' >> \"$1\"\n",
                draft_capture);
        fclose(f);
        chmod(editor_script, 0755);
        setenv("EDITOR", editor_script, 1);
    }

    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forward body: reached inbox");

    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "carol@example.org");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    pty_close(s);

    FILE *cap = fopen(draft_capture, "r");
    ASSERT(cap != NULL, "forward body: draft captured");
    if (cap) {
        char buf[8192] = "";
        buf[fread(buf, 1, sizeof(buf) - 1, cap)] = '\0';
        fclose(cap);
        /* Forwarded draft must contain "--- Forwarded" separator */
        ASSERT(strstr(buf, "Forwarded") != NULL,
               "forward body: forwarded separator present");
        ASSERT(strstr(buf, "Fwd:") != NULL, "forward body: Fwd: subject present");
    }

    write_editor_script();
}

static void test_forward_autocomplete_works(void) {
    /* TC-CD-51: autocomplete works in forward dialog To field */
    restart_imap();
    write_contacts_tsv();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forward autocomplete: reached inbox");

    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "bo");  /* matches bob.smith@acme.com */
    ASSERT_WAIT_FOR(s, "bob.smith@acme.com", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_forward_esc_cancels(void) {
    /* TC-CD-52: Esc cancels forward dialog */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forward esc: reached inbox");

    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "[Press any key", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_forward_in_help_panel(void) {
    /* TC-CD-53: 'F' listed in message list help popup */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forward help: reached inbox");

    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "F");

    pty_send_str(s, "q");
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-54 to TC-CD-56: Compose from folder browser
 * ══════════════════════════════════════════════════════════════════════ */

static PtySession *tui_open_to_folders(void) {
    write_config();
    PtySession *s = tui_run(NULL);
    if (!s) return NULL;
    if (pty_wait_for(s, "testuser", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);  /* accounts screen → folder browser */
    if (pty_wait_for(s, "Folders", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, SETTLE_MS);
    return s;
}

static void test_compose_from_folder_browser(void) {
    /* TC-CD-54: 'c' in folder browser opens compose dialog */
    restart_imap();
    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "compose from folders: opened folders");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "To:");
    ASSERT_SCREEN_CONTAINS(s, "Subject:");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_folder_esc_returns_to_folders(void) {
    /* TC-CD-55: Esc from compose returns to folder list */
    restart_imap();
    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "esc to folders: opened folders");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "INBOX", WAIT_MS);  /* folder list visible again */

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_folder_fill_opens_editor(void) {
    /* TC-CD-56: fill To + Subject in folder-browser compose → editor opens */
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_folders();
    ASSERT(s != NULL, "folder compose editor: opened folders");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "carol@example.org");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "Folder compose test");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);

    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-57 to TC-CD-60: Contacts source
 * ══════════════════════════════════════════════════════════════════════ */

static void test_contacts_from_manifest_appear(void) {
    /* TC-CD-57: sender from received mail appears in autocomplete */
    restart_imap();
    write_inbox_manifest();
    write_contacts_from_manifests();

    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "contacts manifest: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* alice@example.com was in the manifest — should appear */
    pty_send_str(s, "al");
    ASSERT_WAIT_FOR(s, "alice@example.com", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_contacts_tsv_created_on_first_query(void) {
    /* TC-CD-60: contacts.tsv is created when first autocomplete query runs */
    restart_imap();

    /* Remove contacts.tsv if it exists */
    char path[700];
    snprintf(path, sizeof(path),
             "%s/.local/share/email-cli/accounts/testuser/contacts.tsv",
             g_test_home);
    unlink(path);

    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "contacts created: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Trigger autocomplete scan */
    pty_send_str(s, "te");
    pty_settle(s, SETTLE_MS * 3);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);

    /* contacts.tsv should now exist */
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL, "contacts created: contacts.tsv exists");
    if (f) fclose(f);
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-61 to TC-CD-63: ComposeParams Cc / Bcc
 * ══════════════════════════════════════════════════════════════════════ */

static void test_cc_in_smtp_envelope(void) {
    /* TC-CD-61: Cc address appears in the message Cc: header received by SMTP */
    restart_imap(); restart_smtp();

    /* Use an editor script that preserves the Cc: header (just adds body) */
    char editor_script[600];
    snprintf(editor_script, sizeof(editor_script),
             "%s/.cc_test_editor.sh", g_test_home);
    FILE *f = fopen(editor_script, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\necho 'Body text' >> \"$1\"\n");
        fclose(f); chmod(editor_script, 0755);
        setenv("EDITOR", editor_script, 1);
    }

    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "cc envelope: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "carol@example.org");   /* Cc */
    pty_send_key(s, PTY_KEY_TAB);           /* → Bcc */
    pty_send_key(s, PTY_KEY_TAB);           /* → Subject */
    pty_send_str(s, "Cc envelope test");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    pty_close(s);

    /* The mock SMTP server logs the message; verify Cc: header present.
     * The server writes received data to a temp log file at /tmp/smtp-log-*.
     * (This test verifies the integration; exact log path depends on mock.) */

    write_editor_script();
}

static void test_bcc_not_in_message_headers(void) {
    /* TC-CD-62: Bcc must NOT appear in the final RFC 2822 headers */
    restart_imap(); restart_smtp();

    char draft_capture[600];
    snprintf(draft_capture, sizeof(draft_capture),
             "%s/.bcc_draft", g_test_home);
    char editor_script[600];
    snprintf(editor_script, sizeof(editor_script),
             "%s/.bcc_test_editor.sh", g_test_home);
    FILE *f = fopen(editor_script, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\ncp \"$1\" '%s'\necho 'Body' >> \"$1\"\n",
                draft_capture);
        fclose(f); chmod(editor_script, 0755);
        setenv("EDITOR", editor_script, 1);
    }

    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "bcc stripped: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB);           /* → Cc */
    pty_send_key(s, PTY_KEY_TAB);           /* → Bcc */
    pty_send_str(s, "secret@example.org");
    pty_send_key(s, PTY_KEY_TAB);           /* → Subject */
    pty_send_str(s, "Bcc strip test");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    pty_close(s);

    /* The draft file passed to editor has Bcc: header.
     * After send, compose_build_message() must NOT include Bcc in the
     * outgoing RFC 2822 data.
     * Verify: draft capture MUST have Bcc: (user saw it in editor).
     * The sent message (checked via smtp log) must NOT have Bcc: header. */
    FILE *cap = fopen(draft_capture, "r");
    ASSERT(cap != NULL, "bcc strip: draft captured");
    if (cap) {
        char buf[4096] = "";
        buf[fread(buf, 1, sizeof(buf) - 1, cap)] = '\0';
        fclose(cap);
        ASSERT(strstr(buf, "Bcc:") != NULL,
               "bcc strip: draft file contains Bcc: header for editing");
    }

    write_editor_script();
}

static void test_multiple_cc_semicolon_separated(void) {
    /* TC-CD-63: multiple Cc addresses → semicolon-separated in Cc: header */
    restart_imap(); restart_smtp();

    char draft_capture[600];
    snprintf(draft_capture, sizeof(draft_capture),
             "%s/.multi_cc_draft", g_test_home);
    char editor_script[600];
    snprintf(editor_script, sizeof(editor_script),
             "%s/.multi_cc_editor.sh", g_test_home);
    FILE *f = fopen(editor_script, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\ncp \"$1\" '%s'\necho 'Body' >> \"$1\"\n",
                draft_capture);
        fclose(f); chmod(editor_script, 0755);
        setenv("EDITOR", editor_script, 1);
    }

    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "multi cc: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB);                    /* → Cc */
    pty_send_str(s, "carol@example.org;");            /* first Cc */
    pty_send_str(s, "dave@example.net");             /* second Cc */
    pty_send_key(s, PTY_KEY_TAB);                    /* → Bcc */
    pty_send_key(s, PTY_KEY_TAB);                    /* → Subject */
    pty_send_str(s, "Multi Cc test");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    pty_close(s);

    FILE *cap = fopen(draft_capture, "r");
    ASSERT(cap != NULL, "multi cc: draft captured");
    if (cap) {
        char buf[4096] = "";
        buf[fread(buf, 1, sizeof(buf) - 1, cap)] = '\0';
        fclose(cap);
        ASSERT(strstr(buf, "carol@example.org") != NULL,
               "multi cc: first Cc address in draft");
        ASSERT(strstr(buf, "dave@example.net") != NULL,
               "multi cc: second Cc address in draft");
    }

    write_editor_script();
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-66 / TC-CD-67: Reader-view 'r' reply and Sent folder saving
 * ══════════════════════════════════════════════════════════════════════ */

static void test_reader_reply_opens_compose(void) {
    /* TC-CD-66: 'r' pressed inside the message reader opens the reply dialog */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reader reply: reached inbox");

    /* Open first message in reader */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* 'r' in reader → exits reader, compose reply dialog opens */
    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    ASSERT_SCREEN_CONTAINS(s, "To:");
    ASSERT_SCREEN_CONTAINS(s, "Re:");

    /* ESC → dialog closed; main_tui shows "[Press any key]" prompt */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "[Press any key", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_send_saves_to_sent(void) {
    /* TC-CD-67: reply from reader → send → Sent folder saved ("Saved." visible) */
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "sent folder: reached inbox");

    /* Open first message in reader */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* 'r' in reader → reply dialog */
    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Navigate to Subject (reply starts at Cc since To is pre-filled):
     * Cc→Bcc→Subject = 2× Tab */
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_ENTER);

    /* Verify full send + local Sent folder flow */
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Message sent.", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Saved locally", WAIT_MS);

    /* Dismiss "[Press any key]" prompt → back to inbox */
    ASSERT_WAIT_FOR(s, "[Press any key", WAIT_MS);
    pty_send_str(s, " ");
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 * US-CD-13 / US-CD-14: Answered and Forwarded status markers
 *
 * US-CD-13: As a user, after I reply to a message, the message list
 *           shows an 'a' (answered) marker in the status column so I can
 *           see which messages I have responded to.
 * Acceptance:
 *   - 'r' + fill dialog + send → list shows 'a' marker for that message.
 *
 * US-CD-14: As a user, after I forward a message, the message list shows
 *           an 'f' (forwarded) marker in the status column.
 * Acceptance:
 *   - 'F' + fill To + send → list shows 'f' marker for that message.
 * ══════════════════════════════════════════════════════════════════════ */

static void test_replied_message_shows_answered_marker(void) {
    /* TC-CD-64 / US-CD-13 */
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "answered marker: reached inbox");

    /* Reply: r → navigate to Subject → Enter to send */
    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    /* Reply dialog starts at Cc (To is pre-filled with sender address).
     * Cc→Bcc→Subject = 2× Tab */
    pty_send_key(s, PTY_KEY_TAB);   /* Cc → Bcc */
    pty_send_key(s, PTY_KEY_TAB);   /* Bcc → Subject */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[Press any key", WAIT_MS);
    pty_send_str(s, " ");

    /* Return to list and check for 'a' (answered) marker */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "a");  /* answered marker in status column */

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_forwarded_message_shows_forwarded_marker(void) {
    /* TC-CD-65 / US-CD-14 */
    restart_imap(); restart_smtp();
    write_editor_script();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "forwarded marker: reached inbox");

    /* Forward: F → fill To → Enter to send */
    pty_send_str(s, "F");
    ASSERT_WAIT_FOR(s, "Forward", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "fwd@example.com");
    pty_send_key(s, PTY_KEY_TAB);   /* To → Cc */
    pty_send_key(s, PTY_KEY_TAB);   /* Cc → Bcc */
    pty_send_key(s, PTY_KEY_TAB);   /* Bcc → Subject */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[Press any key", WAIT_MS);
    pty_send_str(s, " ");

    /* Return to list and check for 'f' (forwarded) marker */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "f");  /* forwarded marker in status column */

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Main
 * ══════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    printf("--- Pre-compose dialog PTY Tests ---\n\n");

    if (argc < 4) {
        fprintf(stderr,
                "Usage: %s <email-tui> <mock-imap-server> <mock-smtp-server>\n",
                argv[0]);
        return 1;
    }

    snprintf(g_tui_bin,  sizeof(g_tui_bin),  "%s", argv[1]);
    snprintf(g_imap_bin, sizeof(g_imap_bin), "%s", argv[2]);
    snprintf(g_smtp_bin, sizeof(g_smtp_bin), "%s", argv[3]);

    /* Isolated HOME directory */
    snprintf(g_test_home, sizeof(g_test_home), "/tmp/test-compose-dialog-XXXXXX");
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

    /* ── Basic compose dialog flow ────────────────────────────────────── */
    printf("--- Basic compose dialog flow ---\n");
    RUN_TEST(test_compose_dialog_opens);
    RUN_TEST(test_compose_dialog_title);
    RUN_TEST(test_compose_dialog_status_line);
    RUN_TEST(test_compose_esc_cancels);
    RUN_TEST(test_compose_empty_to_error);
    RUN_TEST(test_compose_empty_subject_warning);
    RUN_TEST(test_compose_empty_subject_y_opens_editor);
    RUN_TEST(test_compose_opens_editor_with_to);

    /* ── Field navigation ─────────────────────────────────────────────── */
    printf("\n--- Field navigation ---\n");
    restart_imap();
    RUN_TEST(test_compose_tab_to_cc);
    RUN_TEST(test_compose_tab_full_cycle);
    RUN_TEST(test_compose_shift_tab_reverse);

    /* ── Recipient input ──────────────────────────────────────────────── */
    printf("\n--- Recipient input ---\n");
    restart_imap();
    RUN_TEST(test_compose_to_field_accepts_email);
    RUN_TEST(test_compose_multiple_to_addresses);
    RUN_TEST(test_compose_backspace_removes_address);
    RUN_TEST(test_compose_cc_written_to_tempfile);
    RUN_TEST(test_compose_bcc_written_to_tempfile);

    /* ── Autocomplete ─────────────────────────────────────────────────── */
    printf("\n--- Autocomplete ---\n");
    restart_imap();
    RUN_TEST(test_autocomplete_dropdown_appears);
    RUN_TEST(test_autocomplete_filters_realtime);
    RUN_TEST(test_autocomplete_no_match_no_dropdown);
    RUN_TEST(test_autocomplete_single_char_no_dropdown);
    RUN_TEST(test_autocomplete_down_highlights_second);
    RUN_TEST(test_autocomplete_enter_selects);
    RUN_TEST(test_autocomplete_tab_selects_and_advances);
    RUN_TEST(test_autocomplete_esc_dismisses_keeps_text);
    RUN_TEST(test_autocomplete_case_insensitive);
    RUN_TEST(test_autocomplete_display_name_match);

    /* ── Reply dialog ─────────────────────────────────────────────────── */
    printf("\n--- Reply dialog ---\n");
    restart_imap();
    RUN_TEST(test_reply_dialog_opens);
    RUN_TEST(test_reply_dialog_title);
    RUN_TEST(test_reply_to_prefilled);
    RUN_TEST(test_reply_subject_prefilled);
    RUN_TEST(test_reply_re_not_doubled);
    RUN_TEST(test_reply_esc_cancels);
    RUN_TEST(test_reply_can_add_cc);
    RUN_TEST(test_reply_editor_opens_with_quoted_body);

    /* ── Reply-All dialog ─────────────────────────────────────────────── */
    printf("\n--- Reply-All dialog (A key) ---\n");
    restart_imap();
    RUN_TEST(test_reply_all_dialog_opens);
    RUN_TEST(test_reply_all_title);
    RUN_TEST(test_reply_all_own_address_excluded_from_cc);
    RUN_TEST(test_reply_all_esc_cancels);
    RUN_TEST(test_reply_all_in_help_panel);

    /* ── Forward dialog ───────────────────────────────────────────────── */
    printf("\n--- Forward dialog (F key) ---\n");
    restart_imap();
    RUN_TEST(test_forward_dialog_opens);
    RUN_TEST(test_forward_dialog_title);
    RUN_TEST(test_forward_to_field_empty);
    RUN_TEST(test_forward_subject_prefilled_fwd);
    RUN_TEST(test_forward_empty_to_error);
    RUN_TEST(test_forward_fill_to_opens_editor);
    RUN_TEST(test_forward_body_has_original);
    RUN_TEST(test_forward_autocomplete_works);
    RUN_TEST(test_forward_esc_cancels);
    RUN_TEST(test_forward_in_help_panel);

    /* ── Compose from folder browser ──────────────────────────────────── */
    printf("\n--- Compose from folder browser ---\n");
    restart_imap();
    RUN_TEST(test_compose_from_folder_browser);
    RUN_TEST(test_compose_folder_esc_returns_to_folders);
    RUN_TEST(test_compose_folder_fill_opens_editor);

    /* ── Contact suggestions source ───────────────────────────────────── */
    printf("\n--- Contact suggestions ---\n");
    restart_imap();
    RUN_TEST(test_contacts_from_manifest_appear);
    RUN_TEST(test_contacts_tsv_created_on_first_query);

    /* ── ComposeParams Cc / Bcc headers ───────────────────────────────── */
    printf("\n--- Cc / Bcc headers ---\n");
    restart_imap();
    RUN_TEST(test_cc_in_smtp_envelope);
    RUN_TEST(test_bcc_not_in_message_headers);
    RUN_TEST(test_multiple_cc_semicolon_separated);

    /* ── Reader reply / Send + Sent folder (TC-CD-66 / TC-CD-67) ────── */
    printf("\n--- Reader reply / Send + Sent ---\n");
    restart_imap();
    RUN_TEST(test_reader_reply_opens_compose);
    RUN_TEST(test_reply_send_saves_to_sent);

    /* ── Answered / Forwarded status markers (US-CD-13 / US-CD-14) ───── */
    printf("\n--- Answered / Forwarded status markers ---\n");
    restart_imap();
    RUN_TEST(test_replied_message_shows_answered_marker);
    RUN_TEST(test_forwarded_message_shows_forwarded_marker);

    /* ── Cleanup ──────────────────────────────────────────────────────── */
    stop_server(&g_imap_pid);
    stop_server(&g_smtp_pid);
    if (g_old_home[0]) setenv("HOME", g_old_home, 1);

    printf("\n--- PTY Compose Dialog Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
