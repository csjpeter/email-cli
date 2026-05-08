/**
 * @file test_pty_attachment.c
 * @brief PTY tests for file attachment support in compose (US-84)
 *        and the post-compose review screen (US-85).
 *
 * These are SPECIFICATION tests written before the feature exists (TDD).
 * They will fail until US-84 and US-85 are implemented.
 *
 * Usage: test-pty-attachment <email-tui> <mock-imap-server> <mock-smtp-server>
 *
 * Test groups:
 *   TC-AT-01 … TC-AT-11   Attach field in pre-compose dialog  (US-84)
 *   TC-PCR-01 … TC-PCR-16 Post-compose review screen          (US-85)
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

/* ── Test globals ───────────────────────────────────────────────────── */

int g_tests_run    = 0;
int g_tests_failed = 0;

static char g_tui_bin[512];
static char g_imap_bin[512];
static char g_smtp_bin[512];

static char g_test_home[512];
static char g_old_home[512];
static char g_editor_script[600];

static char g_attach_file1[600];  /* report.pdf (stub) */
static char g_attach_file2[600];  /* notes.txt */

static pid_t g_imap_pid = -1;
static pid_t g_smtp_pid = -1;

#define WAIT_MS   6000
#define SETTLE_MS  400
#define ROWS 24
#define COLS 120

#define SHIFT_TAB "\x1b[Z"   /* Shift-Tab escape sequence */

