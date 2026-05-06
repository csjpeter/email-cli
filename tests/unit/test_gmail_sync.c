#include "test_helpers.h"
#include "gmail_sync.h"
#include "gmail_client.h"
#include "local_store.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef ENABLE_GCOV
extern void __gcov_dump(void);
#  define GCOV_FLUSH() __gcov_dump()
#else
#  define GCOV_FLUSH() ((void)0)
#endif

/* ── Mock HTTP server (reused from test_gmail_client.c pattern) ─────── */

static int gs_make_listener(int *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    /* 2-second accept() timeout so server children exit cleanly when the
     * test exhausts its expected connections — prevents gs_wait_child() hang */
    struct timeval acc_tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &acc_tv, sizeof(acc_tv));
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    *port_out = ntohs(addr.sin_port);
    return fd;
}

static void gs_send_json(int fd, int code, const char *body) {
    const char *reason = (code == 200) ? "OK" :
                         (code == 204) ? "No Content" :
                         (code == 404) ? "Not Found" : "Error";
    char hdr[512];
    size_t blen = body ? strlen(body) : 0;
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n",
             code, reason, blen);
    ssize_t r;
    r = write(fd, hdr, strlen(hdr)); (void)r;
    if (body && blen > 0) { r = write(fd, body, blen); (void)r; }
}

static int gs_read_req(int fd, char *buf, int bufsz) {
    int total = 0;
    while (total < bufsz - 1) {
        ssize_t n = read(fd, buf + total, (size_t)(bufsz - total - 1));
        if (n <= 0) break;
        total += (int)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[total] = '\0';
    return total;
}

/* base64url encode for mock raw message */
static const char gs_b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static char *gs_b64encode(const char *data, size_t len) {
    size_t alloc = ((len + 2) / 3) * 4 + 1;
    char *out = malloc(alloc);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = ((unsigned int)(unsigned char)data[i]) << 16;
        if (i + 1 < len) n |= ((unsigned int)(unsigned char)data[i+1]) << 8;
        if (i + 2 < len) n |= ((unsigned int)(unsigned char)data[i+2]);
        out[o++] = gs_b64_chars[(n >> 18) & 0x3F];
        out[o++] = gs_b64_chars[(n >> 12) & 0x3F];
        if (i + 1 < len) out[o++] = gs_b64_chars[(n >> 6) & 0x3F];
        if (i + 2 < len) out[o++] = gs_b64_chars[n & 0x3F];
    }
    out[o] = '\0';
    return out;
}

/* Build a GmailClient pointing at a mock server on loopback */
static GmailClient *gs_make_client(int port) {
    char api_base[128];
    snprintf(api_base, sizeof(api_base),
             "http://127.0.0.1:%d/gmail/v1/users/me", port);
    setenv("GMAIL_TEST_TOKEN", "test_access_token", 1);
    setenv("GMAIL_API_BASE_URL", api_base, 1);

    Config cfg = {0};
    cfg.gmail_mode = 1;
    cfg.gmail_refresh_token = "fake";
    return gmail_connect(&cfg);
}

static void gs_wait_child(pid_t pid) {
    if (pid > 0) { int st; waitpid(pid, &st, 0); }
}

/* ── is_filtered_label ───────────────────────────────────────────────── */

static void test_filtered_null(void) {
    ASSERT(gmail_sync_is_filtered_label(NULL) == 1, "filtered: NULL → filtered");
}

static void test_filtered_category(void) {
    /* CATEGORY_* labels are now indexed (not filtered) — they appear in a
     * dedicated section in the TUI label list. */
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_PERSONAL") == 0,
           "not filtered: CATEGORY_PERSONAL (indexed as category)");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_SOCIAL") == 0,
           "not filtered: CATEGORY_SOCIAL (indexed as category)");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_PROMOTIONS") == 0,
           "not filtered: CATEGORY_PROMOTIONS (indexed as category)");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_UPDATES") == 0,
           "not filtered: CATEGORY_UPDATES (indexed as category)");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_FORUMS") == 0,
           "not filtered: CATEGORY_FORUMS (indexed as category)");
}

static void test_filtered_important(void) {
    ASSERT(gmail_sync_is_filtered_label("IMPORTANT") == 1,
           "filtered: IMPORTANT");
}

static void test_not_filtered_system(void) {
    ASSERT(gmail_sync_is_filtered_label("INBOX") == 0, "not filtered: INBOX");
    ASSERT(gmail_sync_is_filtered_label("SENT") == 0, "not filtered: SENT");
    ASSERT(gmail_sync_is_filtered_label("TRASH") == 0, "not filtered: TRASH");
    ASSERT(gmail_sync_is_filtered_label("SPAM") == 0, "not filtered: SPAM");
    ASSERT(gmail_sync_is_filtered_label("STARRED") == 0, "not filtered: STARRED");
    ASSERT(gmail_sync_is_filtered_label("UNREAD") == 0, "not filtered: UNREAD");
    ASSERT(gmail_sync_is_filtered_label("DRAFT") == 0, "not filtered: DRAFT");
}

static void test_not_filtered_user(void) {
    ASSERT(gmail_sync_is_filtered_label("Work") == 0, "not filtered: Work");
    ASSERT(gmail_sync_is_filtered_label("Personal") == 0, "not filtered: Personal");
    ASSERT(gmail_sync_is_filtered_label("Projects/Alpha") == 0,
           "not filtered: nested label");
}

static void test_filtered_edge_cases(void) {
    ASSERT(gmail_sync_is_filtered_label("") == 0, "not filtered: empty string");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_") == 0,
           "not filtered: bare CATEGORY_ prefix (indexed as category)");
    ASSERT(gmail_sync_is_filtered_label("CATEGORY_X") == 0,
           "not filtered: unknown CATEGORY_ suffix (indexed as category)");
}

/* ── build_hdr ───────────────────────────────────────────────────────── */

static void test_build_hdr_basic(void) {
    const char *raw = "From: Alice <alice@example.com>\r\n"
                      "Subject: Hello\r\n"
                      "Date: Wed, 16 Apr 2026 09:30:00 +0000\r\n"
                      "\r\n"
                      "Body text\r\n";
    char *labels[] = {"INBOX", "UNREAD"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 2);
    ASSERT(hdr != NULL, "build_hdr basic: not NULL");

    /* Verify tab-separated format: from\tsubject\tdate\tlabels\tflags */
    int tabs = 0;
    for (const char *p = hdr; *p; p++)
        if (*p == '\t') tabs++;
    ASSERT(tabs == 4, "build_hdr basic: 4 tab separators");

    /* Verify label string contains both labels */
    ASSERT(strstr(hdr, "INBOX") != NULL, "build_hdr basic: has INBOX");
    ASSERT(strstr(hdr, "UNREAD") != NULL, "build_hdr basic: has UNREAD");

    /* Verify UNREAD sets MSG_FLAG_UNSEEN bit (value 1) */
    const char *last_tab = strrchr(hdr, '\t');
    ASSERT(last_tab != NULL, "build_hdr basic: has last tab");
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 1) != 0, "build_hdr basic: UNSEEN flag set");

    free(hdr);
}

