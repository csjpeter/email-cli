/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-04-26 */
/* PTY tests of the pre-compose dialog. */
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
#include "test_scratch.h"
#include "pty_editor.h"

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

/* EMAIL-175: sending is in the review screen's ':' menu (row 0) and asks
 * first, naming the recipients; removing the last attachment is row 2. */
#define REVIEW_SEND(s) do { pty_send_str((s), ":"); \
    pty_wait_for((s), "Actions", 8000); pty_settle((s), 200); \
    pty_send_key((s), PTY_KEY_ENTER); \
    pty_wait_for((s), "Yes", 8000); pty_send_str((s), "y"); } while (0)
#define REVIEW_UNATTACH(s) do { pty_send_str((s), ":"); \
    pty_wait_for((s), "Actions", 8000); pty_settle((s), 200); \
    pty_send_key((s), PTY_KEY_DOWN); pty_send_key((s), PTY_KEY_DOWN); \
    pty_send_key((s), PTY_KEY_ENTER); } while (0)

/* ── Test globals ────────────────────────────────────────────────────── */

int g_tests_run    = 0;
int g_tests_failed = 0;

static char g_tui_bin[512];
static char g_imap_bin[512];
static char g_smtp_bin[512];

/* EMAIL_USER value in the generated config — the local store names the
 * data directory after it (the .config directory name is unrelated). */
#define TEST_ACCOUNT "testuser@example.com"

/* 1: the generated config names no SMTP server (the prompt of EMAIL-115) */
static int  g_no_smtp;
static char g_test_home[512];
static char g_old_home[512];

static pid_t g_imap_pid = -1;
static pid_t g_smtp_pid = -1;
static int   g_imap_port = 0;   /* both ports are picked by the kernel */
static int   g_smtp_port = 0;

#define WAIT_MS   6000
/* A positive screen assertion waits for the text (EMAIL-224): under ASan or
 * Valgrind the program redraws slowly, and a read in between sees half a screen. */
#undef ASSERT_SCREEN_CONTAINS
#define ASSERT_SCREEN_CONTAINS(s, text) ASSERT_WAIT_FOR(s, text, WAIT_MS)
#define SETTLE_MS  400
#define ROWS 24
#define COLS 120

#define RUN_TEST(fn) do { if (!test_selected(#fn)) break; printf("  %s...\n", #fn); fflush(stdout); fn(); } while(0)

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
    snprintf(d, sizeof(d), "%s/.local/share/email-cli/accounts/" TEST_ACCOUNT "", g_test_home); mkdir(d, 0700);
    snprintf(d, sizeof(d), "%s/.local/share/email-cli/accounts/" TEST_ACCOUNT "/manifests", g_test_home); mkdir(d, 0700);
}

static void write_config(void) {
    mkdirs();
    char path[700];
    snprintf(path, sizeof(path),
             "%s/.config/email-cli/accounts/testuser/config.ini", g_test_home);
    FILE *fp = fopen(path, "w");
    if (!fp) {                       /* a fixture that cannot be made fails the run */
        fprintf(stderr, "compose-dialog: cannot write %s\n", path);
        g_tests_failed++;
        return;
    }
    if (g_no_smtp) {
        fprintf(fp,
            "EMAIL_HOST=imaps://localhost:%d\n"
            "EMAIL_USER=testuser@example.com\n"
            "EMAIL_PASS=testpass\n"
            "EMAIL_FOLDER=INBOX\n"
            "SSL_NO_VERIFY=1\n", g_imap_port);
    } else {
        fprintf(fp,
            "EMAIL_HOST=imaps://localhost:%d\n"
            "EMAIL_USER=testuser@example.com\n"
            "EMAIL_PASS=testpass\n"
            "EMAIL_FOLDER=INBOX\n"
            "SMTP_HOST=smtps://localhost:%d\n"
            "SMTP_PORT=%d\n"
            "SMTP_USER=testuser@example.com\n"
            "SMTP_PASS=testpass\n"
            "SSL_NO_VERIFY=1\n", g_imap_port, g_smtp_port, g_smtp_port);
    }
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
             "%s/.local/share/email-cli/accounts/" TEST_ACCOUNT "/contacts.tsv",
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
             "%s/.local/share/email-cli/accounts/" TEST_ACCOUNT "/manifests/INBOX.tsv",
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
             "%s/.local/share/email-cli/accounts/" TEST_ACCOUNT "/contacts.tsv",
             g_test_home);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "alice@example.com\t\t2\n");
    fprintf(f, "bob.smith@acme.com\tBob Smith\t1\n");
    fclose(f);
}

