/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-22 */
/* Unit tests of the operation history: recording, reverting, refusals. */
#include "test_helpers.h"
#include "test_scratch.h"
#include "op_history.h"
#include "history_service.h"
#include "config_store.h"
#include "mail_rules.h"
#include "local_store.h"
#include "email_service.h"
#include "history_view.h"
#include "pty_stdio.h"
#include "platform/terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Fixture ─────────────────────────────────────────────────────────── */

#define IMAP_USER "alice@example.org"
#define GMAIL_USER "gm@example.org"

static Config g_imap, g_gmail;

/** The calls the fake server received. */
static struct {
    char last_op[32];
    char msgid[128], from[128], to[128];
    char uid[64], label[128];
    int add, create, calls;
    int refuse;          /**< 1: the fake server refuses, in its own words */
} g_srv;

static int fake_move_back(const Config *cfg, const char *msgid, const char *from,
                          const char *to, char *refusal, size_t size) {
    (void)cfg;
    snprintf(g_srv.last_op, sizeof(g_srv.last_op), "move_back");
    snprintf(g_srv.msgid, sizeof(g_srv.msgid), "%s", msgid);
    snprintf(g_srv.from, sizeof(g_srv.from), "%s", from);
    snprintf(g_srv.to, sizeof(g_srv.to), "%s", to);
    g_srv.calls++;
    if (g_srv.refuse) {
        snprintf(refusal, size, "NO [CANNOT] Mailbox is locked by the server");
        return -1;
    }
    return 0;
}

static int fake_label(const Config *cfg, const char *uid, const char *label, int add) {
    (void)cfg;
    snprintf(g_srv.last_op, sizeof(g_srv.last_op), "label");
    snprintf(g_srv.uid, sizeof(g_srv.uid), "%s", uid);
    snprintf(g_srv.label, sizeof(g_srv.label), "%s", label);
    g_srv.add = add;
    g_srv.calls++;
    return g_srv.refuse ? -1 : 0;
}

static int fake_folder(const Config *cfg, const char *name, const char *id, int create) {
    (void)cfg; (void)id;
    snprintf(g_srv.last_op, sizeof(g_srv.last_op), "folder");
    snprintf(g_srv.label, sizeof(g_srv.label), "%s", name);
    g_srv.create = create;
    g_srv.calls++;
    return g_srv.refuse ? -1 : 0;
}

static const HistoryBackend g_fake = { fake_move_back, fake_label, fake_folder };

static int nsteps(void) {
    OpHistory h = {0};
    if (op_history_load(NULL, NULL, &h) != 0) return -1;
    int n = h.count;
    op_history_free(&h);
    return n;
}

static unsigned long last_id(void) {
    OpHistory h = {0};
    if (op_history_load(NULL, NULL, &h) != 0 || h.count == 0) return 0;
    unsigned long id = h.steps[h.count - 1].id;
    op_history_free(&h);
    return id;
}

/** Copies the fields of step @p id into the caller's buffers. */
static int step_fields(unsigned long id, char *op, char *old_v, char *new_v,
                       unsigned long *reverts, OpKind *kind) {
    OpHistory h = {0};
    if (op_history_load(NULL, NULL, &h) != 0) return -1;
    const OpStep *s = op_history_step(&h, id);
    if (!s) { op_history_free(&h); return -1; }
    if (op) snprintf(op, 64, "%s", s->op);
    if (old_v) snprintf(old_v, 512, "%s", s->old_value);
    if (new_v) snprintf(new_v, 512, "%s", s->new_value);
    if (reverts) *reverts = s->reverts;
    if (kind) *kind = s->kind;
    op_history_free(&h);
    return 0;
}

static void use(const Config *cfg) { local_store_init(cfg->host, cfg->user); }

static void put_flags(const char *folder, const char *uid, int flags) {
    Manifest *m = manifest_load(folder);
    if (!m) m = calloc(1, sizeof(Manifest));
    manifest_upsert(m, uid, strdup("a@example.org"), strdup("Subject"),
                    strdup("2026-10-22 10:00"), flags);
    manifest_save(folder, m);
    manifest_free(m);
}

static int flags_of(const char *folder, const char *uid) {
    Manifest *m = manifest_load(folder);
    if (!m) return -1;
    ManifestEntry *e = manifest_find(m, uid);
    int f = e ? e->flags : -1;
    manifest_free(m);
    return f;
}

static int g_ready;
static char g_old_env[4][4096];
static int  g_old_set[4];
static const char *const g_env_names[4] = { "HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
                                            "XDG_CACHE_HOME" };