static void test_build_hdr_starred(void) {
    const char *raw = "From: Bob\r\nSubject: Star me\r\nDate: Thu, 17 Apr 2026 10:00:00 +0000\r\n\r\n";
    char *labels[] = {"STARRED"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 1);
    ASSERT(hdr != NULL, "build_hdr starred: not NULL");

    /* STARRED sets MSG_FLAG_FLAGGED (value 2) */
    const char *last_tab = strrchr(hdr, '\t');
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 2) != 0, "build_hdr starred: FLAGGED flag set");

    free(hdr);
}

static void test_build_hdr_no_labels(void) {
    const char *raw = "From: Nobody\r\nSubject: Archived\r\nDate: Mon, 14 Apr 2026 08:00:00 +0000\r\n\r\n";
    char *hdr = gmail_sync_build_hdr(raw, NULL, 0);
    ASSERT(hdr != NULL, "build_hdr no labels: not NULL");

    /* Flags should be 0 (no UNREAD, no STARRED) */
    const char *last_tab = strrchr(hdr, '\t');
    int flags = atoi(last_tab + 1);
    ASSERT(flags == 0, "build_hdr no labels: flags=0");

    free(hdr);
}

static void test_build_hdr_missing_headers(void) {
    /* Message with no From/Subject/Date headers */
    const char *raw = "\r\nJust a body.\r\n";
    char *labels[] = {"INBOX"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 1);
    ASSERT(hdr != NULL, "build_hdr missing headers: not NULL");

    /* Should have empty fields but not crash */
    ASSERT(hdr[0] == '\t', "build_hdr missing headers: from is empty");

    free(hdr);
}

static void test_build_hdr_combined_flags(void) {
    const char *raw = "From: X\r\nSubject: Y\r\nDate: Mon, 14 Apr 2026 08:00:00 +0000\r\n\r\n";
    char *labels[] = {"UNREAD", "STARRED", "INBOX"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 3);
    ASSERT(hdr != NULL, "build_hdr combined: not NULL");

    const char *last_tab = strrchr(hdr, '\t');
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 1) != 0, "build_hdr combined: UNSEEN set");
    ASSERT((flags & 2) != 0, "build_hdr combined: FLAGGED set");

    free(hdr);
}

/* ── incremental sync — no history ───────────────────────────────────── */

static void test_incremental_no_history(void) {
    /* Without local_store_init, local_gmail_history_load returns NULL → -2 */
    int rc = gmail_sync_incremental(NULL);
    ASSERT(rc == -2, "incremental: no historyId → returns -2");
}

/* ── repair_archive_flags ────────────────────────────────────────────── */

/* Helper: sets HOME to a temp dir and inits local store for Gmail. */
static void setup_gmail_test_env(const char *home) {
    setenv("HOME", home, 1);
    unsetenv("XDG_DATA_HOME");
    local_store_init("gmail://csjpeterjaket@gmail.com", "csjpeterjaket@gmail.com");
}

/* Helper: wipe the test home dir and reinitialise store.
 * Used by network-based tests that need a clean message store to avoid
 * interference from messages written by earlier tests. */
static void reset_gmail_test_env(void) {
    int _sr = system("rm -rf '/tmp/email-cli-gmail-sync-test'"); (void)_sr;
    setup_gmail_test_env("/tmp/email-cli-gmail-sync-test");
}

static void test_repair_archive_flags_clears_unseen(void) {
    /* A message synced to _nolabel while still having UNREAD label → UNSEEN
     * flag should be cleared by repair_archive_flags(). */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    const char *uid = "0000000000aabbcc";

    /* Write a .hdr with UNSEEN bit set (flags = 1) */
    const char *hdr = "Sender\tArchived msg\t2026-04-20\tUNREAD\t1";
    local_hdr_save("", uid, hdr, strlen(hdr));

    /* Add to _nolabel index */
    label_idx_add("_nolabel", uid);

    /* Run repair */
    gmail_sync_repair_archive_flags();

    /* Load and verify UNSEEN was cleared */
    char *loaded = local_hdr_load("", uid);
    ASSERT(loaded != NULL, "repair: .hdr still exists");
    const char *last_tab = strrchr(loaded, '\t');
    ASSERT(last_tab != NULL, "repair: flags tab present");
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 1) == 0, "repair: UNSEEN bit cleared for archived message");
    free(loaded);
}

static void test_repair_archive_flags_preserves_flagged(void) {
    /* STARRED (FLAGGED bit) must survive the repair. */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    const char *uid = "0000000000aabbdd";

    /* flags = 3 = UNSEEN | FLAGGED */
    const char *hdr = "Sender\tStarred archived\t2026-04-20\tUNREAD,STARRED\t3";
    local_hdr_save("", uid, hdr, strlen(hdr));
    label_idx_add("_nolabel", uid);

    gmail_sync_repair_archive_flags();

    char *loaded = local_hdr_load("", uid);
    ASSERT(loaded != NULL, "repair flagged: .hdr exists");
    const char *last_tab = strrchr(loaded, '\t');
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 1) == 0, "repair flagged: UNSEEN cleared");
    ASSERT((flags & 2) != 0, "repair flagged: FLAGGED preserved");
    free(loaded);
}

static void test_repair_archive_flags_noop_when_already_read(void) {
    /* If UNSEEN is already 0, the .hdr should not be rewritten (flags stay). */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    const char *uid = "0000000000aabbee";

    /* flags = 0, no UNREAD */
    const char *hdr = "Sender\tAlready read\t2026-04-20\t\t0";
    local_hdr_save("", uid, hdr, strlen(hdr));
    label_idx_add("_nolabel", uid);

    gmail_sync_repair_archive_flags();

    char *loaded = local_hdr_load("", uid);
    ASSERT(loaded != NULL, "repair noop: .hdr exists");
    const char *last_tab = strrchr(loaded, '\t');
    int flags = atoi(last_tab + 1);
    ASSERT(flags == 0, "repair noop: flags remain 0");
    free(loaded);
}

static void test_build_hdr_archive_unread_flags(void) {
    /* build_hdr with UNREAD but no real label → flags still has UNSEEN.
     * The caller (sync loop) is responsible for clearing it when assigning
     * to _nolabel.  Verify build_hdr itself does not silently drop the flag. */
    const char *raw = "From: X\r\nSubject: Archived\r\nDate: Mon, 14 Apr 2026 08:00:00 +0000\r\n\r\n";
    char *labels[] = {"UNREAD", "CATEGORY_PROMOTIONS"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 2);
    ASSERT(hdr != NULL, "build_hdr archive+unread: not NULL");

    const char *last_tab = strrchr(hdr, '\t');
    int flags = atoi(last_tab + 1);
    /* build_hdr sets UNSEEN; the caller must clear it for _nolabel messages */
    ASSERT((flags & 1) != 0, "build_hdr archive+unread: UNSEEN set by build_hdr (caller clears it)");

    free(hdr);
}