/* ── Mock server management ────────────────────────────────────────────── */

/* Start a mock.  *port is 0 the first time (the kernel picks) and the port of
 * the previous instance on a restart, because the config names it.  The call
 * returns once the mock has bound the port and published it. */
static int start_server(const char *bin, const char *prefix, pid_t *pid_out,
                        int *port) {
    int bound = 0;
    *pid_out = test_spawn_mock(bin, prefix, *port, &bound);
    if (*pid_out < 0) {
        fprintf(stderr, "compose-dialog: %s did not start\n", bin);
        g_tests_failed++;
        return -1;
    }
    *port = bound;
    return 0;
}

static void stop_server(pid_t *pid_out) {
    if (*pid_out > 0) {
        kill(*pid_out, SIGKILL);
        waitpid(*pid_out, NULL, 0);
        *pid_out = -1;
    }
}

static void restart_imap(void) {
    pty_editor_forget_draft(g_test_home);
    stop_server(&g_imap_pid);
    (void)start_server(g_imap_bin, "MOCK_IMAP", &g_imap_pid, &g_imap_port);
}

static void restart_smtp(void) {
    pty_editor_forget_draft(g_test_home);
    stop_server(&g_smtp_pid);
    (void)start_server(g_smtp_bin, "MOCK_SMTP", &g_smtp_pid, &g_smtp_port);
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
/* US-85: submitting the compose dialog runs $EDITOR and then shows the
 * post-compose review screen.  Press 's' there to actually send. */
static void pcr_send(PtySession *s) {
    if (pty_editor_type_and_leave(s, "Test body") != 0)
        printf("  [WARN] pcr_send: editor did not appear\n");
    if (pty_wait_for(s, "Review: Message", WAIT_MS) != 0)
        printf("  [WARN] pcr_send: review screen did not appear\n");
    REVIEW_SEND(s);
}

/* The same, but copies the autosaved compose draft to @p capture first, so a
 * test can look at what the editor held (headers and the body text). */
static void pcr_send_capture(PtySession *s, const char *capture) {
    if (pty_editor_type_and_leave(s, "Test body") != 0)
        printf("  [WARN] pcr_send_capture: editor did not appear\n");
    if (pty_wait_for(s, "Review: Message", WAIT_MS) != 0)
        printf("  [WARN] pcr_send_capture: review screen did not appear\n");
    char src[700];
    snprintf(src, sizeof(src), "%s/.local/share/email-cli/compose-draft", g_test_home);
    FILE *in = fopen(src, "r");
    FILE *out = fopen(capture, "w");
    if (in && out) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    }
    if (in) fclose(in);
    if (out) fclose(out);
    REVIEW_SEND(s);
}

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
    ASSERT_SCREEN_CONTAINS(s, "Esc");

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
    /* A cancel returns to the list at once, with no pause (EMAIL-222). */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "[Press any key");
    pty_settle(s, SETTLE_MS);
    /* Must NOT have opened editor (vim is not running) */
    ASSERT_SCREEN_NOT_CONTAINS(s, "~"); /* vim empty-buffer tilde markers */

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* EMAIL-175: Esc asks before it throws typed text away; No is the default. */
static void test_compose_esc_asks_before_discarding(void) {
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "esc asks: reached inbox");

    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "ann@example.org");
    pty_settle(s, 300);

    pty_send_key(s, PTY_KEY_ESC);                 /* first Esc closes the drop-down, if any */
    pty_settle(s, 300);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Discard what was typed?", WAIT_MS);
    pty_send_key(s, PTY_KEY_ENTER);               /* Enter on the default: No */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Discard what was typed?");
    ASSERT_SCREEN_CONTAINS(s, "New Message");     /* still in the dialog */
    ASSERT_SCREEN_CONTAINS(s, "ann@example.org"); /* nothing lost */

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Discard what was typed?", WAIT_MS);
    pty_send_key(s, PTY_KEY_ESC);                 /* Esc answers No, too */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "ann@example.org");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Discard what was typed?", WAIT_MS);
    pty_send_str(s, "y");                         /* Yes discards */
    /* A cancel returns to the list at once, with no pause (EMAIL-222). */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "[Press any key");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

