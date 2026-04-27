/**
 * @file test_pty_mail_rules.c
 * @brief PTY/functional tests for mail sorting rules (US-MR-01 … US-MR-06).
 *
 * Usage: test-pty-mail-rules <email-sync> <email-tui> <mock-imap-server>
 *
 * Tests verify that mail sorting rules defined in rules.ini are applied
 * during sync (and retroactively via --apply-rules), and that flag bits
 * are set correctly in the local manifest.
 *
 * MSG_FLAG values (from local_store.h):
 *   MSG_FLAG_UNSEEN  = 1 (bit 0)
 *   MSG_FLAG_FLAGGED = 2 (bit 1)
 *   MSG_FLAG_DONE    = 4 (bit 2)
 *   MSG_FLAG_ATTACH  = 8 (bit 3)
 *   MSG_FLAG_JUNK    = 64 (bit 6)
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

#define WAIT_MS   6000
#define SETTLE_MS  600
#define ROWS        24
#define COLS       100

/* Flag bit values (mirror local_store.h) */
#define FLAG_FLAGGED  (1 << 1)   /* MSG_FLAG_FLAGGED */
#define FLAG_JUNK     (1 << 6)   /* MSG_FLAG_JUNK    */

/* ── Global state ────────────────────────────────────────────────────── */

static pid_t g_mock_pid  = -1;
static char  g_test_home[256];
static char  g_old_home[512];
static char  g_sync_bin[512];
static char  g_tui_bin[512];
static char  g_mock_bin[512];

/* ── Infrastructure ──────────────────────────────────────────────────── */

/** Write the standard IMAP test config. */
static void write_config(void) {
    char d1[300], d2[300], d3[350], d4[400], path[450];
    snprintf(d1, sizeof(d1), "%s/.config",                                 g_test_home);
    snprintf(d2, sizeof(d2), "%s/.config/email-cli",                       g_test_home);
    snprintf(d3, sizeof(d3), "%s/.config/email-cli/accounts",              g_test_home);
    snprintf(d4, sizeof(d4), "%s/.config/email-cli/accounts/testuser",     g_test_home);
    mkdir(g_test_home, 0700);
    mkdir(d1, 0700);
    mkdir(d2, 0700);
    mkdir(d3, 0700);
    mkdir(d4, 0700);
    snprintf(path, sizeof(path), "%s/config.ini", d4);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp,
        "EMAIL_HOST=imaps://localhost:9993\n"
        "EMAIL_USER=testuser\n"
        "EMAIL_PASS=testpass\n"
        "EMAIL_FOLDER=INBOX\n"
        "SSL_NO_VERIFY=1\n");
    fclose(fp);
    chmod(path, 0600);
}

/**
 * Write (or overwrite) rules.ini for the testuser account.
 * Pass NULL to delete the file (simulates missing rules.ini).
 */
static void write_rules(const char *content) {
    char dir[450], path[500];
    snprintf(dir, sizeof(dir),
             "%s/.config/email-cli/accounts/testuser", g_test_home);
    mkdir(dir, 0700);   /* ensure it exists */
    snprintf(path, sizeof(path), "%s/rules.ini", dir);
    if (!content) {
        unlink(path);
        return;
    }
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fputs(content, fp);
    fclose(fp);
    chmod(path, 0600);
}

/** Wipe the entire local email-cli store (account data + UI prefs). */
static void reset_all_state(void) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd),
             "rm -rf '%s/.local/share/email-cli'", g_test_home);
    int _rc = system(cmd); (void)_rc;
}

/* ── Mock IMAP server ────────────────────────────────────────────────── */

static int probe_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(9993),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    close(fd);
    return ret;
}

static int start_mock_server(void) {
    g_mock_pid = fork();
    if (g_mock_pid < 0) return -1;
    if (g_mock_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execl(g_mock_bin, "mock_imap_server", (char *)NULL);
        _exit(127);
    }
    usleep(800000);
    return 0;
}

static void stop_mock_server(void) {
    if (g_mock_pid > 0) {
        kill(g_mock_pid, SIGKILL);
        waitpid(g_mock_pid, NULL, 0);
        g_mock_pid = -1;
    }
}

static void restart_mock(void) {
    if (g_mock_pid > 0) {
        kill(g_mock_pid, SIGKILL);
        waitpid(g_mock_pid, NULL, 0);
        g_mock_pid = -1;
    }
    usleep(200000);
    start_mock_server();
    for (int i = 0; i < 30 && probe_server() != 0; i++)
        usleep(100000);
}

/* ── Sync / TUI helpers ──────────────────────────────────────────────── */

/** Run email-sync (possibly with extra args) in a PTY and return the session. */
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

/**
 * Scan the manifest TSV for the testuser/INBOX folder and return the flags
 * integer of the first entry whose subject field contains @p subject_needle.
 *
 * Manifest line format (tab-separated):
 *   uid  from  subject  date  flags
 *
 * Returns -1 if the manifest cannot be opened or the entry is not found.
 */