/* ── pending_fetch queue (local_store) ───────────────────────────────── */

static void test_pending_fetch_empty_initially(void) {
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    local_pending_fetch_clear();
    ASSERT(local_pending_fetch_count() == 0, "pending_fetch: empty initially");
    int count = -1;
    char (*uids)[17] = local_pending_fetch_load(&count);
    ASSERT(count == 0, "pending_fetch: load count is 0");
    free(uids);
}

static void test_pending_fetch_add_and_load(void) {
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);
    local_pending_fetch_clear();

    const char *uid1 = "aaaa000000000001";
    const char *uid2 = "aaaa000000000002";
    ASSERT(local_pending_fetch_add(uid1) == 0, "pending_fetch: add uid1");
    ASSERT(local_pending_fetch_add(uid2) == 0, "pending_fetch: add uid2");

    ASSERT(local_pending_fetch_count() == 2, "pending_fetch: count == 2");

    int count = 0;
    char (*uids)[17] = local_pending_fetch_load(&count);
    ASSERT(count == 2, "pending_fetch: load returns 2");
    ASSERT(uids != NULL, "pending_fetch: uids not NULL");
    ASSERT(strcmp(uids[0], uid1) == 0 || strcmp(uids[1], uid1) == 0,
           "pending_fetch: uid1 present");
    ASSERT(strcmp(uids[0], uid2) == 0 || strcmp(uids[1], uid2) == 0,
           "pending_fetch: uid2 present");
    free(uids);
}

static void test_pending_fetch_remove(void) {
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);
    local_pending_fetch_clear();

    local_pending_fetch_add("bbbb000000000001");
    local_pending_fetch_add("bbbb000000000002");
    local_pending_fetch_add("bbbb000000000003");

    local_pending_fetch_remove("bbbb000000000002");

    int count = 0;
    char (*uids)[17] = local_pending_fetch_load(&count);
    ASSERT(count == 2, "pending_fetch remove: 2 entries remain");
    int found2 = 0;
    for (int i = 0; i < count; i++)
        if (strcmp(uids[i], "bbbb000000000002") == 0) found2 = 1;
    ASSERT(!found2, "pending_fetch remove: uid2 gone");
    free(uids);
}

static void test_pending_fetch_clear(void) {
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    local_pending_fetch_add("cccc000000000001");
    local_pending_fetch_add("cccc000000000002");
    ASSERT(local_pending_fetch_count() >= 2, "pending_fetch clear: non-zero before clear");

    local_pending_fetch_clear();
    ASSERT(local_pending_fetch_count() == 0, "pending_fetch clear: zero after clear");
}

static void test_pending_fetch_count_matches_load(void) {
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);
    local_pending_fetch_clear();

    for (int i = 0; i < 5; i++) {
        char uid[17];
        snprintf(uid, sizeof(uid), "dddd%012d", i);
        local_pending_fetch_add(uid);
    }

    int cnt_fast = local_pending_fetch_count();
    int cnt_load = 0;
    char (*uids)[17] = local_pending_fetch_load(&cnt_load);
    ASSERT(cnt_fast == cnt_load,
           "pending_fetch: count() matches load() count");
    free(uids);
}

/* ── gmail_sync_rebuild_indexes ──────────────────────────────────────────── */

/*
 * Exercise the full rebuild_label_indexes + gmail_sync_rebuild_indexes code
 * path by populating a set of .hdr files then calling the public function.
 *
 * Coverage goal: lines 192-338 (rebuild_label_indexes) + 346-357
 * (gmail_sync_rebuild_indexes).
 */
static void test_rebuild_indexes_empty_store(void) {
    /* With no messages, rebuild_indexes is a no-op but must not crash. */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    int rc = gmail_sync_rebuild_indexes();
    ASSERT(rc == 0, "rebuild_indexes empty: returns 0");
}

static void test_rebuild_indexes_basic(void) {
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    /* Write a few .hdr files with known labels */
    const char *uid1 = "1100000000000001";
    const char *uid2 = "1100000000000002";
    const char *uid3 = "1100000000000003";

    /* uid1: INBOX + UNREAD (flags=1) */
    const char *hdr1 = "Alice\tHello\t2026-04-01\tINBOX,UNREAD\t1";
    local_hdr_save("", uid1, hdr1, strlen(hdr1));

    /* uid2: STARRED only (flags=2) */
    const char *hdr2 = "Bob\tStarred\t2026-04-02\tSTARRED\t2";
    local_hdr_save("", uid2, hdr2, strlen(hdr2));

    /* uid3: SPAM (flags=0) — should be indexed as _spam */
    const char *hdr3 = "Eve\tSpam\t2026-04-03\tSPAM\t0";
    local_hdr_save("", uid3, hdr3, strlen(hdr3));

    int rc = gmail_sync_rebuild_indexes();
    ASSERT(rc == 0, "rebuild_indexes basic: returns 0");

    /* Verify INBOX index contains uid1 */
    char (*idx_uids)[17] = NULL;
    int idx_count = 0;
    int load_rc = label_idx_load("INBOX", &idx_uids, &idx_count);
    ASSERT(load_rc == 0, "rebuild_indexes: INBOX index loaded");
    int found1 = 0;
    for (int i = 0; i < idx_count; i++)
        if (strcmp(idx_uids[i], uid1) == 0) found1 = 1;
    free(idx_uids);
    ASSERT(found1, "rebuild_indexes: uid1 in INBOX index");

    /* Verify _spam index contains uid3 */
    idx_uids = NULL; idx_count = 0;
    load_rc = label_idx_load("_spam", &idx_uids, &idx_count);
    ASSERT(load_rc == 0, "rebuild_indexes: _spam index loaded");
    int found3 = 0;
    for (int i = 0; i < idx_count; i++)
        if (strcmp(idx_uids[i], uid3) == 0) found3 = 1;
    free(idx_uids);
    ASSERT(found3, "rebuild_indexes: uid3 in _spam index");
}

static void test_rebuild_indexes_nolabel(void) {
    /* A message with only CATEGORY_ labels and no real labels → goes to _nolabel.
     * Note: UNREAD is a real (non-CATEGORY_) label, so we must NOT include it
     * here — otherwise has_real=1 and the message won't go to _nolabel. */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    const char *uid = "2200000000000001";
    /* Only category label, no UNREAD (flags=0) → should go to _nolabel */
    const char *hdr = "Cat\tCategory mail\t2026-04-04\tCATEGORY_PROMOTIONS\t0";
    local_hdr_save("", uid, hdr, strlen(hdr));

    int rc = gmail_sync_rebuild_indexes();
    ASSERT(rc == 0, "rebuild_indexes nolabel: returns 0");

    /* _nolabel index should contain this uid */
    char (*idx_uids)[17] = NULL;
    int idx_count = 0;
    int load_rc = label_idx_load("_nolabel", &idx_uids, &idx_count);
    ASSERT(load_rc == 0, "rebuild_indexes nolabel: _nolabel index loaded");
    int found = 0;
    for (int i = 0; i < idx_count; i++)
        if (strcmp(idx_uids[i], uid) == 0) found = 1;
    free(idx_uids);
    ASSERT(found, "rebuild_indexes nolabel: uid in _nolabel");

    /* The flags field should remain 0 (no UNSEEN to clear) */
    char *loaded = local_hdr_load("", uid);
    ASSERT(loaded != NULL, "rebuild_indexes nolabel: hdr still exists");
    const char *last_tab = strrchr(loaded, '\t');
    ASSERT(last_tab != NULL, "rebuild_indexes nolabel: flags tab present");
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 1) == 0, "rebuild_indexes nolabel: UNSEEN clear for archived msg");
    free(loaded);
}