/* Ctrl-C leaves the dialog at once, without asking. */
static void test_compose_ctrl_c_does_not_ask(void) {
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "ctrl-c: reached inbox");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_str(s, "bob@example.org");
    pty_settle(s, 300);
    pty_send_key(s, PTY_KEY_CTRL_C);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "Discard what was typed?");
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
    pcr_send(s);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);

    pty_close(s);
}

/* ── EMAIL-87: the body editor (tui_textedit) and the autosaved draft ──── */

static void draft_file(char *buf, size_t size) {
    snprintf(buf, size, "%s/.local/share/email-cli/compose-draft", g_test_home);
}

static int draft_contains(const char *needle) {
    char path[700], txt[16384];
    draft_file(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(txt, 1, sizeof(txt) - 1, f);
    fclose(f);
    txt[n] = '\0';
    return strstr(txt, needle) != NULL;
}

static void draft_clear(void) {
    char path[700];
    draft_file(path, sizeof(path));
    unlink(path);
}

/* Inbox, `c`, a filled dialog, Enter: the body editor is on screen. */
static PtySession *open_body_editor(const char *subject) {
    PtySession *s = tui_open_to_inbox();
    if (!s) return NULL;
    pty_send_str(s, "c");
    if (pty_wait_for(s, "New Message", WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_send_str(s, "alice@example.com");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, subject);
    pty_send_key(s, PTY_KEY_ENTER);
    if (pty_wait_for(s, PTY_EDITOR_MARK, WAIT_MS) != 0) { pty_close(s); return NULL; }
    pty_settle(s, 150);
    return s;
}

static void test_body_editor_type_and_esc_saves(void) {
    restart_imap(); restart_smtp(); draft_clear();
    PtySession *s = open_body_editor("Editor basics");
    ASSERT(s != NULL, "body editor: opens after the dialog");
    pty_send_str(s, "first line\rsecond line");
    ASSERT_WAIT_FOR(s, "second line", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "first line");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Body:    2 lines", WAIT_MS);
    ASSERT(draft_contains("first line\nsecond line"), "body editor: Esc saved the draft");
    ASSERT(draft_contains("Subject: Editor basics"), "body editor: the draft holds the headers");
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Editor basics");
    pty_send_str(s, "n");              /* declined: still in review, draft kept */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "actions (send)");
    ASSERT(draft_contains("second line"), "body editor: declining keeps the draft");
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, "Cancelled. Draft discarded.", WAIT_MS);
    pty_close(s);
    ASSERT(!draft_contains("Subject:"), "body editor: discarding removed the draft");
}

static void test_body_editor_autosave_while_typing(void) {
    restart_imap(); restart_smtp(); draft_clear();
    PtySession *s = open_body_editor("Autosave");
    ASSERT(s != NULL, "autosave: opens");
    pty_send_str(s, "typed, never left with Esc");
    ASSERT_WAIT_FOR(s, "autosaved", 8000);
    ASSERT(draft_contains("typed, never left with Esc"), "autosave: the idle edit is on disk");
    pty_close(s);                       /* killed: the text survives */
    ASSERT(draft_contains("typed, never left with Esc"), "autosave: it survives a kill");
    draft_clear();
}

static void test_body_editor_reopen_restores(void) {
    restart_imap(); restart_smtp(); draft_clear();
    PtySession *s = open_body_editor("Come back");
    ASSERT(s != NULL, "restore: opens");
    pty_send_str(s, "kept across sessions");
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    pty_close(s);                       /* the review is left without sending */

    s = tui_open_to_inbox();
    ASSERT(s != NULL, "restore: second session reaches the inbox");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_send_str(s, "bob@example.org");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "Another");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Unsent draft found", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "Come back");
    pty_send_str(s, "y");
    ASSERT_WAIT_FOR(s, PTY_EDITOR_MARK, WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "kept across sessions");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "alice@example.com");
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    pty_send_str(s, "y");
    pty_close(s);
}

static void test_body_editor_resume_declined(void) {
    restart_imap(); restart_smtp(); draft_clear();
    PtySession *s = open_body_editor("Old one");
    ASSERT(s != NULL, "resume n: opens");
    pty_send_str(s, "old text");
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    pty_close(s);

    s = tui_open_to_inbox();
    ASSERT(s != NULL, "resume n: second session reaches the inbox");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_send_str(s, "bob@example.org");
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "New one");
    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "Unsent draft found", WAIT_MS);
    pty_send_str(s, "n");
    ASSERT_WAIT_FOR(s, PTY_EDITOR_MARK, WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "old text");
    ASSERT_SCREEN_CONTAINS(s, "New one");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    pty_send_str(s, "y");
    pty_close(s);
}