static int find_manifest_flags(const char *subject_needle) {
    char path[600];
    snprintf(path, sizeof(path),
             "%s/.local/share/email-cli/accounts/testuser/manifests/INBOX.tsv",
             g_test_home);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[1024];
    int found_flags = -1;
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* Split by tabs: uid, from, subject, date, flags */
        (void)strtok(line, "\t");         /* uid — discard */
        char *from = strtok(NULL, "\t");  /* from */
        char *subj = strtok(NULL, "\t");  /* subject */
        char *date = strtok(NULL, "\t");  /* date */
        char *fstr = strtok(NULL, "\t");  /* flags */
        (void)from; (void)date;

        if (!subj || !fstr) continue;
        if (subject_needle && strstr(subj, subject_needle) == NULL) continue;

        found_flags = atoi(fstr);
        break;
    }
    fclose(fp);
    return found_flags;
}

/* ══════════════════════════════════════════════════════════════════════
 *  US-MR-05: Missing rules.ini must not break sync
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * US-MR-05: If no rules.ini exists the sync must complete normally and
 * print "Sync complete".
 */
static void test_rule_no_rules_sync_succeeds(void) {
    reset_all_state();
    write_config();
    write_rules(NULL);   /* ensure rules.ini is absent */
    restart_mock();

    PtySession *s = sync_run(NULL);
    ASSERT(s != NULL, "no-rules sync: PTY opens");
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);
}

/* ══════════════════════════════════════════════════════════════════════
 *  US-MR-01: if-from glob flags matching messages
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * US-MR-01: A rule matching the sender via if-from glob adds _flagged
 * label, which maps to MSG_FLAG_FLAGGED (bit 1) in the manifest.
 * Mock sends "From: Test User <test@example.com>".
 */
static void test_rule_from_glob_flags_message(void) {
    reset_all_state();
    write_config();
    write_rules(
        "[rule \"Flag example.com\"]\n"
        "if-from = *@example.com\n"
        "then-add-label = _flagged\n"
    );
    restart_mock();

    PtySession *s = sync_run(NULL);
    ASSERT(s != NULL, "from-glob flag: PTY opens");
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);

    int flags = find_manifest_flags("Test Message");
    ASSERT(flags >= 0, "from-glob flag: manifest entry found");
    ASSERT((flags & FLAG_FLAGGED) != 0,
           "from-glob flag: MSG_FLAG_FLAGGED bit set in manifest");
}

/* ══════════════════════════════════════════════════════════════════════
 *  US-MR-02: if-subject glob flags matching messages
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * US-MR-02: A rule matching the subject via if-subject glob adds _flagged
 * label.  Mock message subject is "Test Message".
 */
static void test_rule_subject_glob_flags_message(void) {
    reset_all_state();
    write_config();
    write_rules(
        "[rule \"Flag Test subjects\"]\n"
        "if-subject = *Test*\n"
        "then-add-label = _flagged\n"
    );
    restart_mock();

    PtySession *s = sync_run(NULL);
    ASSERT(s != NULL, "subject-glob flag: PTY opens");
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);

    int flags = find_manifest_flags("Test Message");
    ASSERT(flags >= 0, "subject-glob flag: manifest entry found");
    ASSERT((flags & FLAG_FLAGGED) != 0,
           "subject-glob flag: MSG_FLAG_FLAGGED bit set in manifest");
}

/* ══════════════════════════════════════════════════════════════════════
 *  US-MR-03: Non-matching rule must not affect unrelated messages
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * US-MR-03: A rule whose if-from glob does not match the sender must not
 * set any flag bits.  Mock sends "test@example.com", rule matches
 * "*@nomatch.example.com" which will never fire.
 */
static void test_rule_nonmatch_no_effect(void) {
    reset_all_state();
    write_config();
    write_rules(
        "[rule \"No match\"]\n"
        "if-from = *@nomatch.example.com\n"
        "then-add-label = _flagged\n"
    );
    restart_mock();

    PtySession *s = sync_run(NULL);
    ASSERT(s != NULL, "nonmatch rule: PTY opens");
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);

    int flags = find_manifest_flags("Test Message");
    ASSERT(flags >= 0, "nonmatch rule: manifest entry found");
    ASSERT((flags & FLAG_FLAGGED) == 0,
           "nonmatch rule: MSG_FLAG_FLAGGED must NOT be set");
}

/* ══════════════════════════════════════════════════════════════════════
 *  US-MR-04: Multiple rules run in order; all matching actions apply
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * US-MR-04: When two rules both match (one on subject, one on from),
 * both actions must be applied.  Expect FLAG_FLAGGED | FLAG_JUNK.
 */