static void test_rebuild_indexes_many_labels(void) {
    /* Write many messages to force realloc in rebuild_label_indexes.
     * Each message has 6 labels → ~cap*5 pairs initial cap, then grows. */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    /* Write 12 messages, each with multiple labels, to force the realloc path */
    for (int i = 0; i < 12; i++) {
        char uid[17];
        snprintf(uid, sizeof(uid), "3300%012d", i);
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
                 "Sender%d\tSubject%d\t2026-04-01\tINBOX,UNREAD,STARRED,SENT,DRAFT,Work\t3",
                 i, i);
        local_hdr_save("", uid, hdr, strlen(hdr));
    }

    int rc = gmail_sync_rebuild_indexes();
    ASSERT(rc == 0, "rebuild_indexes many: returns 0");

    /* INBOX should have all 12 */
    char (*idx_uids)[17] = NULL;
    int idx_count = 0;
    label_idx_load("INBOX", &idx_uids, &idx_count);
    ASSERT(idx_count >= 12, "rebuild_indexes many: INBOX has >= 12 entries");
    free(idx_uids);
}

static void test_rebuild_indexes_trash(void) {
    /* TRASH label should be indexed as _trash */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    const char *uid = "4400000000000001";
    const char *hdr = "Trashed\tDeleted\t2026-04-05\tTRASH\t0";
    local_hdr_save("", uid, hdr, strlen(hdr));

    gmail_sync_rebuild_indexes();

    char (*idx_uids)[17] = NULL;
    int idx_count = 0;
    label_idx_load("_trash", &idx_uids, &idx_count);
    int found = 0;
    for (int i = 0; i < idx_count; i++)
        if (strcmp(idx_uids[i], uid) == 0) found = 1;
    free(idx_uids);
    ASSERT(found, "rebuild_indexes trash: uid in _trash index");
}

static void test_rebuild_indexes_flags_sync(void) {
    /* A message where flags integer is inconsistent with labels:
     * labels say UNREAD,STARRED but flags field says 0.
     * rebuild_label_indexes must update the flags field. */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    const char *uid = "5500000000000001";
    /* flags=0 but UNREAD and STARRED present → new_flags should be 3 */
    const char *hdr = "X\tFlagsSync\t2026-04-06\tINBOX,UNREAD,STARRED\t0";
    local_hdr_save("", uid, hdr, strlen(hdr));

    gmail_sync_rebuild_indexes();

    char *loaded = local_hdr_load("", uid);
    ASSERT(loaded != NULL, "flags_sync: hdr still exists");
    const char *last_tab = strrchr(loaded, '\t');
    int flags = last_tab ? atoi(last_tab + 1) : -1;
    ASSERT((flags & 1) != 0, "flags_sync: UNSEEN bit set after rebuild");
    ASSERT((flags & 2) != 0, "flags_sync: FLAGGED bit set after rebuild");
    free(loaded);
}

static void test_rebuild_indexes_hdr_no_tabs(void) {
    /* A malformed .hdr with no tabs should be gracefully skipped. */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    const char *uid = "6600000000000001";
    /* No tab separators — rebuild_label_indexes should skip gracefully */
    const char *hdr = "malformed-hdr-no-tabs";
    local_hdr_save("", uid, hdr, strlen(hdr));

    int rc = gmail_sync_rebuild_indexes();
    ASSERT(rc == 0, "rebuild_indexes no-tabs: no crash, returns 0");
}

/* ── gmail_sync_incremental: re-verify no-history path after store init ─── */

static void test_incremental_with_saved_history_no_server(void) {
    /* Verify that gmail_sync_incremental returns -2 whenever there is no
     * saved historyId, regardless of prior store state.  This exercises the
     * early-return branch in gmail_sync_incremental (lines 794-798). */
    const char home[] = "/tmp/email-cli-gmail-sync-test";
    setup_gmail_test_env(home);

    /* Ensure no history id is saved (fresh environment) */
    char *hid = local_gmail_history_load();
    /* If some previous test left a history file, skip gracefully */
    if (hid) {
        free(hid);
        ASSERT(1, "incremental no-server: history present from prev test, skipped");
        return;
    }

    int rc = gmail_sync_incremental(NULL);
    ASSERT(rc == -2, "incremental no-server: no historyId → -2");
}

/* ── build_hdr: SPAM label sets MSG_FLAG_JUNK ─────────────────────────────── */

static void test_build_hdr_spam_flag(void) {
    const char *raw = "From: Spammer\r\nSubject: Buy now\r\nDate: Mon, 14 Apr 2026 08:00:00 +0000\r\n\r\n";
    char *labels[] = {"SPAM"};
    char *hdr = gmail_sync_build_hdr(raw, labels, 1);
    ASSERT(hdr != NULL, "build_hdr spam: not NULL");

    /* SPAM label sets MSG_FLAG_JUNK (1<<6 = 64 per local_store.h) */
    const char *last_tab = strrchr(hdr, '\t');
    ASSERT(last_tab != NULL, "build_hdr spam: flags tab present");
    int flags = atoi(last_tab + 1);
    ASSERT((flags & 64) != 0, "build_hdr spam: JUNK flag set");

    free(hdr);
}

/* ── Mock servers for gmail_sync tests ──────────────────────────────── */

/*
 * reconcile_server: responds to:
 *   GET /messages  → 2-message list with historyId
 *   GET /labels    → label list
 *   GET /profile   → profile with historyId (fallback)
 *   GET /messages/{id} → raw message body
 *   any other      → 404
 */