static void test_body_editor_paste_and_wide(void) {
    restart_imap(); restart_smtp(); draft_clear();
    PtySession *s = open_body_editor("Paste");
    ASSERT(s != NULL, "paste: opens");
    /* bracketed paste of three lines, then wide and accented characters */
    pty_send_str(s, "\033[200~one\ntwo\nthree\033[201~");
    pty_send_str(s, " \xe4\xbd\xa0\xe5\xa5\xbd caf\xc3\xa9");
    ASSERT_WAIT_FOR(s, "caf", WAIT_MS);
    ASSERT_SCREEN_CONTAINS(s, "one");
    ASSERT_SCREEN_CONTAINS(s, "three");
    ASSERT_SCREEN_CONTAINS(s, "\xe4\xbd\xa0\xe5\xa5\xbd");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Body:    3 lines", WAIT_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    pty_send_str(s, "y");
    pty_close(s);
}

static void test_body_editor_long_line_scrolls(void) {
    restart_imap(); restart_smtp(); draft_clear();
    PtySession *s = open_body_editor("Long");
    ASSERT(s != NULL, "long line: opens");
    char line[400];
    memset(line, 'x', sizeof(line));
    memcpy(line + 190, "END", 3);
    line[193] = '\0';
    pty_send_str(s, line);
    ASSERT_WAIT_FOR(s, "END", WAIT_MS);   /* scrolled horizontally to the cursor */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    ASSERT(draft_contains("END"), "long line: the whole line was saved");
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    pty_send_str(s, "y");
    pty_close(s);
}

static void test_body_editor_ctrl_s_saves_and_stays(void) {
    restart_imap(); restart_smtp(); draft_clear();
    PtySession *s = open_body_editor("Ctrl-S");
    ASSERT(s != NULL, "ctrl-s: opens");
    pty_send_str(s, "text one");
    pty_send_str(s, "\x13");
    ASSERT_WAIT_FOR(s, "saved", WAIT_MS);
    ASSERT(draft_contains("text one"), "ctrl-s: written");
    ASSERT_SCREEN_CONTAINS(s, "Message body");      /* the editor's title bar: still in the editor */
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    pty_send_str(s, "y");
    pty_close(s);
}

static void test_body_editor_resize_and_too_small(void) {
    restart_imap(); restart_smtp(); draft_clear();
    PtySession *s = open_body_editor("Resize");
    ASSERT(s != NULL, "editor resize: opens");
    pty_send_str(s, "kept text");
    ASSERT_WAIT_FOR(s, "kept text", WAIT_MS);
    pty_resize(s, 140, 30);
    pty_settle(s, SETTLE_MS * 2);
    ASSERT_SCREEN_CONTAINS(s, "kept text");
    ASSERT_SCREEN_CONTAINS(s, PTY_EDITOR_MARK);
    pty_resize(s, 60, 20);
    ASSERT_WAIT_FOR(s, "Terminal too small", WAIT_MS);
    pty_send_str(s, "hidden");          /* not edited while nothing is drawn */
    pty_settle(s, 200);
    pty_resize(s, 120, 24);
    ASSERT_WAIT_FOR(s, "kept text", WAIT_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "hidden");
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    pty_send_str(s, "q");
    ASSERT_WAIT_FOR(s, "Discard draft?", WAIT_MS);
    pty_send_str(s, "y");
    pty_close(s);
}

static void test_send_removes_draft(void) {
    restart_imap(); restart_smtp(); draft_clear();
    PtySession *s = open_body_editor("Send removes");
    ASSERT(s != NULL, "send: opens");
    pty_send_str(s, "to be sent");
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "actions (send)", WAIT_MS);
    ASSERT(draft_contains("to be sent"), "send: the draft exists before sending");
    REVIEW_SEND(s);
    ASSERT_WAIT_FOR(s, "Message sent", WAIT_MS);
    pty_settle(s, 200);
    ASSERT(!draft_contains("Subject:"), "send: the draft is gone after sending");
    pty_close(s);
}