static void setup(void) {
    if (g_ready) return;
    g_ready = 1;
    for (int i = 0; i < 4; i++) {
        const char *v = getenv(g_env_names[i]);
        g_old_set[i] = v != NULL;
        if (v) snprintf(g_old_env[i], sizeof(g_old_env[i]), "%s", v);
    }
    test_set_home(TP("history/home"));
    op_history_set_actor("alice", 0);
    history_set_backend(&g_fake);

    g_imap.host = "imaps://history.example.org";
    g_imap.user = IMAP_USER;
    g_imap.pass = "oldsecret-abc";
    config_save_account(&g_imap);
    g_gmail.host = "imaps://gmailhistory.example.org";
    g_gmail.user = GMAIL_USER;
    g_gmail.gmail_mode = 1;
    g_gmail.gmail_refresh_token = "refresh-token-fixture";
    config_save_account(&g_gmail);
}

/** Does, reverts, and reverts the revert; the caller checks the state. */
static int revert_ok(unsigned long id) {
    char refusal[512];
    int rc = history_revert(id, refusal, sizeof(refusal));
    if (rc != 0) printf("  refused: %s\n", refusal);
    return rc;
}

/* ── The store ───────────────────────────────────────────────────────── */

static void test_store_records(void) {
    setup();
    int before = nsteps();
    OpRecord r = { IMAP_USER, "flag", "mail INBOX/0000000000000001", "unread",
                   "yes", "no", "INBOX", OPH_REVERTABLE, 0 };
    long id = op_history_record(&r);
    ASSERT(id > 0, "a change is recorded");
    ASSERT(nsteps() == before + 1, "one step more");
    ASSERT(last_id() == (unsigned long)id, "the new step is the last");

    OpHistory h = {0};
    ASSERT(op_history_load(NULL, "mail INBOX/0000000000000001", &h) == 0, "load object");
    ASSERT(h.count == 1, "the history of one object has its step");
    ASSERT(strcmp(h.steps[0].who, "alice") == 0, "who");
    ASSERT(strlen(h.steps[0].time) == 20 && h.steps[0].time[10] == 'T', "when, ISO 8601 UTC");
    ASSERT(strcmp(h.steps[0].old_value, "yes") == 0 &&
           strcmp(h.steps[0].new_value, "no") == 0, "old and new value");
    op_history_free(&h);
}

static void test_store_nothing_changed(void) {
    setup();
    int before = nsteps();
    OpRecord r = { IMAP_USER, "flag", "mail INBOX/0000000000000002", "unread",
                   "yes", "yes", "INBOX", OPH_REVERTABLE, 0 };
    ASSERT(op_history_record(&r) == 0, "no change: nothing recorded");
    ASSERT(nsteps() == before, "the history did not grow");
}

static void test_store_escaping(void) {
    setup();
    OpRecord r = { IMAP_USER, "rule", "rule A\tB", "add", "", "line1\nline2\\x\r",
                   "", OPH_REVERTABLE, 0 };
    long id = op_history_record(&r);
    ASSERT(id > 0, "recorded");
    OpHistory h = {0};
    ASSERT(op_history_load(NULL, NULL, &h) == 0, "load");
    const OpStep *s = op_history_step(&h, (unsigned long)id);
    ASSERT(s && strcmp(s->object, "rule A\tB") == 0, "tab survives");
    ASSERT(strcmp(s->new_value, "line1\nline2\\x\r") == 0, "newline and backslash survive");
    op_history_free(&h);
}

static void test_store_file_mode(void) {
    setup();
    char path[1024];
    snprintf(path, sizeof(path), "%s/.local/share/email-cli/history.tsv", TP("history/home"));
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL, "the history file exists in the data directory");
    fclose(f);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "test \"$(stat -c %%a '%s')\" = 600", path);
    ASSERT(system(cmd) == 0, "the history file is private (0600)");
}

/* ── Flags (IMAP): revert and revert of the revert ───────────────────── */

static void test_revert_flag(void) {
    setup();
    use(&g_imap);
    put_flags("INBOX", "00000000000000a1", MSG_FLAG_UNSEEN);
    int before = nsteps();
    email_service_set_flag(&g_imap, "00000000000000a1", "INBOX", MSG_FLAG_UNSEEN, 0);
    ASSERT(!(flags_of("INBOX", "00000000000000a1") & MSG_FLAG_UNSEEN), "marked read");
    ASSERT(nsteps() == before + 1, "the change is a step");
    unsigned long first = last_id();

    ASSERT(revert_ok(first) == 0, "revert accepted");
    ASSERT(flags_of("INBOX", "00000000000000a1") & MSG_FLAG_UNSEEN, "the state is back");
    ASSERT(nsteps() == before + 2, "two steps in the history");
    unsigned long second = last_id();
    unsigned long reverts = 0;
    ASSERT(step_fields(second, NULL, NULL, NULL, &reverts, NULL) == 0, "step");
    ASSERT(reverts == first, "the revert names the step it reverts");

    ASSERT(revert_ok(second) == 0, "the revert can be reverted");
    ASSERT(!(flags_of("INBOX", "00000000000000a1") & MSG_FLAG_UNSEEN),
           "reverting the revert restores the change");
    ASSERT(nsteps() == before + 3, "three steps");
}