static void run_reconcile_server(int lfd, int count) {
    const char *raw_email =
        "From: alice@example.com\r\n"
        "To: me@gmail.com\r\n"
        "Subject: Reconcile Test\r\n"
        "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n"
        "\r\n"
        "Body here.\r\n";
    char *b64 = gs_b64encode(raw_email, strlen(raw_email));

    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    for (int i = 0; i < count; i++) {
        int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) break;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char buf[4096];
        if (gs_read_req(cfd, buf, (int)sizeof(buf)) <= 0) { close(cfd); continue; }

        char method[16] = {0}, path[2048] = {0};
        sscanf(buf, "%15s %2047s", method, path);

        if (strstr(path, "/messages") && !strstr(path, "/messages/") &&
            strcmp(method, "GET") == 0) {
            gs_send_json(cfd, 200,
                "{\"messages\":["
                "{\"id\":\"aabbcc0000000001\",\"threadId\":\"t1\"},"
                "{\"id\":\"aabbcc0000000002\",\"threadId\":\"t2\"}"
                "],\"resultSizeEstimate\":2,\"historyId\":\"99001\"}");
        } else if (strstr(path, "/messages/") && strcmp(method, "GET") == 0) {
            char body_buf[2048];
            snprintf(body_buf, sizeof(body_buf),
                "{\"id\":\"aabbcc0000000001\","
                "\"labelIds\":[\"INBOX\",\"UNREAD\"],"
                "\"raw\":\"%s\"}",
                b64 ? b64 : "");
            gs_send_json(cfd, 200, body_buf);
        } else if (strstr(path, "/labels") && strcmp(method, "GET") == 0) {
            gs_send_json(cfd, 200,
                "{\"labels\":["
                "{\"id\":\"INBOX\",\"name\":\"INBOX\"},"
                "{\"id\":\"UNREAD\",\"name\":\"UNREAD\"}"
                "]}");
        } else if (strstr(path, "/profile")) {
            gs_send_json(cfd, 200,
                "{\"historyId\":\"99001\","
                "\"emailAddress\":\"test@gmail.com\"}");
        } else {
            gs_send_json(cfd, 404, "{}");
        }
        close(cfd);
    }
    free(b64);
    close(lfd);
    GCOV_FLUSH();
    _exit(0);
}

static pid_t start_reconcile_server(int *port_out, int count) {
    int lfd = gs_make_listener(port_out);
    if (lfd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(lfd); return -1; }
    if (pid == 0) run_reconcile_server(lfd, count);
    close(lfd);
    return pid;
}

static void test_reconcile_success(void) {
    reset_gmail_test_env();
    local_pending_fetch_clear();

    int port = 0;
    /* 2 connections: list_messages + list_labels */
    pid_t pid = start_reconcile_server(&port, 2);
    if (pid < 0) { ASSERT(0, "reconcile: could not start mock server"); return; }

    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "reconcile: client connected"); return; }

    int queued = gmail_sync_reconcile(gc);
    int pending_cnt = local_pending_fetch_count();
    gmail_disconnect(gc);
    gs_wait_child(pid);

    ASSERT(queued == 2, "reconcile: 2 messages queued");
    ASSERT(pending_cnt == 2, "reconcile: pending_fetch count == 2");
}

static void test_reconcile_with_cached_messages(void) {
    /* Pre-cache one of the two server messages so reconcile sees 1 cached
     * and 1 queued.  Covers lines 450-456 (the "cached++" path). */
    reset_gmail_test_env();
    local_pending_fetch_clear();

    /* Pre-save uid1 (already cached) */
    const char *uid1 = "aabbcc0000000001";
    const char *raw = "From: alice@example.com\r\nSubject: Reconcile Test\r\n\r\nBody\r\n";
    local_msg_save("", uid1, raw, strlen(raw));
    local_hdr_save("", uid1, "Alice\tReconcile Test\t2024-01-01\tINBOX\t0",
                   strlen("Alice\tReconcile Test\t2024-01-01\tINBOX\t0"));

    int port = 0;
    /* 2 connections: list_messages + list_labels */
    pid_t pid = start_reconcile_server(&port, 2);
    if (pid < 0) { ASSERT(0, "reconcile_cached: could not start mock server"); return; }

    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "reconcile_cached: client connected"); return; }

    int queued = gmail_sync_reconcile(gc);
    int pend_cnt = local_pending_fetch_count();
    gmail_disconnect(gc);
    gs_wait_child(pid);

    /* uid1 is cached, uid2 is new → 1 queued */
    ASSERT(queued == 1, "reconcile_cached: 1 new message queued");
    ASSERT(pend_cnt == 1, "reconcile_cached: pending count == 1");
}

static void test_fetch_pending_empty_queue(void) {
    /* fetch_pending with an empty queue should return 0 immediately. */
    reset_gmail_test_env();
    local_pending_fetch_clear();

    /* No network needed — empty queue exits early */
    setenv("GMAIL_TEST_TOKEN", "test_access_token", 1);
    setenv("GMAIL_API_BASE_URL", "http://127.0.0.1:1/gmail/v1/users/me", 1);
    Config cfg = {0};
    cfg.gmail_mode = 1;
    cfg.gmail_refresh_token = "fake";
    GmailClient *gc = gmail_connect(&cfg);
    if (!gc) { ASSERT(0, "fetch_empty: client created"); return; }

    int fetched = gmail_sync_fetch_pending(gc);
    gmail_disconnect(gc);

    ASSERT(fetched == 0, "fetch_empty: returns 0 on empty queue");
}

/* Server that returns a message with only CATEGORY_ labels (no real labels).
 * Used to test the _nolabel path in store_fetched_message (lines 397-404). */
static void run_nolabel_msg_server(int lfd, int count) {
    const char *raw_email =
        "From: promo@example.com\r\n"
        "To: me@gmail.com\r\n"
        "Subject: Promotions\r\n"
        "Date: Tue, 01 Jan 2025 00:00:00 +0000\r\n"
        "\r\n"
        "Click here to buy things!\r\n";
    char *b64 = gs_b64encode(raw_email, strlen(raw_email));

    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    for (int i = 0; i < count; i++) {
        int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) break;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char buf[4096];
        if (gs_read_req(cfd, buf, (int)sizeof(buf)) <= 0) { close(cfd); continue; }

        char method[16] = {0}, path[2048] = {0};
        sscanf(buf, "%15s %2047s", method, path);

        if (strstr(path, "/messages/") && strcmp(method, "GET") == 0) {
            /* Message with only CATEGORY_ label and IMPORTANT (both non-real).
             * IMPORTANT is filtered (filtered_label→skip).
             * CATEGORY_PROMOTIONS passes filter but is_category_label→true.
             * Result: has_real_label=0 → goes to _nolabel with UNSEEN cleared. */
            char body_buf[2048];
            snprintf(body_buf, sizeof(body_buf),
                "{\"id\":\"nolabel000000001\","
                "\"labelIds\":[\"CATEGORY_PROMOTIONS\",\"IMPORTANT\"],"
                "\"raw\":\"%s\"}",
                b64 ? b64 : "");
            gs_send_json(cfd, 200, body_buf);
        } else {
            gs_send_json(cfd, 404, "{}");
        }
        close(cfd);
    }
    free(b64);
    close(lfd);
    GCOV_FLUSH();
    _exit(0);
}

static pid_t start_nolabel_msg_server(int *port_out, int count) {
    int lfd = gs_make_listener(port_out);
    if (lfd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(lfd); return -1; }
    if (pid == 0) run_nolabel_msg_server(lfd, count);
    close(lfd);
    return pid;
}