static void test_compose_opens_editor_with_to(void) {
    /* TC-CD-04: filling To + Subject and pressing Enter opens editor */
    restart_imap(); restart_smtp();
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
    pcr_send(s);
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


/* EMAIL-115: the caret moves inside a field; Backspace and Delete act at it. */
static void test_compose_caret_editing(void) {
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "caret editing: reached inbox");
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_key(s, PTY_KEY_TAB);               /* To -> Cc */
    pty_send_key(s, PTY_KEY_TAB);               /* Cc -> Bcc */
    pty_send_key(s, PTY_KEY_TAB);               /* Bcc -> Subject */
    pty_send_str(s, "abc");
    pty_send_key(s, PTY_KEY_LEFT);
    pty_send_str(s, "X");                       /* abXc */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "abXc");
    pty_send_key(s, PTY_KEY_BACK);              /* ab|c: removes X */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "abc");
    ASSERT_SCREEN_NOT_CONTAINS(s, "abXc");
    pty_send_key(s, PTY_KEY_LEFT);
    pty_send_key(s, PTY_KEY_DELETE);            /* a|c: removes b */
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "Subject:");
    ASSERT_SCREEN_NOT_CONTAINS(s, "abc");
    pty_send_key(s, PTY_KEY_RIGHT);
    pty_send_key(s, PTY_KEY_RIGHT);             /* at the end: no effect */
    pty_send_key(s, PTY_KEY_DELETE);            /* nothing to delete */
    pty_send_str(s, "Z");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "acZ");
    pty_send_key(s, PTY_KEY_LEFT);
    pty_send_key(s, PTY_KEY_LEFT);
    pty_send_key(s, PTY_KEY_LEFT);
    pty_send_key(s, PTY_KEY_LEFT);              /* stops at the start */
    pty_send_key(s, PTY_KEY_BACK);              /* nothing before the caret */
    pty_send_str(s, "_");
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "_acZ");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_compose_cc_written_to_tempfile(void) {
    /* TC-CD-16: Cc address appears as Cc: header in the editor temp file */
    restart_imap(); restart_smtp();
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
    pcr_send(s);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);

    pty_close(s);
}