static void test_flag_noop_records_nothing(void) {
    setup();
    use(&g_imap);
    put_flags("INBOX", "00000000000000a2", 0);
    int before = nsteps();
    email_service_set_flag(&g_imap, "00000000000000a2", "INBOX", MSG_FLAG_FLAGGED, 0);
    ASSERT(nsteps() == before, "an operation that changes nothing records nothing");
}

static void test_revert_flag_overtaken(void) {
    setup();
    use(&g_imap);
    put_flags("INBOX", "00000000000000a3", MSG_FLAG_UNSEEN);
    email_service_set_flag(&g_imap, "00000000000000a3", "INBOX", MSG_FLAG_UNSEEN, 0);
    unsigned long first = last_id();
    email_service_set_flag(&g_imap, "00000000000000a3", "INBOX", MSG_FLAG_UNSEEN, 1);
    int before = nsteps();
    char refusal[512];
    ASSERT(history_revert(first, refusal, sizeof(refusal)) != 0, "an overtaken step is refused");
    ASSERT(strstr(refusal, "overtaken") != NULL, "the refusal says it was overtaken");
    ASSERT(nsteps() == before, "a refused revert records nothing");
}

static void test_revert_flag_local_store_lacks_mail(void) {
    setup();
    use(&g_imap);
    history_note_flag(&g_imap, "INBOX", "00000000000000ff", MSG_FLAG_UNSEEN, 1, 0);
    char refusal[512];
    ASSERT(history_revert(last_id(), refusal, sizeof(refusal)) != 0, "refused");
    ASSERT(strstr(refusal, "local store") != NULL,
           "the refusal says the local store lacks the mail");
}

static void test_non_admin_refused(void) {
    setup();
    use(&g_imap);
    put_flags("INBOX", "00000000000000a4", MSG_FLAG_UNSEEN);
    op_history_set_actor("bob", 0);
    email_service_set_flag(&g_imap, "00000000000000a4", "INBOX", MSG_FLAG_UNSEEN, 0);
    unsigned long id = last_id();
    op_history_set_actor("alice", 0);
    char refusal[512];
    int before = nsteps();
    ASSERT(history_revert(id, refusal, sizeof(refusal)) != 0,
           "a non-administrator is refused on another's step");
    ASSERT(refusal[0] != '\0', "with a reason");
    ASSERT(!(flags_of("INBOX", "00000000000000a4") & MSG_FLAG_UNSEEN), "nothing changed");
    ASSERT(nsteps() == before, "nothing recorded");

    op_history_set_actor("root", 1);
    ASSERT(revert_ok(id) == 0, "an administrator may revert another's step");
    ASSERT(flags_of("INBOX", "00000000000000a4") & MSG_FLAG_UNSEEN, "reverted");
    op_history_set_actor("alice", 0);
}

/* ── IMAP move, trash, archive ───────────────────────────────────────── */

static void imap_move_case(const char *op, const char *target) {
    setup();
    use(&g_imap);
    memset(&g_srv, 0, sizeof(g_srv));
    int before = nsteps();
    history_note_move(&g_imap, op, "INBOX", "00000000000000b1", target, "<m1@example.org>");
    ASSERT(nsteps() == before + 1, "the move is a step");
    unsigned long first = last_id();

    ASSERT(revert_ok(first) == 0, "revert accepted");
    ASSERT(strcmp(g_srv.last_op, "move_back") == 0, "the inverse is a move on the server");
    ASSERT(strcmp(g_srv.msgid, "<m1@example.org>") == 0, "found by Message-ID");
    ASSERT(strcmp(g_srv.from, target) == 0 && strcmp(g_srv.to, "INBOX") == 0, "moved back");
    ASSERT(nsteps() == before + 2, "two steps");

    ASSERT(revert_ok(last_id()) == 0, "the revert can be reverted");
    ASSERT(strcmp(g_srv.from, "INBOX") == 0 && strcmp(g_srv.to, target) == 0,
           "reverting the revert moves it again");
    ASSERT(nsteps() == before + 3, "three steps");
}

static void test_revert_imap_move(void)    { imap_move_case("move", "Projects"); }
static void test_revert_imap_trash(void)   { imap_move_case("trash", "Trash"); }
static void test_revert_imap_archive(void) { imap_move_case("archive", "Archive"); }

static void test_revert_imap_move_server_words(void) {
    setup();
    use(&g_imap);
    memset(&g_srv, 0, sizeof(g_srv));
    history_note_move(&g_imap, "move", "INBOX", "00000000000000b2", "Projects", "<m2@example.org>");
    g_srv.refuse = 1;
    char refusal[512];
    ASSERT(history_revert(last_id(), refusal, sizeof(refusal)) != 0, "refused");
    ASSERT(strstr(refusal, "Mailbox is locked by the server") != NULL,
           "the refusal is in the server's words");
    g_srv.refuse = 0;
}

static void test_revert_imap_move_without_message_id(void) {
    setup();
    use(&g_imap);
    history_note_move(&g_imap, "move", "INBOX", "00000000000000b3", "Projects", NULL);
    char refusal[512];
    ASSERT(history_revert(last_id(), refusal, sizeof(refusal)) != 0, "refused");
    ASSERT(strstr(refusal, "local store") != NULL,
           "the refusal says the local store had not the mail");
}