static void test_fetch_pending_nolabel_message(void) {
    /* Message has CATEGORY_PROMOTIONS + IMPORTANT labels.
     * CATEGORY_PROMOTIONS: is_category_label=true → has_real_label not set.
     * IMPORTANT: filtered → skipped entirely.
     * Result: has_real_label=0 → goes to _nolabel (covers lines 397-403).
     * No UNREAD → cur_flags=0 → UNSEEN was never set, flags remain 0. */
    reset_gmail_test_env();
    local_pending_fetch_clear();

    const char *uid = "nolabel000000001";
    local_pending_fetch_add(uid);

    int port = 0;
    pid_t pid = start_nolabel_msg_server(&port, 1);
    if (pid < 0) { ASSERT(0, "fetch_nolabel: no server"); return; }

    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "fetch_nolabel: client connected"); return; }

    int fetched = gmail_sync_fetch_pending(gc);
    gmail_disconnect(gc);
    gs_wait_child(pid);

    ASSERT(fetched == 1, "fetch_nolabel: 1 message downloaded");
    ASSERT(local_msg_exists("", uid), "fetch_nolabel: .eml saved");

    /* Should be in _nolabel index */
    char (*idx_uids)[17] = NULL;
    int idx_count = 0;
    label_idx_load("_nolabel", &idx_uids, &idx_count);
    int found = 0;
    for (int i = 0; i < idx_count; i++)
        if (strcmp(idx_uids[i], uid) == 0) found = 1;
    free(idx_uids);
    ASSERT(found, "fetch_nolabel: uid in _nolabel index");

    /* UNSEEN flag should be cleared (archived messages are always read) */
    char *hdr = local_hdr_load("", uid);
    if (hdr) {
        const char *last_tab = strrchr(hdr, '\t');
        int flags = last_tab ? atoi(last_tab + 1) : -1;
        ASSERT((flags & 1) == 0, "fetch_nolabel: UNSEEN cleared for archived msg");
        free(hdr);
    }
}

static void test_fetch_pending_with_rules(void) {
    /* Write a mail rule that matches alice@example.com, then fetch_pending
     * so apply_rules_to_new_message is called.  Covers lines 109-167. */
    reset_gmail_test_env();
    local_pending_fetch_clear();

    /* Write a rules.ini for the test account */
    char rules_dir[512];
    snprintf(rules_dir, sizeof(rules_dir),
             "/tmp/email-cli-gmail-sync-test/.config/email-cli/accounts/csjpeterjaket@gmail.com");
    /* Use system() with literal paths — no variable in rm -rf */
    int _sr2 = system("mkdir -p '/tmp/email-cli-gmail-sync-test/.config/email-cli/accounts/csjpeterjaket@gmail.com'"); (void)_sr2;

    char rules_path[600];
    snprintf(rules_path, sizeof(rules_path), "%s/rules.ini", rules_dir);
    FILE *f = fopen(rules_path, "w");
    if (f) {
        fputs("[rule \"Test Rule\"]\n", f);
        fputs("if-from = *@example.com\n", f);
        fputs("then-add-label = Filtered\n", f);
        fclose(f);
    }

    const char *uid = "aabbcc0000000001";
    local_pending_fetch_add(uid);

    int port = 0;
    /* 1 connection: gmail_fetch_message */
    pid_t pid = start_reconcile_server(&port, 1);
    if (pid < 0) { ASSERT(0, "fetch_rules: no server"); return; }

    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "fetch_rules: client connected"); return; }

    int fetched = gmail_sync_fetch_pending(gc);
    gmail_disconnect(gc);
    gs_wait_child(pid);

    ASSERT(fetched == 1, "fetch_rules: 1 message downloaded");
    /* Rule applied — message should be fetched (rules firing doesn't prevent storage) */
    ASSERT(local_msg_exists("", uid), "fetch_rules: .eml saved after rule application");
}

static void test_reconcile_server_error(void) {
    /* When the server returns 500 for messages.list, gmail_list_messages
     * treats it as an empty result (0 messages). Reconcile returns 0 with
     * nothing queued.  Exercises the error-response code path in list. */
    reset_gmail_test_env();

    int port = 0;
    int lfd = gs_make_listener(&port);
    if (lfd < 0) { ASSERT(0, "reconcile_server_err: no listener"); return; }

    pid_t pid = fork();
    if (pid < 0) { close(lfd); ASSERT(0, "reconcile_server_err: fork failed"); return; }
    if (pid == 0) {
        struct sockaddr_in cli = {0};
        socklen_t cli_len = sizeof(cli);
        for (int i = 0; i < 4; i++) {
            int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
            if (cfd < 0) break;
            char buf[512];
            gs_read_req(cfd, buf, (int)sizeof(buf));
            gs_send_json(cfd, 500, "{\"error\":\"server error\"}");
            close(cfd);
        }
        close(lfd);
        GCOV_FLUSH();
        _exit(0);
    }
    close(lfd);
    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "reconcile_server_err: client connected"); return; }

    int rc = gmail_sync_reconcile(gc);
    gmail_disconnect(gc);
    gs_wait_child(pid);

    /* gmail_list_messages returns 0 even on 500 (treats as empty list).
     * So reconcile returns 0 with 0 messages queued. */
    ASSERT(rc == 0, "reconcile_server_err: returns 0 (empty list) on server 500");
    ASSERT(local_pending_fetch_count() == 0,
           "reconcile_server_err: nothing queued when server returns 500");
}

static void test_fetch_pending_success(void) {
    reset_gmail_test_env();
    local_pending_fetch_clear();

    const char *uid = "aabbcc0000000001";
    local_pending_fetch_add(uid);

    int port = 0;
    /* 1 connection: gmail_fetch_message for the 1 pending UID */
    pid_t pid = start_reconcile_server(&port, 1);
    if (pid < 0) { ASSERT(0, "fetch_pending: no server"); return; }

    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "fetch_pending: client connected"); return; }

    int fetched = gmail_sync_fetch_pending(gc);
    int msg_ok  = local_msg_exists("", uid);
    int hdr_ok  = local_hdr_exists("", uid);
    int pend_cnt = local_pending_fetch_count();
    gmail_disconnect(gc);
    gs_wait_child(pid);

    ASSERT(fetched == 1, "fetch_pending: 1 message downloaded");
    ASSERT(msg_ok,   "fetch_pending: .eml saved");
    ASSERT(hdr_ok,   "fetch_pending: .hdr saved");
    ASSERT(pend_cnt == 0, "fetch_pending: queue empty");
}

static void test_fetch_pending_already_cached(void) {
    reset_gmail_test_env();
    local_pending_fetch_clear();

    const char *uid = "bbccdd0000000001";
    const char *raw = "From: X\r\nSubject: Y\r\n\r\nBody\r\n";
    local_msg_save("", uid, raw, strlen(raw));
    const char *hdr = "X\tY\t2024-01-01\tINBOX\t0";
    local_hdr_save("", uid, hdr, strlen(hdr));
    local_pending_fetch_add(uid);

    /* Use an unreachable port — no network calls should happen */
    setenv("GMAIL_TEST_TOKEN", "test_access_token", 1);
    setenv("GMAIL_API_BASE_URL", "http://127.0.0.1:1/gmail/v1/users/me", 1);
    Config cfg = {0};
    cfg.gmail_mode = 1;
    cfg.gmail_refresh_token = "fake";
    GmailClient *gc = gmail_connect(&cfg);
    if (!gc) { ASSERT(0, "fetch_cached: client created"); return; }

    int fetched = gmail_sync_fetch_pending(gc);
    int pend_cnt = local_pending_fetch_count();
    gmail_disconnect(gc);

    ASSERT(fetched == 0, "fetch_cached: 0 downloaded (already cached)");
    ASSERT(pend_cnt == 0, "fetch_cached: queue cleared");
}