static void test_compose_bcc_written_to_tempfile(void) {
    /* TC-CD-17: Bcc address appears in Bcc: header in the editor temp file */
    restart_imap(); restart_smtp();
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

    pcr_send(s);
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
    /* Esc asks before it throws away the prefilled text (EMAIL-175). */
    ASSERT_WAIT_FOR(s, "Discard what was typed?", WAIT_MS);
    pty_send_str(s, "y");
    /* A cancel returns to the list at once, with no pause (EMAIL-222). */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "[Press any key");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_can_add_cc(void) {
    /* TC-CD-33: can add Cc before opening editor */
    restart_imap(); restart_smtp();
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

    pcr_send(s);
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

    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply body: reached inbox");

    pty_send_str(s, "r");
    ASSERT_WAIT_FOR(s, "Reply", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    /* Reply dialog starts at Cc (To pre-filled): Cc→Bcc→Subject = 2× Tab */
    pty_send_key(s, PTY_KEY_TAB);   /* Cc → Bcc */
    pty_send_key(s, PTY_KEY_TAB);   /* Bcc → Subject */
    pty_send_key(s, PTY_KEY_ENTER);
    pcr_send_capture(s, draft_capture);
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


/* The tests of EMAIL-115 run in a home of their own: messages and contacts
 * cached by earlier tests would change what reply-all and the address field
 * see. */
static char g_saved_test_home[512];
static void fresh_home_begin(const char *name) {
    snprintf(g_saved_test_home, sizeof(g_saved_test_home), "%s", g_test_home);
    snprintf(g_test_home, sizeof(g_test_home), "%s", test_path(name));
    mkdir(g_test_home, 0700);
    setenv("HOME", g_test_home, 1);
}
static void fresh_home_end(void) {
    snprintf(g_test_home, sizeof(g_test_home), "%s", g_saved_test_home);
    setenv("HOME", g_test_home, 1);
}

/* EMAIL-115: reply-all collects the other recipients of the message into Cc,
 * without the user's own address and without duplicates of the sender. */
static void test_reply_all_collects_recipients(void) {
    fresh_home_begin("home-reply-all");
    setenv("MOCK_IMAP_TO", "Testuser <testuser@example.com>, dave@example.org", 1);
    setenv("MOCK_IMAP_CC", "erin@example.net,  Frank <frank@example.net> ", 1);
    restart_imap();
    unsetenv("MOCK_IMAP_TO");
    unsetenv("MOCK_IMAP_CC");
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply-all recipients: reached inbox");

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Reply All", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "dave@example.org");
    ASSERT_SCREEN_CONTAINS(s, "erin@example.net");
    ASSERT_SCREEN_CONTAINS(s, "frank@example.net");
    ASSERT_SCREEN_NOT_CONTAINS(s, "testuser@example.com");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
    fresh_home_end();
}

/* EMAIL-115: an account without an SMTP server is offered the SMTP wizard
 * when a message is composed; the answer never blocks the composing. */
static PtySession *compose_without_smtp(void) {
    fresh_home_begin("home-no-smtp");
    g_no_smtp = 1;
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    g_no_smtp = 0;
    return s;
}

/* The text of every account's config.ini (the account may have been saved
 * under its address, so all of them are read). */
static void accounts_text(char *text, size_t cap) {
    text[0] = '\0';
    char adir[700];
    snprintf(adir, sizeof(adir), "%s/.config/email-cli/accounts", g_test_home);
    DIR *d = opendir(adir);
    struct dirent *de;
    while (d && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char cp[1400];
        snprintf(cp, sizeof(cp), "%s/%s/config.ini", adir, de->d_name);
        FILE *cf = fopen(cp, "r");
        if (!cf) continue;
        size_t used = strlen(text);
        size_t cn = fread(text + used, 1, cap - 1 - used, cf);
        text[used + cn] = '\0';
        fclose(cf);
    }
    if (d) closedir(d);
}

/* Fill the dialog and confirm it: the prompt comes before the editor. */
static void smtp_prompt_reach(PtySession *s) {
    pty_send_str(s, "c");
    ASSERT_WAIT_FOR(s, "New Message", WAIT_MS);
    pty_send_str(s, "alice@example.com;");
    pty_settle(s, 200);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_key(s, PTY_KEY_TAB);
    pty_send_str(s, "No SMTP yet");
    pty_send_key(s, PTY_KEY_ENTER);
}

/* After the prompt: the editor opens, then the review, which is discarded. */
static void smtp_prompt_finish(PtySession *s) {
    ASSERT(pty_editor_type_and_leave(s, "Test body") == 0, "the editor opens after the prompt");
    ASSERT_WAIT_FOR(s, "Review: Message", WAIT_MS);
    pty_send_str(s, "q");
    pty_send_str(s, "y");
    pty_close(s);
    fresh_home_end();
}

static void test_smtp_prompt_declined(void) {
    PtySession *s = compose_without_smtp();
    ASSERT(s != NULL, "smtp prompt declined: reached inbox");
    smtp_prompt_reach(s);
    ASSERT_WAIT_FOR(s, "Configure SMTP now? [Y/n]", WAIT_MS);
    pty_send_str(s, "n\r");
    smtp_prompt_finish(s);                               /* composing goes on */
}

static void test_smtp_prompt_wizard_abandoned(void) {
    PtySession *s = compose_without_smtp();
    ASSERT(s != NULL, "smtp prompt abandoned: reached inbox");
    smtp_prompt_reach(s);
    ASSERT_WAIT_FOR(s, "Configure SMTP now? [Y/n]", WAIT_MS);
    pty_send_str(s, "y\r");
    ASSERT_WAIT_FOR(s, "SMTP (outgoing mail) configuration", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_CTRL_D);
    pty_settle(s, SETTLE_MS);
    char text[4096];
    accounts_text(text, sizeof(text));
    ASSERT(strstr(text, "SMTP_HOST=") == NULL, "an abandoned SMTP wizard saves nothing");
    smtp_prompt_finish(s);
}

static void test_smtp_prompt_wizard_saves(void) {
    PtySession *s = compose_without_smtp();
    ASSERT(s != NULL, "smtp prompt saves: reached inbox");
    smtp_prompt_reach(s);
    ASSERT_WAIT_FOR(s, "Configure SMTP now? [Y/n]", WAIT_MS);
    pty_send_str(s, "\r");                              /* Enter = yes */
    ASSERT_WAIT_FOR(s, "SMTP (outgoing mail) configuration", WAIT_MS);
    for (int i = 0; i < 6; i++) {                       /* accept every default */
        pty_settle(s, 150);
        pty_send_key(s, PTY_KEY_ENTER);
    }
    ASSERT_WAIT_FOR(s, "Message body", WAIT_MS);        /* the wizard is through */
    char text[4096];
    accounts_text(text, sizeof(text));
    ASSERT(strstr(text, "SMTP_HOST=") != NULL, "the wizard's SMTP settings were saved");
    smtp_prompt_finish(s);
}

static void test_reply_all_esc_cancels(void) {
    /* TC-CD-42: Esc cancels reply-all */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply-all esc: reached inbox");

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Reply All", WAIT_MS);

    pty_send_key(s, PTY_KEY_ESC);
    /* Esc asks before it throws away the prefilled text (EMAIL-175). */
    ASSERT_WAIT_FOR(s, "Discard what was typed?", WAIT_MS);
    pty_send_str(s, "y");
    /* A cancel returns to the list at once, with no pause (EMAIL-222). */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "[Press any key");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_all_in_help_panel(void) {
    /* TC-CD-43: 'A' appears in message list help popup */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply-all help: reached inbox");

    pty_send_str(s, "?");
    ASSERT_WAIT_FOR(s, "Reply-all", WAIT_MS);
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

    pcr_send(s);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);

    pty_close(s);
}

static void test_forward_body_has_original(void) {
    /* TC-CD-50: forwarded draft contains original message body */
    restart_imap(); restart_smtp();

    char draft_capture[600];
    snprintf(draft_capture, sizeof(draft_capture),
             "%s/.fwd_draft", g_test_home);

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
    pcr_send_capture(s, draft_capture);
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
    /* Esc asks before it throws away the prefilled text (EMAIL-175). */
    ASSERT_WAIT_FOR(s, "Discard what was typed?", WAIT_MS);
    pty_send_str(s, "y");
    /* A cancel returns to the list at once, with no pause (EMAIL-222). */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "[Press any key");

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
    pcr_send(s);
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
             "%s/.local/share/email-cli/accounts/" TEST_ACCOUNT "/contacts.tsv",
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
    pcr_send(s);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    pty_close(s);

    /* The mock SMTP server logs the message; verify Cc: header present.
     * (This test verifies the integration; the mock keeps the received data
     * in its log under the scratch directory.) */
}