#define RUN_TEST(fn) \
    do { printf("  %s...\n", #fn); fflush(stdout); fn(); } while (0)

/* ── Fake editor ────────────────────────────────────────────────────── */

static void write_editor_script(void)
{
    snprintf(g_editor_script, sizeof(g_editor_script),
             "%s/.fake_editor.sh", g_test_home);
    FILE *f = fopen(g_editor_script, "w");
    if (!f) return;
    fprintf(f, "#!/bin/sh\necho 'Test body' >> \"$1\"\n");
    fclose(f);
    chmod(g_editor_script, 0755);
    setenv("EDITOR", g_editor_script, 1);
}

/* ── Config / directory helpers ─────────────────────────────────────── */

static void mkdirs(void)
{
    char d[600];
#define MK(path) snprintf(d, sizeof(d), "%s/" path, g_test_home); mkdir(d, 0700)
    MK(".config");
    MK(".config/email-cli");
    MK(".config/email-cli/accounts");
    MK(".config/email-cli/accounts/testuser");
    MK(".local");
    MK(".local/share");
    MK(".local/share/email-cli");
    MK(".local/share/email-cli/accounts");
    MK(".local/share/email-cli/accounts/testuser");
    MK(".local/share/email-cli/accounts/testuser/manifests");
    MK("attachments");
#undef MK
}

static void write_config(void)
{
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

static void write_attach_files(void)
{
    snprintf(g_attach_file1, sizeof(g_attach_file1),
             "%s/attachments/report.pdf", g_test_home);
    snprintf(g_attach_file2, sizeof(g_attach_file2),
             "%s/attachments/notes.txt", g_test_home);

    FILE *f;
    f = fopen(g_attach_file1, "w");
    if (f) { fprintf(f, "%%PDF-1.4 stub\n"); fclose(f); }

    f = fopen(g_attach_file2, "w");
    if (f) { fprintf(f, "Meeting notes:\n- Item 1\n"); fclose(f); }
}

static void write_contacts_tsv(void)
{
    char path[700];
    snprintf(path, sizeof(path),
             "%s/.local/share/email-cli/accounts/testuser/contacts.tsv",
             g_test_home);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "alice@example.com\tAlice Example\t10\n");
    fprintf(f, "bob.smith@acme.com\tBob Smith\t8\n");
    fclose(f);
}

/* ── Mock server management ─────────────────────────────────────────── */

static void start_server(const char *bin, pid_t *pid)
{
    *pid = fork();
    if (*pid == 0) { execl(bin, bin, NULL); _exit(1); }
}

static void stop_server(pid_t *pid)
{
    if (*pid > 0) { kill(*pid, SIGTERM); waitpid(*pid, NULL, 0); *pid = -1; }
}

static int probe_port(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons((uint16_t)port),
    };
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    close(fd);
    return rc;
}

static void restart_servers(void)
{
    stop_server(&g_imap_pid);
    stop_server(&g_smtp_pid);
    start_server(g_imap_bin, &g_imap_pid);
    start_server(g_smtp_bin, &g_smtp_pid);
    for (int i = 0; i < 30 && probe_port(9993) != 0; i++) usleep(100000);
    for (int i = 0; i < 30 && probe_port(9025) != 0; i++) usleep(100000);
}

/* ── TUI session helpers ─────────────────────────────────────────────── */

static PtySession *tui_run(void)
{
    const char *args[] = { g_tui_bin, NULL };
    PtySession *s = pty_open(COLS, ROWS);
    if (!s) return NULL;
    if (pty_run(s, args) != 0) { pty_close(s); return NULL; }
    return s;
}

static PtySession *tui_open_to_inbox(void)
{
    PtySession *s = tui_run();
    if (!s) return NULL;
    if (pty_wait_for(s, "testuser", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "INBOX", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_key(s, PTY_KEY_HOME);
    for (int i = 0; i < 6; i++) { pty_send_key(s, PTY_KEY_DOWN); pty_settle(s, 50); }
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, "message(s)", WAIT_MS) != 0) { pty_close(s); return NULL; }
    return s;
}

/* Navigate compose dialog to Attach field: Tab×4 from To */
static void nav_to_attach_field(PtySession *s)
{
    pty_send_key(s, PTY_KEY_TAB);   /* To → Cc      */
    pty_send_key(s, PTY_KEY_TAB);   /* Cc → Bcc     */
    pty_send_key(s, PTY_KEY_TAB);   /* Bcc → Subject */
    pty_send_key(s, PTY_KEY_TAB);   /* Subject → Attach */
    pty_settle(s, 100);
}

/* ══════════════════════════════════════════════════════════════════════
 * TC-AT — Attach field in pre-compose dialog (US-84)
 * ══════════════════════════════════════════════════════════════════════ */

/* TC-AT-01: Compose dialog shows an "Attach:" field */
static void test_attach_field_visible(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-01: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Attach:");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-AT-02: Tab from Subject moves focus to Attach field */
static void test_attach_field_tab_from_subject(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-02: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    nav_to_attach_field(s);
    pty_send_str(s, "/tmp");
    pty_settle(s, 200);
    ASSERT_SCREEN_CONTAINS(s, "/tmp");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-AT-03: Typing partial path shows file-browser dropdown */
static void test_attach_dropdown_on_partial_path(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-03: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    nav_to_attach_field(s);
    char prefix[600];
    snprintf(prefix, sizeof(prefix), "%s/attachments/", g_test_home);
    pty_send_str(s, prefix);
    pty_settle(s, 400);
    ASSERT_SCREEN_CONTAINS(s, "report.pdf");
    ASSERT_SCREEN_CONTAINS(s, "notes.txt");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-AT-04: Tab in Attach field completes the first match */
static void test_attach_tab_completion_cycles(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-04: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    nav_to_attach_field(s);
    char prefix[600];
    snprintf(prefix, sizeof(prefix), "%s/attachments/rep", g_test_home);
    pty_send_str(s, prefix);
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_TAB);
    pty_settle(s, 200);
    ASSERT_SCREEN_CONTAINS(s, "report.pdf");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-AT-05: Enter with complete file path adds file to attachment list */
static void test_attach_enter_adds_file(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-05: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    nav_to_attach_field(s);
    pty_send_str(s, g_attach_file1);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "report.pdf");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-AT-06: Non-existent path shows inline error "File not found" */
static void test_attach_nonexistent_shows_error(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-06: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    nav_to_attach_field(s);
    pty_send_str(s, "/tmp/does-not-exist-xyz-404.pdf");
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "File not found");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-AT-07: Attaching a directory path shows inline error "Not a file" */
static void test_attach_directory_shows_error(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-07: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    nav_to_attach_field(s);
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/attachments/", g_test_home);
    pty_send_str(s, dir);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "Not a file");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-AT-08: [d] while Attach field focused removes last attachment */
static void test_attach_d_removes_last(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-08: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    nav_to_attach_field(s);
    pty_send_str(s, g_attach_file1);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, 200);
    ASSERT_SCREEN_CONTAINS(s, "report.pdf");
    pty_send_str(s, "d");
    pty_settle(s, 200);
    ASSERT_SCREEN_NOT_CONTAINS(s, "report.pdf");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-AT-09: Multiple attachments can be added sequentially */
static void test_attach_multiple_files(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-09: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    nav_to_attach_field(s);
    pty_send_str(s, g_attach_file1);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, 200);
    pty_send_str(s, g_attach_file2);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, 200);
    ASSERT_SCREEN_CONTAINS(s, "report.pdf");
    ASSERT_SCREEN_CONTAINS(s, "notes.txt");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-AT-10: Shift-Tab from Attach field moves back to Subject */
static void test_attach_shift_tab_back_to_subject(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-10: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    nav_to_attach_field(s);
    pty_send_str(s, SHIFT_TAB);
    pty_settle(s, 100);
    pty_send_str(s, "ShiftTabTest");
    pty_settle(s, 100);
    ASSERT_SCREEN_CONTAINS(s, "ShiftTabTest");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-AT-11: Confirming dialog with attachment shows it in the review screen */
static void test_attach_shown_in_review(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-AT-11: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "Test with attachment");
    nav_to_attach_field(s);
    pty_send_str(s, g_attach_file1);
    pty_send_key(s, PTY_KEY_ENTER);
    pty_settle(s, 200);
    /* Confirm: opens editor (fake), then review screen */
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Review", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "report.pdf");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 * TC-PCR — Post-compose review screen (US-85)
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * Helper: compose, fill To+Subject, confirm dialog → editor (fake) → review.
 * Returns open session on review screen, or NULL on failure.
 */
static PtySession *compose_to_review(const char *to, const char *subject)
{
    PtySession *s = tui_open_to_inbox();
    if (!s) return NULL;
    pty_send_str(s, "c");
    if (pty_wait_for(s, "New Message", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_str(s, to);
    pty_send_key(s, PTY_KEY_TAB);   /* To → Cc      */
    pty_send_key(s, PTY_KEY_TAB);   /* Cc → Bcc     */
    pty_send_key(s, PTY_KEY_TAB);   /* Bcc → Subject */
    pty_send_str(s, subject);
    pty_send_key(s, PTY_KEY_ENTER); /* confirm → editor → review */
    if (pty_wait_for(s, "Review", WAIT_MS) != 0) { pty_close(s); return NULL; }
    return s;
}

/* TC-PCR-01: Review screen appears after editor exits (no plain "Send? [y/n]") */
static void test_review_screen_appears(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "Review screen test");
    ASSERT(s != NULL, "TC-PCR-01: review screen reached");
    ASSERT_SCREEN_CONTAINS(s, "Review");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Send? [y/n]");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-02: Review screen shows From address */
static void test_review_shows_from(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "From header test");
    ASSERT(s != NULL, "TC-PCR-02: review screen reached");
    ASSERT_SCREEN_CONTAINS(s, "testuser@example.com");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-03: Review screen shows To address */
static void test_review_shows_to(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "To header test");
    ASSERT(s != NULL, "TC-PCR-03: review screen reached");
    ASSERT_SCREEN_CONTAINS(s, "alice@example.com");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-04: Review screen shows Subject */
static void test_review_shows_subject(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "My unique subject");
    ASSERT(s != NULL, "TC-PCR-04: review screen reached");
    ASSERT_SCREEN_CONTAINS(s, "My unique subject");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-05: Review screen shows body line count */
static void test_review_shows_body_lines(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "Body count test");
    ASSERT(s != NULL, "TC-PCR-05: review screen reached");
    /* Fake editor appended one line → "1 line" or "1 lines" */
    ASSERT_SCREEN_CONTAINS(s, "line");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-06: Review screen shows "(none)" when no attachments */
static void test_review_no_attachments_shown(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "No attach test");
    ASSERT(s != NULL, "TC-PCR-06: review screen reached");
    ASSERT_SCREEN_CONTAINS(s, "(none)");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-07: [s] sends the message and shows confirmation */
static void test_review_s_sends_message(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "Send via review");
    ASSERT(s != NULL, "TC-PCR-07: review screen reached");
    pty_send_str(s, "s");
    ASSERT_WAIT_FOR(s, "sent", WAIT_MS);
    pty_close(s);
}

/* TC-PCR-08: [e] reopens header dialog pre-filled with current values */
static void test_review_e_reopens_header_dialog(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "Edit headers test");
    ASSERT(s != NULL, "TC-PCR-08: review screen reached");
    pty_send_str(s, "e");
    ASSERT_WAIT_FOR(s, "Message", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "alice@example.com");
    ASSERT_SCREEN_CONTAINS(s, "Edit headers test");
    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* TC-PCR-09: [b] reopens editor; review reappears with updated line count */
static void test_review_b_reopens_editor(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "Edit body test");
    ASSERT(s != NULL, "TC-PCR-09: review screen reached");
    pty_send_str(s, "b");
    /* Fake editor appends another "Test body" line → 2 lines total */
    ASSERT_WAIT_FOR(s, "Review", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "2");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-10: [a] opens attachment picker; picked file appears in review */
static void test_review_a_opens_attach_picker(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "Attach from review");
    ASSERT(s != NULL, "TC-PCR-10: review screen reached");
    pty_send_str(s, "a");
    pty_settle(s, 200);
    /* Picker overlay visible */
    ASSERT_SCREEN_CONTAINS(s, "Attach");
    pty_send_str(s, g_attach_file2);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Review", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "notes.txt");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-11: [d] removes last attachment; "(none)" shown when list empty */
static void test_review_d_removes_attachment(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "Remove attach test");
    ASSERT(s != NULL, "TC-PCR-11: review reached");
    /* Add via [a] */
    pty_send_str(s, "a");
    pty_settle(s, 200);
    pty_send_str(s, g_attach_file1);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Review", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "report.pdf");
    /* Remove via [d] */
    pty_send_str(s, "d");
    pty_settle(s, 200);
    ASSERT_SCREEN_NOT_CONTAINS(s, "report.pdf");
    ASSERT_SCREEN_CONTAINS(s, "(none)");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-12: [q] asks discard confirmation; [n] returns to review */
static void test_review_q_asks_discard_confirm(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "Cancel test");
    ASSERT(s != NULL, "TC-PCR-12: review reached");
    pty_send_str(s, "q");
    pty_settle(s, 200);
    ASSERT_SCREEN_CONTAINS(s, "Discard");
    pty_send_str(s, "n");
    pty_settle(s, 200);
    ASSERT_SCREEN_CONTAINS(s, "Review");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-13: [q]+[y] discards and returns to inbox */
static void test_review_q_y_cancels(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "Discard confirm");
    ASSERT(s != NULL, "TC-PCR-13: review reached");
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "message(s)", WAIT_MS);
    pty_close(s);
}

/* TC-PCR-14: Esc triggers discard confirmation like [q] */
static void test_review_esc_asks_discard_confirm(void)
{
    restart_servers();
    PtySession *s = compose_to_review("alice@example.com", "Esc cancel test");
    ASSERT(s != NULL, "TC-PCR-14: review reached");
    pty_send_key(s, PTY_KEY_ESC);
    pty_settle(s, 200);
    ASSERT_SCREEN_CONTAINS(s, "Discard");
    pty_send_str(s, "y");
    pty_close(s);
}

/* TC-PCR-15: Empty To in draft disables [s] and shows error */
static void test_review_empty_to_disables_send(void)
{
    /* Use a custom editor that strips the To: line from the draft */
    restart_servers();
    char strip_editor[700];
    snprintf(strip_editor, sizeof(strip_editor),
             "%s/.strip_to_editor.sh", g_test_home);
    FILE *ef = fopen(strip_editor, "w");
    if (ef) {
        fprintf(ef, "#!/bin/sh\n"
                    "grep -v '^To:' \"$1\" > \"$1.tmp\"\n"
                    "mv \"$1.tmp\" \"$1\"\n"
                    "echo 'body' >> \"$1\"\n");
        fclose(ef);
        chmod(strip_editor, 0755);
    }
    setenv("EDITOR", strip_editor, 1);

    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-PCR-15: inbox opens");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "No-To test");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Review", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "To:");
    /* [s] should not send; error about empty To expected */
    pty_send_str(s, "s");
    pty_settle(s, 300);
    ASSERT_SCREEN_CONTAINS(s, "Review");
    pty_send_str(s, "q");
    pty_send_str(s, "y");

    setenv("EDITOR", g_editor_script, 1);
    pty_close(s);
}

/* TC-PCR-16: Reply flow also uses the review screen (not plain y/n) */
static void test_review_reply_uses_review_screen(void)
{
    restart_servers();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "TC-PCR-16: inbox opens");
    pty_send_key(s, PTY_KEY_HOME);
    pty_send_key(s, PTY_KEY_DOWN);
    pty_settle(s, 100);
    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Review", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Review");
    ASSERT_SCREEN_NOT_CONTAINS(s, "Send? [y/n]");
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <email-tui> <mock-imap-server> <mock-smtp-server>\n",
            argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(g_tui_bin,  sizeof(g_tui_bin),  "%s", argv[1]);
    snprintf(g_imap_bin, sizeof(g_imap_bin), "%s", argv[2]);
    snprintf(g_smtp_bin, sizeof(g_smtp_bin), "%s", argv[3]);

    snprintf(g_test_home, sizeof(g_test_home),
             "/tmp/email-pty-attach-%d", (int)getpid());
    mkdir(g_test_home, 0700);

    if (getenv("HOME"))
        snprintf(g_old_home, sizeof(g_old_home), "%s", getenv("HOME"));
    setenv("HOME", g_test_home, 1);
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");

    write_config();
    write_attach_files();
    write_contacts_tsv();
    write_editor_script();

    /* ── TC-AT: Attach field ─────────────────────────────────────────── */
    printf("\n--- TC-AT: Attachment field in compose dialog (US-84) ---\n");
    RUN_TEST(test_attach_field_visible);
    RUN_TEST(test_attach_field_tab_from_subject);
    RUN_TEST(test_attach_dropdown_on_partial_path);
    RUN_TEST(test_attach_tab_completion_cycles);
    RUN_TEST(test_attach_enter_adds_file);
    RUN_TEST(test_attach_nonexistent_shows_error);
    RUN_TEST(test_attach_directory_shows_error);
    RUN_TEST(test_attach_d_removes_last);
    RUN_TEST(test_attach_multiple_files);
    RUN_TEST(test_attach_shift_tab_back_to_subject);
    RUN_TEST(test_attach_shown_in_review);

    /* ── TC-PCR: Post-compose review screen ─────────────────────────── */
    printf("\n--- TC-PCR: Post-compose review screen (US-85) ---\n");
    RUN_TEST(test_review_screen_appears);
    RUN_TEST(test_review_shows_from);
    RUN_TEST(test_review_shows_to);
    RUN_TEST(test_review_shows_subject);
    RUN_TEST(test_review_shows_body_lines);
    RUN_TEST(test_review_no_attachments_shown);
    RUN_TEST(test_review_s_sends_message);
    RUN_TEST(test_review_e_reopens_header_dialog);
    RUN_TEST(test_review_b_reopens_editor);
    RUN_TEST(test_review_a_opens_attach_picker);
    RUN_TEST(test_review_d_removes_attachment);
    RUN_TEST(test_review_q_asks_discard_confirm);
    RUN_TEST(test_review_q_y_cancels);
    RUN_TEST(test_review_esc_asks_discard_confirm);
    RUN_TEST(test_review_empty_to_disables_send);
    RUN_TEST(test_review_reply_uses_review_screen);

    /* ── Cleanup ─────────────────────────────────────────────────────── */
    stop_server(&g_imap_pid);
    stop_server(&g_smtp_pid);
    if (g_old_home[0]) setenv("HOME", g_old_home, 1);

    printf("\n--- PTY Attachment Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);
    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