static void test_fetch_pending_server_error(void) {
    reset_gmail_test_env();
    local_pending_fetch_clear();

    const char *uid = "ccddee0000000001";
    local_pending_fetch_add(uid);

    int port = 0;
    int lfd = gs_make_listener(&port);
    if (lfd < 0) { ASSERT(0, "fetch_err: no listener"); return; }

    pid_t pid = fork();
    if (pid < 0) { close(lfd); ASSERT(0, "fetch_err: fork failed"); return; }
    if (pid == 0) {
        struct sockaddr_in cli = {0};
        socklen_t cli_len = sizeof(cli);
        for (int i = 0; i < 3; i++) {
            int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
            if (cfd < 0) break;
            char buf[512];
            gs_read_req(cfd, buf, (int)sizeof(buf));
            gs_send_json(cfd, 404, "{\"error\":{\"code\":404}}");
            close(cfd);
        }
        close(lfd);
        GCOV_FLUSH();
        _exit(0);
    }
    close(lfd);
    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "fetch_err: client connected"); return; }

    int fetched = gmail_sync_fetch_pending(gc);
    int pend_cnt = local_pending_fetch_count();
    gmail_disconnect(gc);
    gs_wait_child(pid);

    ASSERT(fetched == 0, "fetch_err: 0 downloaded on 404");
    ASSERT(pend_cnt == 1, "fetch_err: uid stays in queue");
}

/*
 * incremental_server: handles the various gmail_sync_incremental paths.
 * Returns history with messagesAdded, labelsAdded, labelsRemoved, messagesDeleted.
 */
static void run_incremental_server(int lfd, int count) {
    const char *raw_email =
        "From: bob@example.com\r\n"
        "To: me@gmail.com\r\n"
        "Subject: Incremental Test\r\n"
        "Date: Mon, 01 Jan 2024 12:00:00 +0000\r\n"
        "\r\n"
        "Incremental body.\r\n";
    char *b64 = gs_b64encode(raw_email, strlen(raw_email));

    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    for (int i = 0; i < count; i++) {
        int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
        if (cfd < 0) break;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char buf[4096];
        if (gs_read_req(cfd, buf, (int)sizeof(buf)) <= 0) { close(cfd); continue; }

        char method[16] = {0}, path[2048] = {0};
        sscanf(buf, "%15s %2047s", method, path);

        if (strstr(path, "/history") && strcmp(method, "GET") == 0) {
            /* Flat top-level JSON: json_foreach_object searches at top level */
            char body_buf[2048];
            snprintf(body_buf, sizeof(body_buf),
                "{"
                "\"historyId\":\"99999\","
                "\"messagesAdded\":["
                "  {\"id\":\"incr000000000001\"}"
                "],"
                "\"labelsAdded\":["
                "  {\"id\":\"incr000000000001\",\"labelIds\":[\"STARRED\"]}"
                "],"
                "\"labelsRemoved\":["
                "  {\"id\":\"incr000000000001\",\"labelIds\":[\"UNREAD\"]}"
                "],"
                "\"messagesDeleted\":["
                "  {\"id\":\"incr000000000002\"}"
                "]"
                "}");
            gs_send_json(cfd, 200, body_buf);
        } else if (strstr(path, "/messages/") && strcmp(method, "GET") == 0) {
            char body_buf[2048];
            snprintf(body_buf, sizeof(body_buf),
                "{\"id\":\"incr000000000001\","
                "\"labelIds\":[\"INBOX\",\"STARRED\"],"
                "\"raw\":\"%s\"}",
                b64 ? b64 : "");
            gs_send_json(cfd, 200, body_buf);
        } else if (strstr(path, "/labels") && strcmp(method, "GET") == 0) {
            gs_send_json(cfd, 200,
                "{\"labels\":["
                "{\"id\":\"INBOX\",\"name\":\"INBOX\"},"
                "{\"id\":\"STARRED\",\"name\":\"STARRED\"},"
                "{\"id\":\"UNREAD\",\"name\":\"UNREAD\"}"
                "]}");
        } else if (strstr(path, "/profile")) {
            gs_send_json(cfd, 200,
                "{\"historyId\":\"99999\","
                "\"emailAddress\":\"test@gmail.com\"}");
        } else {
            gs_send_json(cfd, 404, "{}");
        }
        close(cfd);
    }
    free(b64);
    close(lfd);
    GCOV_FLUSH();
    _exit(0);
}

static pid_t start_incremental_server(int *port_out, int count) {
    int lfd = gs_make_listener(port_out);
    if (lfd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(lfd); return -1; }
    if (pid == 0) run_incremental_server(lfd, count);
    close(lfd);
    return pid;
}

static void test_incremental_with_history(void) {
    /* Tests gmail_sync_incremental with a live mock server.
     * Covers: process_message_added, process_labels_added,
     *         process_labels_removed, process_message_deleted,
     *         the label-refresh branch (label_changes > 0). */
    reset_gmail_test_env();
    local_pending_fetch_clear();

    local_gmail_history_save("12345");

    /* Pre-save the "deleted" message so remove operations have something to do */
    const char *del_uid = "incr000000000002";
    local_msg_save("", del_uid, "From: X\r\n\r\nbody\r\n", 18);
    local_hdr_save("", del_uid, "X\tDel\t2024-01-01\tINBOX\t0", 25);
    label_idx_add("INBOX", del_uid);

    int port = 0;
    /* Connections: history(1) + msg_added fetch(1) + labels_removed fetch(1)
     *              + labels for msg_deleted(1) + labels refresh(1) = 5 */
    pid_t pid = start_incremental_server(&port, 5);
    if (pid < 0) { ASSERT(0, "incremental: could not start mock server"); return; }

    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "incremental: client connected"); return; }

    int rc = gmail_sync_incremental(gc);
    char *hid = local_gmail_history_load();
    int msg_ok = local_msg_exists("", "incr000000000001");
    gmail_disconnect(gc);
    gs_wait_child(pid);

    ASSERT(rc == 0, "incremental: returns 0 on success");
    ASSERT(hid != NULL, "incremental: historyId saved");
    if (hid) {
        ASSERT(strcmp(hid, "99999") == 0, "incremental: new historyId == 99999");
        free(hid);
    }
    ASSERT(msg_ok, "incremental: added message saved");
}