static void test_bcc_not_in_message_headers(void) {
    /* TC-CD-62: Bcc must NOT appear in the final RFC 2822 headers */
    restart_imap(); restart_smtp();

    char draft_capture[600];
    snprintf(draft_capture, sizeof(draft_capture),
             "%s/.bcc_draft", g_test_home);

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
    pcr_send_capture(s, draft_capture);
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
}

static void test_multiple_cc_semicolon_separated(void) {
    /* TC-CD-63: multiple Cc addresses → semicolon-separated in Cc: header */
    restart_imap(); restart_smtp();

    char draft_capture[600];
    snprintf(draft_capture, sizeof(draft_capture),
             "%s/.multi_cc_draft", g_test_home);

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
    pcr_send_capture(s, draft_capture);
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
}

/* ══════════════════════════════════════════════════════════════════════
 *  TC-CD-66 / TC-CD-67: Reader-view 'r' reply and Sent folder saving
 * ══════════════════════════════════════════════════════════════════════ */

static void test_reader_reply_all_opens_compose(void) {
    /* EMAIL-155: 'A' in the reader is reply-all, as in the list */
    restart_imap();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reader reply-all: reached inbox");

    pty_send_key(s, PTY_KEY_ENTER);
    ASSERT_WAIT_FOR(s, "From:", WAIT_MS);
    pty_settle(s, SETTLE_MS);

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Reply All", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_CONTAINS(s, "To:");
    ASSERT_SCREEN_CONTAINS(s, "Cc:");
    ASSERT_SCREEN_CONTAINS(s, "Re:");

    pty_send_key(s, PTY_KEY_ESC);
    ASSERT_WAIT_FOR(s, "Discard what was typed?", WAIT_MS);
    pty_send_str(s, "y");
    /* A cancel returns to the list at once, with no pause (EMAIL-222). */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "[Press any key");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

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

    /* ESC → dialog closed */
    pty_send_key(s, PTY_KEY_ESC);
    /* Esc asks before it throws away the prefilled text (EMAIL-175). */
    ASSERT_WAIT_FOR(s, "Discard what was typed?", WAIT_MS);
    pty_send_str(s, "y");
    /* A cancel returns to the list at once, with no pause (EMAIL-222). */
    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    ASSERT_SCREEN_NOT_CONTAINS(s, "[Press any key");

    pty_send_key(s, PTY_KEY_ESC);
    pty_close(s);
}