/* ── Junk ────────────────────────────────────────────────────────────── */

/* email_service_mark_junk() needs a server; the junk revert is exercised
 * with the state check only: an overtaken junk step is refused. */
static void test_revert_junk_overtaken(void) {
    setup();
    use(&g_imap);
    history_note_junk(&g_imap, "00000000000000c1", 1);
    unsigned long first = last_id();
    history_note_junk(&g_imap, "00000000000000c1", 0);
    char refusal[512];
    ASSERT(history_revert(first, refusal, sizeof(refusal)) != 0, "overtaken junk step refused");
    ASSERT(strstr(refusal, "overtaken") != NULL, "says overtaken");
}

/* ── Gmail: label, move, trash, archive ──────────────────────────────── */

static void gm_mail(const char *uid, const char *labels) {
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "From: a@example.org\r\nMessage-ID: <%s@example.org>\r\n\r\n", uid);
    local_hdr_save("", uid, hdr, strlen(hdr));
    char *copy = strdup(labels), *save = NULL;
    for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save))
        label_idx_add(t, uid);
    free(copy);
}

static int has(const char *label, const char *uid) { return label_idx_contains(label, uid) > 0; }

static void test_revert_gmail_label(void) {
    setup();
    use(&g_gmail);
    gm_mail("00000000000000d1", "INBOX,Work");
    memset(&g_srv, 0, sizeof(g_srv));
    int before = nsteps();
    label_idx_remove("Work", "00000000000000d1");
    history_note_label(&g_gmail, "00000000000000d1", "Work", 0);
    unsigned long first = last_id();

    ASSERT(revert_ok(first) == 0, "revert accepted");
    ASSERT(has("Work", "00000000000000d1"), "the label is back");
    ASSERT(strcmp(g_srv.label, "Work") == 0 && g_srv.add == 1, "the server got the label back");
    ASSERT(nsteps() == before + 2, "two steps");

    ASSERT(revert_ok(last_id()) == 0, "the revert can be reverted");
    ASSERT(!has("Work", "00000000000000d1"), "reverting the revert takes it off again");
    ASSERT(nsteps() == before + 3, "three steps");
}

static void test_revert_gmail_label_overtaken(void) {
    setup();
    use(&g_gmail);
    gm_mail("00000000000000d2", "INBOX");
    label_idx_add("Work", "00000000000000d2");
    history_note_label(&g_gmail, "00000000000000d2", "Work", 1);
    unsigned long first = last_id();
    label_idx_remove("Work", "00000000000000d2");
    history_note_label(&g_gmail, "00000000000000d2", "Work", 0);
    char refusal[512];
    ASSERT(history_revert(first, refusal, sizeof(refusal)) != 0, "refused");
    ASSERT(strstr(refusal, "overtaken") != NULL, "says overtaken");
}

static void test_revert_gmail_move(void) {
    setup();
    use(&g_gmail);
    gm_mail("00000000000000d3", "INBOX");
    memset(&g_srv, 0, sizeof(g_srv));
    int before = nsteps();
    label_idx_remove("INBOX", "00000000000000d3");
    label_idx_add("Work", "00000000000000d3");
    history_note_gmail_move(&g_gmail, "00000000000000d3", "INBOX", "Work");
    ASSERT(revert_ok(last_id()) == 0, "revert accepted");
    ASSERT(has("INBOX", "00000000000000d3") && !has("Work", "00000000000000d3"),
           "the mail is back in the source label");
    ASSERT(nsteps() == before + 2, "two steps");
    ASSERT(revert_ok(last_id()) == 0, "the revert can be reverted");
    ASSERT(!has("INBOX", "00000000000000d3") && has("Work", "00000000000000d3"),
           "reverting the revert moves it again");
}

static void test_revert_gmail_trash(void) {
    setup();
    use(&g_gmail);
    gm_mail("00000000000000d4", "INBOX,Work");
    memset(&g_srv, 0, sizeof(g_srv));
    int before = nsteps();
    label_idx_remove("INBOX", "00000000000000d4");
    label_idx_remove("Work", "00000000000000d4");
    label_idx_add("_trash", "00000000000000d4");
    history_note_gmail_trash(&g_gmail, "00000000000000d4", "INBOX,Work");

    ASSERT(revert_ok(last_id()) == 0, "revert accepted");
    ASSERT(has("INBOX", "00000000000000d4") && has("Work", "00000000000000d4") &&
           !has("_trash", "00000000000000d4"), "out of the trash, labels back");
    ASSERT(strcmp(g_srv.label, "TRASH") == 0 && g_srv.add == 0, "TRASH taken off on the server");
    ASSERT(nsteps() == before + 2, "two steps");

    ASSERT(revert_ok(last_id()) == 0, "the revert can be reverted");
    ASSERT(has("_trash", "00000000000000d4") && !has("INBOX", "00000000000000d4") &&
           !has("Work", "00000000000000d4"), "reverting the revert trashes it again");
    ASSERT(nsteps() == before + 3, "three steps");
}