static void test_rule_multiple_rules_both_apply(void) {
    reset_all_state();
    write_config();
    write_rules(
        "[rule \"Flag by subject\"]\n"
        "if-subject = *Test*\n"
        "then-add-label = _flagged\n"
        "\n"
        "[rule \"Junk by from\"]\n"
        "if-from = *@example.com\n"
        "then-add-label = _junk\n"
    );
    restart_mock();

    PtySession *s = sync_run(NULL);
    ASSERT(s != NULL, "multi-rule: PTY opens");
    ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
    pty_close(s);

    int flags = find_manifest_flags("Test Message");
    ASSERT(flags >= 0, "multi-rule: manifest entry found");
    ASSERT((flags & FLAG_FLAGGED) != 0,
           "multi-rule: MSG_FLAG_FLAGGED bit set (rule 1 fired)");
    ASSERT((flags & FLAG_JUNK) != 0,
           "multi-rule: MSG_FLAG_JUNK bit set (rule 2 fired)");
}

/* ══════════════════════════════════════════════════════════════════════
 *  US-MR-06: --apply-rules retroactively applies rules to existing messages
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * US-MR-06: Sync without rules (message is downloaded, no flags set),
 * then create rules.ini, then sync --apply-rules → message is now flagged.
 */
static void test_rule_apply_rules_retroactive(void) {
    /* Step 1: sync without any rules */
    reset_all_state();
    write_config();
    write_rules(NULL);   /* no rules.ini */
    restart_mock();

    {
        PtySession *s = sync_run(NULL);
        ASSERT(s != NULL, "apply-rules retro: initial sync PTY opens");
        ASSERT_WAIT_FOR(s, "Sync complete", WAIT_MS);
        pty_close(s);
    }

    /* Verify no flags set after initial sync */
    int flags_before = find_manifest_flags("Test Message");
    ASSERT(flags_before >= 0, "apply-rules retro: manifest entry found after initial sync");
    ASSERT((flags_before & FLAG_FLAGGED) == 0,
           "apply-rules retro: no FLAGGED bit before rules are created");

    /* Step 2: create rules.ini targeting the already-downloaded message */
    write_rules(
        "[rule \"Retroactive flag\"]\n"
        "if-from = *@example.com\n"
        "then-add-label = _flagged\n"
    );

    /* Step 3: run email-sync --apply-rules (no server contact needed) */
    {
        const char *args[] = {"--apply-rules", NULL};
        PtySession *s = sync_run(args);
        ASSERT(s != NULL, "apply-rules retro: --apply-rules PTY opens");
        /* --apply-rules prints "Rules applied." or "Sync complete" */
        ASSERT_WAIT_FOR(s, "ule", WAIT_MS);   /* matches "Rules applied." */
        pty_close(s);
    }

    /* Step 4: verify the flag bit is now set */
    int flags_after = find_manifest_flags("Test Message");
    ASSERT(flags_after >= 0, "apply-rules retro: manifest entry found after --apply-rules");
    ASSERT((flags_after & FLAG_FLAGGED) != 0,
           "apply-rules retro: MSG_FLAG_FLAGGED bit set after --apply-rules");
}

/* ══════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    printf("--- email-cli PTY Mail Rules Tests ---\n\n");

    if (argc < 4) {
        fprintf(stderr,
                "Usage: %s <email-sync> <email-tui> <mock-imap-server>\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(g_sync_bin,  sizeof(g_sync_bin),  "%s", argv[1]);
    snprintf(g_tui_bin,   sizeof(g_tui_bin),   "%s", argv[2]);
    snprintf(g_mock_bin,  sizeof(g_mock_bin),  "%s", argv[3]);

    const char *home = getenv("HOME");
    if (home) snprintf(g_old_home, sizeof(g_old_home), "%s", home);

    snprintf(g_test_home, sizeof(g_test_home),
             "/tmp/email-cli-rules-test-%d", getpid());
    setenv("HOME", g_test_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_DATA_HOME");

    mkdir(g_test_home, 0700);
    write_config();

    printf("--- Mail rules: no rules ---\n");
    RUN_TEST(test_rule_no_rules_sync_succeeds);

    printf("\n--- Mail rules: from-glob (US-MR-01) ---\n");
    RUN_TEST(test_rule_from_glob_flags_message);

    printf("\n--- Mail rules: subject-glob (US-MR-02) ---\n");
    RUN_TEST(test_rule_subject_glob_flags_message);

    printf("\n--- Mail rules: non-matching rule (US-MR-03) ---\n");
    RUN_TEST(test_rule_nonmatch_no_effect);

    printf("\n--- Mail rules: multiple rules (US-MR-04) ---\n");
    RUN_TEST(test_rule_multiple_rules_both_apply);

    printf("\n--- Mail rules: retroactive --apply-rules (US-MR-06) ---\n");
    RUN_TEST(test_rule_apply_rules_retroactive);

    stop_mock_server();
    if (g_old_home[0]) setenv("HOME", g_old_home, 1);

    printf("\n--- PTY Mail Rules Test Results ---\n");
    printf("Tests Run:    %d\n", g_tests_run);
    printf("Tests Passed: %d\n", g_tests_run - g_tests_failed);
    printf("Tests Failed: %d\n", g_tests_failed);

    return g_tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