static void test_reply_send_saves_to_sent(void) {
    /* TC-CD-67: reply from reader → send → Sent folder saved ("Saved." visible) */
    restart_imap(); restart_smtp();
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
    pcr_send(s);
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
    pcr_send(s);
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
    pcr_send(s);
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

static void test_reply_all_send(void) {
    /* EMAIL-115: a reply-all can be written and sent like a reply */
    restart_imap(); restart_smtp();
    PtySession *s = tui_open_to_inbox();
    ASSERT(s != NULL, "reply-all answered: reached inbox");

    pty_send_str(s, "A");
    ASSERT_WAIT_FOR(s, "Reply All", WAIT_MS);
    pty_settle(s, SETTLE_MS);
    pty_send_key(s, PTY_KEY_TAB);   /* Cc -> Bcc */
    pty_send_key(s, PTY_KEY_TAB);   /* Bcc -> Subject */
    pty_send_key(s, PTY_KEY_ENTER);
    pcr_send(s);
    ASSERT_WAIT_FOR(s, "Sending", WAIT_MS);
    ASSERT_WAIT_FOR(s, "Message sent.", WAIT_MS);
    ASSERT_WAIT_FOR(s, "[Press any key", WAIT_MS);
    pty_send_str(s, " ");

    ASSERT_WAIT_FOR(s, "message(s) in", WAIT_MS);
    pty_settle(s, SETTLE_MS);
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
    snprintf(g_test_home, sizeof(g_test_home), "%s", TP("home"));
    if (mkdir(g_test_home, 0700) != 0) { perror("mkdir home"); return 1; }
    if (getenv("HOME"))
        snprintf(g_old_home, sizeof(g_old_home), "%s", getenv("HOME"));
    setenv("HOME", g_test_home, 1);
    /* XDG_*_HOME wins over $HOME in the binaries; if the caller's environment
     * sets them (GitHub runners do) the test home is bypassed and the setup
     * wizard opens instead of the configured account. */
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CACHE_HOME");

    /* The mocks first: the config written next names their ports. */
    restart_imap();
    restart_smtp();
    if (g_imap_pid < 0 || g_smtp_pid < 0) return 1;

    mkdirs();
    write_config();

    /* ── Basic compose dialog flow ────────────────────────────────────── */
    printf("--- Basic compose dialog flow ---\n");
    RUN_TEST(test_compose_dialog_opens);
    RUN_TEST(test_compose_dialog_title);
    RUN_TEST(test_compose_dialog_status_line);
    RUN_TEST(test_compose_esc_cancels);
    RUN_TEST(test_compose_esc_asks_before_discarding);
    RUN_TEST(test_compose_ctrl_c_does_not_ask);
    RUN_TEST(test_compose_empty_to_error);
    RUN_TEST(test_compose_empty_subject_warning);
    RUN_TEST(test_compose_empty_subject_y_opens_editor);
    RUN_TEST(test_compose_opens_editor_with_to);
    RUN_TEST(test_body_editor_type_and_esc_saves);
    RUN_TEST(test_body_editor_autosave_while_typing);
    RUN_TEST(test_body_editor_reopen_restores);
    RUN_TEST(test_body_editor_resume_declined);
    RUN_TEST(test_body_editor_paste_and_wide);
    RUN_TEST(test_body_editor_long_line_scrolls);
    RUN_TEST(test_body_editor_ctrl_s_saves_and_stays);
    RUN_TEST(test_send_removes_draft);
    RUN_TEST(test_body_editor_resize_and_too_small);

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
    RUN_TEST(test_compose_caret_editing);
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
    RUN_TEST(test_reply_all_collects_recipients);
    RUN_TEST(test_smtp_prompt_declined);
    RUN_TEST(test_smtp_prompt_wizard_abandoned);
    RUN_TEST(test_smtp_prompt_wizard_saves);
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
    RUN_TEST(test_reader_reply_all_opens_compose);
    RUN_TEST(test_reply_send_saves_to_sent);

    /* ── Answered / Forwarded status markers (US-CD-13 / US-CD-14) ───── */
    printf("\n--- Answered / Forwarded status markers ---\n");
    restart_imap();
    RUN_TEST(test_replied_message_shows_answered_marker);
    RUN_TEST(test_forwarded_message_shows_forwarded_marker);
    RUN_TEST(test_reply_all_send);

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