static void test_revert_gmail_archive(void) {
    setup();
    use(&g_gmail);
    gm_mail("00000000000000d5", "INBOX,UNREAD");
    memset(&g_srv, 0, sizeof(g_srv));
    int before = nsteps();
    label_idx_remove("INBOX", "00000000000000d5");
    label_idx_remove("UNREAD", "00000000000000d5");
    label_idx_add("_nolabel", "00000000000000d5");
    history_note_gmail_archive(&g_gmail, "00000000000000d5", "INBOX", 1);

    ASSERT(revert_ok(last_id()) == 0, "revert accepted");
    ASSERT(has("INBOX", "00000000000000d5") && has("UNREAD", "00000000000000d5") &&
           !has("_nolabel", "00000000000000d5"), "unarchived: labels and unread back");
    ASSERT(nsteps() == before + 2, "two steps");

    ASSERT(revert_ok(last_id()) == 0, "the revert can be reverted");
    ASSERT(has("_nolabel", "00000000000000d5") && !has("INBOX", "00000000000000d5") &&
           !has("UNREAD", "00000000000000d5"), "reverting the revert archives it again");
    ASSERT(nsteps() == before + 3, "three steps");
}

static void test_revert_gmail_missing_mail(void) {
    setup();
    use(&g_gmail);
    history_note_label(&g_gmail, "00000000000000ee", "Work", 1);
    char refusal[512];
    ASSERT(history_revert(last_id(), refusal, sizeof(refusal)) != 0, "refused");
    ASSERT(strstr(refusal, "local store") != NULL, "the refusal says the local store lacks the mail");
}

/* ── Folders and labels ──────────────────────────────────────────────── */

static void test_revert_folder_create(void) {
    setup();
    use(&g_imap);
    memset(&g_srv, 0, sizeof(g_srv));
    int before = nsteps();
    history_note_folder(&g_imap, "Projects2", "", 1);
    ASSERT(revert_ok(last_id()) == 0, "revert accepted");
    ASSERT(strcmp(g_srv.last_op, "folder") == 0 && g_srv.create == 0, "the folder is deleted");
    ASSERT(nsteps() == before + 2, "two steps");
    ASSERT(revert_ok(last_id()) == 0, "the revert can be reverted");
    ASSERT(g_srv.create == 1, "reverting the revert creates it again");
    ASSERT(nsteps() == before + 3, "three steps");
}

/* ── Rules ───────────────────────────────────────────────────────────── */

static MailRules *one_rule(const char *name, const char *tag) {
    MailRules *r = calloc(1, sizeof(MailRules));
    r->rules = calloc(2, sizeof(MailRule));
    r->cap = 2;
    r->count = 1;
    r->rules[0].name = strdup(name);
    r->rules[0].then_add_label[0] = strdup(tag);
    r->rules[0].then_add_count = 1;
    return r;
}

static char *first_from(void) {
    MailRules *r = mail_rules_load(IMAP_USER);
    char *v = r && r->count && r->rules[0].then_add_count ? strdup(r->rules[0].then_add_label[0]) : NULL;
    mail_rules_free(r);
    return v;
}

static int rule_count(void) {
    MailRules *r = mail_rules_load(IMAP_USER);
    int n = r ? r->count : 0;
    mail_rules_free(r);
    return n;
}

static void test_revert_rule_add_edit_delete(void) {
    setup();
    int before = nsteps();

    /* add */
    MailRules *r = one_rule("Hist", "One");
    ASSERT(mail_rules_save(IMAP_USER, r) == 0, "save");
    mail_rules_free(r);
    ASSERT(nsteps() == before + 1, "the add is one step");
    unsigned long add = last_id();
    ASSERT(revert_ok(add) == 0, "revert of the add");
    ASSERT(rule_count() == 0, "the rule is gone");
    ASSERT(nsteps() == before + 2, "two steps");
    ASSERT(revert_ok(last_id()) == 0, "revert of the revert");
    ASSERT(rule_count() == 1, "the rule is back");

    /* edit */
    r = one_rule("Hist", "Two");
    ASSERT(mail_rules_save(IMAP_USER, r) == 0, "save edit");
    mail_rules_free(r);
    unsigned long edit = last_id();
    char op[64];
    ASSERT(step_fields(edit, op, NULL, NULL, NULL, NULL) == 0 && strcmp(op, "rule") == 0,
           "the edit is a rule step");
    ASSERT(revert_ok(edit) == 0, "revert of the edit");
    char *v = first_from();
    ASSERT(v && strcmp(v, "One") == 0, "the old value is back");
    free(v);
    ASSERT(revert_ok(last_id()) == 0, "revert of the revert");
    v = first_from();
    ASSERT(v && strcmp(v, "Two") == 0, "the edit is restored");
    free(v);

    /* delete */
    r = calloc(1, sizeof(MailRules));
    ASSERT(mail_rules_save(IMAP_USER, r) == 0, "save delete");
    mail_rules_free(r);
    ASSERT(rule_count() == 0, "deleted");
    unsigned long del = last_id();
    ASSERT(revert_ok(del) == 0, "revert of the delete");
    ASSERT(rule_count() == 1, "the rule is back");
    ASSERT(revert_ok(last_id()) == 0, "revert of the revert");
    ASSERT(rule_count() == 0, "the delete is restored");
}