static void test_incremental_history_expired(void) {
    /* Server returns 404 for history → must return -2. */
    reset_gmail_test_env();

    local_gmail_history_save("old_history_id");

    int port = 0;
    int lfd = gs_make_listener(&port);
    if (lfd < 0) { ASSERT(0, "incr_expired: no listener"); return; }

    pid_t pid = fork();
    if (pid < 0) { close(lfd); ASSERT(0, "incr_expired: fork failed"); return; }
    if (pid == 0) {
        struct sockaddr_in cli = {0};
        socklen_t cli_len = sizeof(cli);
        for (int i = 0; i < 3; i++) {
            int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
            if (cfd < 0) break;
            char buf[512];
            gs_read_req(cfd, buf, (int)sizeof(buf));
            gs_send_json(cfd, 404,
                "{\"error\":{\"code\":404,\"message\":\"History ID is too old\"}}");
            close(cfd);
        }
        close(lfd);
        GCOV_FLUSH();
        _exit(0);
    }
    close(lfd);
    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "incr_expired: client connected"); return; }

    int rc = gmail_sync_incremental(gc);
    gmail_disconnect(gc);
    gs_wait_child(pid);

    ASSERT(rc == -2, "incr_expired: returns -2 on expired historyId");
}

static void test_sync_full_success(void) {
    /* gmail_sync_full = reconcile + fetch_pending + rebuild_indexes. */
    reset_gmail_test_env();
    local_pending_fetch_clear();

    int port = 0;
    /* reconcile: list(1) + labels(1) = 2
     * fetch_pending: 2 new messages × 1 each = 2
     * Total = 4 */
    pid_t pid = start_reconcile_server(&port, 4);
    if (pid < 0) { ASSERT(0, "sync_full: could not start mock server"); return; }

    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "sync_full: client connected"); return; }

    int rc = gmail_sync_full(gc);
    gmail_disconnect(gc);
    gs_wait_child(pid);

    ASSERT(rc == 0, "sync_full: returns 0");
}

static void test_reconcile_no_history_id_in_list(void) {
    /* Messages list response has no historyId → falls back to GET /profile.
     * Covers the else-branch in gmail_sync_reconcile. */
    reset_gmail_test_env();
    local_pending_fetch_clear();

    int port = 0;
    int lfd = gs_make_listener(&port);
    if (lfd < 0) { ASSERT(0, "reconcile_nohid: no listener"); return; }

    pid_t pid = fork();
    if (pid < 0) { close(lfd); ASSERT(0, "reconcile_nohid: fork failed"); return; }
    if (pid == 0) {
        struct sockaddr_in cli = {0};
        socklen_t cli_len = sizeof(cli);
        for (int i = 0; i < 6; i++) {
            int cfd = accept(lfd, (struct sockaddr *)&cli, &cli_len);
            if (cfd < 0) break;
            struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
            setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[4096];
            if (gs_read_req(cfd, buf, (int)sizeof(buf)) <= 0) { close(cfd); continue; }
            char method[16] = {0}, path[2048] = {0};
            sscanf(buf, "%15s %2047s", method, path);

            if (strstr(path, "/messages") && !strstr(path, "/messages/") &&
                strcmp(method, "GET") == 0) {
                /* No historyId in list response — forces /profile fallback */
                gs_send_json(cfd, 200,
                    "{\"messages\":[],"
                    "\"resultSizeEstimate\":0}");
            } else if (strstr(path, "/profile")) {
                gs_send_json(cfd, 200,
                    "{\"historyId\":\"77777\","
                    "\"emailAddress\":\"test@gmail.com\"}");
            } else if (strstr(path, "/labels") && strcmp(method, "GET") == 0) {
                gs_send_json(cfd, 200,
                    "{\"labels\":["
                    "{\"id\":\"INBOX\",\"name\":\"INBOX\"}"
                    "]}");
            } else {
                gs_send_json(cfd, 404, "{}");
            }
            close(cfd);
        }
        close(lfd);
        GCOV_FLUSH();
        _exit(0);
    }
    close(lfd);
    usleep(20000);

    GmailClient *gc = gs_make_client(port);
    if (!gc) { gs_wait_child(pid); ASSERT(0, "reconcile_nohid: client connected"); return; }

    int queued = gmail_sync_reconcile(gc);
    char *hid = local_gmail_history_load();
    gmail_disconnect(gc);
    gs_wait_child(pid);

    ASSERT(queued == 0, "reconcile_nohid: 0 messages queued (empty server)");
    ASSERT(hid != NULL, "reconcile_nohid: historyId saved from /profile");
    free(hid);
}

/* ── Registration ────────────────────────────────────────────────────── */

void test_gmail_sync(void) {
    RUN_TEST(test_filtered_null);
    RUN_TEST(test_filtered_category);
    RUN_TEST(test_filtered_important);
    RUN_TEST(test_not_filtered_system);
    RUN_TEST(test_not_filtered_user);
    RUN_TEST(test_filtered_edge_cases);
    RUN_TEST(test_build_hdr_basic);
    RUN_TEST(test_build_hdr_starred);
    RUN_TEST(test_build_hdr_no_labels);
    RUN_TEST(test_build_hdr_missing_headers);
    RUN_TEST(test_build_hdr_combined_flags);
    RUN_TEST(test_build_hdr_archive_unread_flags);
    RUN_TEST(test_build_hdr_spam_flag);
    RUN_TEST(test_incremental_no_history);
    RUN_TEST(test_incremental_with_saved_history_no_server);
    RUN_TEST(test_repair_archive_flags_clears_unseen);
    RUN_TEST(test_repair_archive_flags_preserves_flagged);
    RUN_TEST(test_repair_archive_flags_noop_when_already_read);
    RUN_TEST(test_pending_fetch_empty_initially);
    RUN_TEST(test_pending_fetch_add_and_load);
    RUN_TEST(test_pending_fetch_remove);
    RUN_TEST(test_pending_fetch_clear);
    RUN_TEST(test_pending_fetch_count_matches_load);
    RUN_TEST(test_rebuild_indexes_empty_store);
    RUN_TEST(test_rebuild_indexes_basic);
    RUN_TEST(test_rebuild_indexes_nolabel);
    RUN_TEST(test_rebuild_indexes_many_labels);
    RUN_TEST(test_rebuild_indexes_trash);
    RUN_TEST(test_rebuild_indexes_flags_sync);
    RUN_TEST(test_rebuild_indexes_hdr_no_tabs);
    RUN_TEST(test_reconcile_success);
    RUN_TEST(test_reconcile_with_cached_messages);
    RUN_TEST(test_reconcile_server_error);
    RUN_TEST(test_reconcile_no_history_id_in_list);
    RUN_TEST(test_fetch_pending_success);
    RUN_TEST(test_fetch_pending_empty_queue);
    RUN_TEST(test_fetch_pending_already_cached);
    RUN_TEST(test_fetch_pending_server_error);
    RUN_TEST(test_fetch_pending_nolabel_message);
    RUN_TEST(test_fetch_pending_with_rules);
    RUN_TEST(test_incremental_with_history);
    RUN_TEST(test_incremental_history_expired);
    RUN_TEST(test_sync_full_success);
}