static void test_revert_rule_overtaken(void) {
    setup();
    MailRules *r = one_rule("Hist", "Three");
    mail_rules_save(IMAP_USER, r);
    mail_rules_free(r);
    unsigned long edit1 = last_id();
    r = one_rule("Hist", "Four");
    mail_rules_save(IMAP_USER, r);
    mail_rules_free(r);
    char refusal[512];
    int before = nsteps();
    ASSERT(history_revert(edit1, refusal, sizeof(refusal)) != 0, "an overtaken rule step is refused");
    ASSERT(strstr(refusal, "changed after step") != NULL, "the reason is given");
    ASSERT(nsteps() == before, "nothing recorded");
}

/* ── Account fields, settings, secrets ───────────────────────────────── */

static void test_revert_account_field(void) {
    setup();
    int before = nsteps();
    Config *c = config_load_account(IMAP_USER);
    c->sync_interval = 7;
    config_save_account(c);
    config_free(c);
    ASSERT(nsteps() == before + 1, "one field, one step");
    unsigned long id = last_id();
    ASSERT(revert_ok(id) == 0, "revert accepted");
    c = config_load_account(IMAP_USER);
    ASSERT(c->sync_interval == 0, "the field is back");
    config_free(c);
    ASSERT(revert_ok(last_id()) == 0, "the revert can be reverted");
    c = config_load_account(IMAP_USER);
    ASSERT(c->sync_interval == 7, "reverting the revert restores the change");
    config_free(c);
    ASSERT(nsteps() == before + 3, "three steps");
}

static void test_account_saved_unchanged_records_nothing(void) {
    setup();
    int before = nsteps();
    Config *c = config_load_account(IMAP_USER);
    config_save_account(c);
    config_free(c);
    ASSERT(nsteps() == before, "saving an unchanged account records nothing");
}

static void test_revert_setting(void) {
    setup();
    int before = nsteps();
    int cur = app_settings_get_obfuscation();
    app_settings_set_obfuscation(!cur);
    ASSERT(nsteps() == before + 1, "the setting change is a step");
    ASSERT(revert_ok(last_id()) == 0, "revert accepted");
    ASSERT(app_settings_get_obfuscation() == cur, "the setting is back");
    ASSERT(revert_ok(last_id()) == 0, "the revert can be reverted");
    ASSERT(app_settings_get_obfuscation() == !cur, "reverting the revert restores the change");
    ASSERT(nsteps() == before + 3, "three steps");
    app_settings_set_obfuscation(cur);
}

static void test_account_field_overtaken_and_non_admin(void) {
    setup();
    Config *c = config_load_account(IMAP_USER);
    c->sync_interval = 11;
    config_save_account(c);
    unsigned long id = last_id();
    c->sync_interval = 12;
    config_save_account(c);
    config_free(c);
    char refusal[512];
    ASSERT(history_revert(id, refusal, sizeof(refusal)) != 0, "overtaken account step refused");
    ASSERT(strstr(refusal, "step") != NULL, "reason names the step");

    op_history_set_actor("bob", 0);
    c = config_load_account(IMAP_USER);
    c->sync_interval = 13;
    config_save_account(c);
    config_free(c);
    unsigned long bobs = last_id();
    op_history_set_actor("alice", 0);
    ASSERT(history_revert(bobs, refusal, sizeof(refusal)) != 0,
           "a non-administrator is refused on another's account step");
    op_history_set_actor("root", 1);
    ASSERT(history_revert(bobs, refusal, sizeof(refusal)) == 0, "an administrator may");
    op_history_set_actor("alice", 0);
}

static char *slurp_history(void) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.local/share/email-cli/history.tsv", TP("history/home"));
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = calloc(1, 1 << 20);
    size_t n = fread(buf, 1, (1 << 20) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void test_secret_recorded_as_changed(void) {
    setup();
    int before = nsteps();
    Config *c = config_load_account(IMAP_USER);
    free(c->pass);
    c->pass = strdup("brand-new-secret-xyz");
    config_save_account(c);
    config_free(c);
    ASSERT(nsteps() == before + 1, "a secret change is one step");
    char op[64], ov[512], nv[512];
    OpKind kind;
    ASSERT(step_fields(last_id(), op, ov, nv, NULL, &kind) == 0, "step");
    ASSERT(strcmp(ov, "changed") == 0 && strcmp(nv, "changed") == 0,
           "a secret is recorded as 'changed'");
    char *all = slurp_history();
    ASSERT(all != NULL, "history readable");
    ASSERT(strstr(all, "brand-new-secret-xyz") == NULL, "the new secret is not in the history");
    ASSERT(strstr(all, "oldsecret-abc") == NULL, "the old secret is not in the history");
    free(all);
    ASSERT(kind == OPH_IRREVERSIBLE, "a secret change cannot be reverted");
    char refusal[512];
    ASSERT(history_revert(last_id(), refusal, sizeof(refusal)) != 0, "the revert is refused");
}

/* ── Irreversible operations ─────────────────────────────────────────── */

static void test_irreversible_not_revertable(void) {
    setup();
    use(&g_imap);
    OpHistory h = {0};
    char refusal[512];

    history_note_delete(&g_imap, "Trash", "00000000000000e1", "<x@example.org>");
    unsigned long del = last_id();
    history_note_folder(&g_imap, "Gone", "", 0);
    unsigned long fol = last_id();
    op_history_record_send(IMAP_USER, "bob@example.org",
                           "From: a@example.org\r\nTo: bob@example.org\r\nSubject: Hi\r\n\r\nbody\r\n");
    unsigned long sent = last_id();
    OpRecord rm = { "tmp@example.org", "account", "account tmp@example.org", "remove",
                    "imaps://x", "", "", OPH_IRREVERSIBLE, 0 };
    op_history_record(&rm);
    unsigned long acc = last_id();

    ASSERT(history_load(NULL, NULL, &h) == 0, "load");
    unsigned long ids[4] = { del, fol, sent, acc };
    for (int i = 0; i < 4; i++) {
        const OpStep *s = op_history_step(&h, ids[i]);
        ASSERT(s != NULL, "step exists");
        ASSERT(s->kind == OPH_IRREVERSIBLE, "irreversible");
        ASSERT(history_revertable(&h, s) == 0, "shown as not revertable");
        ASSERT(strcmp(op_history_kind_name(s->kind), "not revertable") == 0, "named so");
        ASSERT(history_revert(ids[i], refusal, sizeof(refusal)) != 0, "a revert is refused");
        ASSERT(refusal[0] != '\0', "with a reason");
    }
    op_history_free(&h);
}

static void test_account_add_is_compensable(void) {
    setup();
    OpHistory h = {0};
    ASSERT(op_history_load(IMAP_USER, "account " IMAP_USER, &h) == 0, "load");
    ASSERT(h.count >= 1, "the account add is recorded");
    ASSERT(strcmp(h.steps[0].thing, "add") == 0, "first step is the add");
    ASSERT(h.steps[0].kind == OPH_COMPENSABLE, "an account add is compensable");
    op_history_free(&h);
}

static void test_shown_recent_own_only(void) {
    setup();
    OpStep s = {0};
    s.who = "bob";
    op_history_set_actor("alice", 0);
    ASSERT(history_shown(&s, 0) == 1, "the history of one object shows every step");
    ASSERT(history_shown(&s, 1) == 0, "recent changes: only the caller's own");
    op_history_set_actor("root", 1);
    ASSERT(history_shown(&s, 1) == 1, "an administrator sees everyone's");
    op_history_set_actor("alice", 0);
}


/* ── The history screen (history_view.c), on a pseudo-terminal ───────── */

static char g_screen[262144];

/** Runs the history screen typed @p keys; its output is left in g_screen. */
static int view_with_keys(const char *object, const char *keys) {
    PtyStdio p;
    if (pty_stdio_begin(&p, 24, 100, keys) != 0) return -99;
    int rc = history_view_run(&g_imap, object);
    pty_stdio_end(&p, g_screen, sizeof(g_screen));
    return rc;
}

static void test_view_recent_and_object(void) {
    setup();
    OpRecord r = { IMAP_USER, "flag", "mail INBOX/0000000000000007", "unread",
                   "yes", "no", "INBOX", OPH_REVERTABLE, 0 };
    ASSERT(op_history_record(&r) > 0, "a step to show");
    int rc = view_with_keys(NULL, "q");
    ASSERT(rc == 0, "q leaves the screen and the program carries on");
    ASSERT(strstr(g_screen, "Recent changes") != NULL, "the recent changes have their title");
    ASSERT(strstr(g_screen, "mail INBOX/0") != NULL, "the step's object is listed");
    ASSERT(strstr(g_screen, "alice") != NULL, "the step's author is listed");
    rc = view_with_keys("mail INBOX/0000000000000007", "\033");
    ASSERT(rc == 0, "Esc leaves the screen");
    ASSERT(strstr(g_screen, "History of mail INBOX/0000000000000007") != NULL,
           "the history of one object is titled with the object");
    ASSERT(strstr(g_screen, "unread") != NULL, "the changed thing is listed");
    rc = view_with_keys("mail INBOX/nothing-here", "q");
    ASSERT(rc == 0, "an object without steps shows an empty screen");
    ASSERT(strstr(g_screen, "unread") == NULL,
           "the history of another object does not show this object's steps");
}

static void test_view_ctrl_c_quits_program(void) {
    setup();
    terminal_clear_quit_request();
    int rc = view_with_keys(NULL, "\003");
    ASSERT(rc == -1, "Ctrl-C asks the caller to quit");
    ASSERT(terminal_quit_requested() == 1, "and the request is remembered for the callers above");
    terminal_clear_quit_request();
}

static void test_view_help_and_navigation(void) {
    setup();
    int rc = view_with_keys(NULL, "?|\033|jkgGq");
    ASSERT(rc == 0, "help, navigation keys and q");
    ASSERT(strstr(g_screen, "History shortcuts") != NULL, "the help names the screen");
    terminal_clear_quit_request();
    rc = view_with_keys(NULL, ":|\033|q");
    ASSERT(rc == 0, "the action menu opens and closes");
    ASSERT(strstr(g_screen, "Revert") != NULL, "the action menu offers Revert");
}

static void test_view_revert_asks_then_reverts(void) {
    setup();
    int cur = app_settings_get_obfuscation();
    app_settings_set_obfuscation(!cur);
    OpHistory h = {0};
    ASSERT(op_history_load(NULL, NULL, &h) == 0 && h.count > 0, "the change is recorded");
    char object[256];
    snprintf(object, sizeof(object), "%s", h.steps[h.count - 1].object);
    op_history_free(&h);
    int before = nsteps();

    int rc = view_with_keys(object, "\rnq");
    ASSERT(rc == 0, "leaving after No");
    ASSERT(nsteps() == before, "No changes nothing");
    ASSERT(app_settings_get_obfuscation() == !cur, "the setting is as it was");

    rc = view_with_keys(object, "\ryq");
    ASSERT(rc == 0, "leaving after Yes");
    ASSERT(nsteps() == before + 1, "Yes records the revert as a step");
    ASSERT(app_settings_get_obfuscation() == cur, "the setting is back");
    ASSERT(strstr(g_screen, "reverted step") != NULL, "the screen says the step was reverted");
}

static void test_view_refusal_shown(void) {
    setup();
    op_history_set_actor("bob", 0);
    OpRecord r = { IMAP_USER, "flag", "mail INBOX/0000000000000009", "unread",
                   "yes", "no", "INBOX", OPH_REVERTABLE, 0 };
    op_history_record(&r);
    op_history_set_actor("alice", 0);
    int rc = view_with_keys("mail INBOX/0000000000000009", "\rynq");
    ASSERT(rc == 0, "leaving");
    ASSERT(strstr(g_screen, "reverted step") == NULL, "another user's step is not reverted");
}

void test_history(void) {
    RUN_TEST(test_store_records);
    RUN_TEST(test_store_nothing_changed);
    RUN_TEST(test_store_escaping);
    RUN_TEST(test_store_file_mode);
    RUN_TEST(test_revert_flag);
    RUN_TEST(test_flag_noop_records_nothing);
    RUN_TEST(test_revert_flag_overtaken);
    RUN_TEST(test_revert_flag_local_store_lacks_mail);
    RUN_TEST(test_non_admin_refused);
    RUN_TEST(test_revert_imap_move);
    RUN_TEST(test_revert_imap_trash);
    RUN_TEST(test_revert_imap_archive);
    RUN_TEST(test_revert_imap_move_server_words);
    RUN_TEST(test_revert_imap_move_without_message_id);
    RUN_TEST(test_revert_junk_overtaken);
    RUN_TEST(test_revert_gmail_label);
    RUN_TEST(test_revert_gmail_label_overtaken);
    RUN_TEST(test_revert_gmail_move);
    RUN_TEST(test_revert_gmail_trash);
    RUN_TEST(test_revert_gmail_archive);
    RUN_TEST(test_revert_gmail_missing_mail);
    RUN_TEST(test_revert_folder_create);
    RUN_TEST(test_revert_rule_add_edit_delete);
    RUN_TEST(test_revert_rule_overtaken);
    RUN_TEST(test_revert_account_field);
    RUN_TEST(test_account_saved_unchanged_records_nothing);
    RUN_TEST(test_revert_setting);
    RUN_TEST(test_account_field_overtaken_and_non_admin);
    RUN_TEST(test_secret_recorded_as_changed);
    RUN_TEST(test_irreversible_not_revertable);
    RUN_TEST(test_account_add_is_compensable);
    RUN_TEST(test_shown_recent_own_only);
    RUN_TEST(test_view_recent_and_object);
    RUN_TEST(test_view_ctrl_c_quits_program);
    RUN_TEST(test_view_help_and_navigation);
    RUN_TEST(test_view_revert_asks_then_reverts);
    RUN_TEST(test_view_refusal_shown);
    history_set_backend(NULL);
    op_history_set_actor(NULL, 0);
    for (int i = 0; i < 4; i++) {
        if (g_old_set[i]) setenv(g_env_names[i], g_old_env[i], 1);
        else unsetenv(g_env_names[i]);
    }
    g_ready = 0;
}
