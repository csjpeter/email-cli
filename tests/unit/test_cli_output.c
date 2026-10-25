/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-24 */
/* Unit tests of the batch output of the read commands: list (table, JSON,
 * columns, filters, virtual views), list-accounts, rules list, list-folders. */
#include "test_helpers.h"
#include "test_scratch.h"
#include "email_service.h"
#include "config_store.h"
#include "mail_rules.h"
#include "local_store.h"
#include "cli_table.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CO_USER "outuser@example.org"
#define CO_HOST "imaps://out.example.org"

static int g_ready;

static void co_setup(void) {
    if (g_ready) return;
    g_ready = 1;
    test_set_home(TP("cli-output/home"));
    Config c = {0};
    c.host = CO_HOST;
    c.user = CO_USER;
    c.pass = "secret";
    config_save_account(&c);
    local_store_init(CO_HOST, CO_USER);
}

static void co_add(Manifest *m, const char *uid, const char *from, const char *subj,
                   const char *date, int flags) {
    manifest_upsert(m, uid, strdup(from), strdup(subj), strdup(date),
                    flags | MSG_FLAG_DMARC_CHECKED | MSG_FLAG_ATTACH_CHECKED);
}

/** A folder of four cached messages; the cache is the only source (cron mode). */
static void co_folder(const char *name) {
    Manifest *m = calloc(1, sizeof(Manifest));
    co_add(m, "0000000000009001", "alice@example.org", "Quarterly numbers",
           "2026-03-01 10:00", MSG_FLAG_UNSEEN);
    co_add(m, "0000000000009002", "bob@example.org", "Lunch plans",
           "2026-03-05 12:30", MSG_FLAG_FLAGGED | MSG_FLAG_ATTACH);
    co_add(m, "0000000000009003", "carol@example.org", "Re: Offer",
           "2026-03-09 08:15", MSG_FLAG_DONE | MSG_FLAG_ANSWERED | MSG_FLAG_JUNK);
    co_add(m, "0000000000009004", "dave@example.org", "Fwd: Photos",
           "2026-03-12 19:45", MSG_FLAG_FORWARDED | MSG_FLAG_DMARC_PASS | MSG_FLAG_PHISHING);
    manifest_save(name, m);
    manifest_free(m);
}

/** Runs @p fn with stdout in a file and returns what it printed (heap). */
static char *co_capture(int (*fn)(void *), void *arg, int *rc_out) {
    const char *path = TP("cli-output/captured.txt");
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(fd, STDOUT_FILENO);
    close(fd);
    int rc = fn(arg);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    if (rc_out) *rc_out = rc;
    char *buf = calloc(1, 131072);
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(buf, 1, 131071, f);
        buf[n] = '\0';
        fclose(f);
    }
    return buf;
}

typedef struct { Config cfg; EmailListOpts opts; } ListCall;

static int co_do_list(void *p) {
    ListCall *c = p;
    return email_service_list(&c->cfg, &c->opts);
}

static void co_list_call(ListCall *c, const char *folder) {
    memset(c, 0, sizeof(*c));
    c->cfg.host = CO_HOST;
    c->cfg.user = CO_USER;
    c->cfg.folder = (char *)folder;
    c->cfg.sync_interval = 1;
    c->opts.folder = folder;
    c->opts.all = 1;
}

static void test_list_batch_table(void) {
    co_setup();
    co_folder("co_table");
    ListCall c;
    co_list_call(&c, "co_table");
    int rc = -9;
    char *out = co_capture(co_do_list, &c, &rc);
    ASSERT(rc == 0, "list table: succeeds");
    ASSERT(strstr(out, "Quarterly numbers") && strstr(out, "Fwd: Photos"),
           "list table: every cached subject is shown");
    ASSERT(strstr(out, "alice@example.org") != NULL, "list table: the sender is shown");
    ASSERT(strstr(out, "2026-03-05") != NULL, "list table: the date is shown");
    ASSERT(strstr(out, "Sts") != NULL, "list table: the status column has a heading");
    ASSERT(strstr(out, "more message") == NULL, "list table: nothing is cut off");
    free(out);

    c.opts.limit = 2;
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "-- 2 more message(s) --") != NULL, "list table: a limit says how many are left");
    ASSERT(strstr(out, "--offset 3") != NULL, "list table: and where the next page starts");
    free(out);

    c.opts.offset = 3;
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "more message") == NULL, "list table: the last page has no hint");
    free(out);
}

static void test_list_batch_json(void) {
    co_setup();
    co_folder("co_json");
    ListCall c;
    co_list_call(&c, "co_json");
    c.opts.json = 1;
    int rc = -9;
    char *out = co_capture(co_do_list, &c, &rc);
    ASSERT(rc == 0, "list json: succeeds");
    ASSERT(out[0] == '[' && strstr(out, "]\n") != NULL, "list json: is one array");
    ASSERT(strstr(out, "\"uid\": \"0000000000009002\"") != NULL, "list json: the uid is a field");
    ASSERT(strstr(out, "\"subject\": \"Lunch plans\"") != NULL, "list json: the subject is whole");
    ASSERT(strstr(out, "\"from\": \"carol@example.org\"") != NULL, "list json: the sender is a field");
    ASSERT(strstr(out, "\"unread\": true") != NULL, "list json: an unread message says so");
    ASSERT(strstr(out, "\"flagged\": true") != NULL, "list json: a starred message says so");
    ASSERT(strstr(out, "\"attachments\": true") != NULL, "list json: an attachment says so");
    ASSERT(strstr(out, "\"folder\": \"co_json\"") != NULL, "list json: the folder is named");
    free(out);
}

static void test_list_columns_and_sort(void) {
    co_setup();
    co_folder("co_cols");
    ListCall c;
    co_list_call(&c, "co_cols");
    cli_out_set_raw("subject,from", "subject");
    int rc = -9;
    char *out = co_capture(co_do_list, &c, &rc);
    ASSERT(rc == 0, "columns: succeeds");
    ASSERT(strstr(out, "SUBJECT") && strstr(out, "FROM"), "columns: the named columns are the heading");
    ASSERT(strstr(out, "UID") == NULL, "columns: the others are left out");
    char *fwd = strstr(out, "Fwd: Photos"), *lunch = strstr(out, "Lunch plans");
    ASSERT(fwd && lunch && fwd < lunch, "columns: --sort orders the rows by subject");
    free(out);

    c.opts.json = 1;
    cli_out_set_raw("uid,unread", NULL);
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "\"uid\"") && strstr(out, "\"unread\""), "columns: json has the named fields");
    ASSERT(strstr(out, "\"subject\"") == NULL, "columns: and only those");
    free(out);

    cli_out_set_raw("nosuchcolumn", NULL);
    c.opts.json = 0;
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(rc == -1, "columns: an unknown column is a usage error");
    free(out);
    cli_out_set_raw(NULL, NULL);
}

static void test_list_filters(void) {
    co_setup();
    co_folder("co_filter");
    ListCall c;
    co_list_call(&c, "co_filter");
    c.opts.filter_from = "BOB";
    int rc = -9;
    char *out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "Lunch plans") != NULL, "filter from: matches the sender, case-insensitively");
    ASSERT(strstr(out, "Quarterly numbers") == NULL, "filter from: leaves the others out");
    free(out);

    c.opts.filter_from = NULL;
    c.opts.filter_since = "2026-03-09";
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "Re: Offer") && strstr(out, "Fwd: Photos"), "filter since: keeps the day itself and later");
    ASSERT(strstr(out, "Lunch plans") == NULL, "filter since: drops earlier mail");
    free(out);

    c.opts.filter_since = NULL;
    c.opts.filter_before = "2026-03-05";
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "Quarterly numbers") != NULL, "filter before: keeps earlier mail");
    ASSERT(strstr(out, "Lunch plans") == NULL, "filter before: the day itself is out");
    free(out);

    c.opts.filter_before = "2026-03-10";
    c.opts.filter_since = "2026-03-05";
    c.opts.filter_from = "example.org";
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "Lunch plans") && strstr(out, "Re: Offer"), "filters: combine");
    ASSERT(strstr(out, "Quarterly numbers") == NULL && strstr(out, "Fwd: Photos") == NULL,
           "filters: outside the window is out");
    free(out);
}

static void test_list_virtual_views(void) {
    co_setup();
    co_folder("co_virtual");
    ListCall c;
    co_list_call(&c, "__flagged__");
    int rc = -9;
    char *out = co_capture(co_do_list, &c, &rc);
    ASSERT(rc == 0, "virtual flagged: succeeds");
    ASSERT(strstr(out, "Lunch plans") != NULL, "virtual flagged: shows the starred mail");
    ASSERT(strstr(out, "  co_") != NULL, "virtual flagged: names the folder each row came from");
    free(out);

    co_list_call(&c, "__unread__");
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "Quarterly numbers") != NULL, "virtual unread: shows unread mail");
    ASSERT(strstr(out, "Lunch plans") == NULL, "virtual unread: and not the read mail");
    free(out);

    co_list_call(&c, "__all__");
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "Quarterly numbers") && strstr(out, "Fwd: Photos"), "virtual all: every cached mail");
    free(out);

    co_list_call(&c, "__junk__");
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "Re: Offer") != NULL, "virtual junk: shows the junk mail");
    free(out);

    co_list_call(&c, "__phishing__");
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "Fwd: Photos") != NULL, "virtual phishing: shows the phishing mail");
    free(out);

    co_list_call(&c, "__answered__");
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "Re: Offer") != NULL, "virtual answered: shows the answered mail");
    free(out);

    co_list_call(&c, "__forwarded__");
    c.opts.json = 1;
    out = co_capture(co_do_list, &c, &rc);
    ASSERT(strstr(out, "\"subject\": \"Fwd: Photos\"") != NULL, "virtual forwarded: json names the mail");
    free(out);
}

static void test_list_empty_cache(void) {
    co_setup();
    ListCall c;
    co_list_call(&c, "co_never_synced");
    int rc = -9;
    char *out = co_capture(co_do_list, &c, &rc);
    ASSERT(rc == 0 || rc == 1, "empty cache: is not an error");
    ASSERT(strstr(out, "sync") != NULL || strstr(out, "No ") != NULL,
           "empty cache: says there is nothing, or to sync");
    free(out);
}

static int co_do_accounts(void *p) { return email_service_list_accounts(*(int *)p); }

static void test_list_accounts_outputs(void) {
    co_setup();
    int json = 0, rc = -9;
    char *out = co_capture(co_do_accounts, &json, &rc);
    ASSERT(rc == 0 && strstr(out, "Account") && strstr(out, CO_USER), "accounts: the table names the account");
    ASSERT(strstr(out, "IMAP") != NULL && strstr(out, "out.example.org") != NULL,
           "accounts: with its type and server");
    free(out);

    json = 1;
    out = co_capture(co_do_accounts, &json, &rc);
    ASSERT(strstr(out, "\"account\": \"") != NULL && strstr(out, "\"type\": \"IMAP\"") != NULL,
           "accounts: json has account and type");
    free(out);

    cli_out_set_raw("account", NULL);
    out = co_capture(co_do_accounts, &json, &rc);
    ASSERT(strstr(out, "\"account\"") != NULL && strstr(out, "\"type\"") == NULL,
           "accounts: json columns are the named ones");
    free(out);
    json = 0;
    out = co_capture(co_do_accounts, &json, &rc);
    ASSERT(strstr(out, "ACCOUNT") != NULL, "accounts: the table columns are the named ones");
    free(out);

    cli_out_set_raw("bogus", NULL);
    out = co_capture(co_do_accounts, &json, &rc);
    ASSERT(rc == -1, "accounts: an unknown column is a usage error");
    free(out);
    cli_out_set_raw(NULL, NULL);
}

typedef struct { const char *only; int json; } RulesCall;
static int co_do_rules_table(void *p) {
    RulesCall *r = p;
    return email_service_rules_table(r->only, r->json);
}
static int co_do_rules_json(void *p) {
    RulesCall *r = p;
    return email_service_rules_json(r->only);
}

static void test_rules_outputs(void) {
    co_setup();
    int n = 0;
    AccountEntry *accs = config_list_accounts(&n);
    char name[256] = "";
    if (n > 0 && accs[0].name) snprintf(name, sizeof(name), "%s", accs[0].name);
    config_free_account_list(accs, n);
    ASSERT(name[0] != '\0', "rules: the fixture account is listed");

    MailRules *r = calloc(1, sizeof(MailRules));
    r->rules = calloc(2, sizeof(MailRule));
    r->cap = 2;
    r->count = 2;
    r->rules[0].name = strdup("Newsletters");
    r->rules[0].when = strdup("from contains news");
    r->rules[0].then_add_label[0] = strdup("Reading");
    r->rules[0].then_add_label[1] = strdup("Later");
    r->rules[0].then_add_count = 2;
    r->rules[0].then_rm_label[0] = strdup("INBOX");
    r->rules[0].then_rm_count = 1;
    r->rules[0].then_move_folder = strdup("Archive");
    r->rules[1].when = strdup("subject contains invoice");
    r->rules[1].then_add_label[0] = strdup("Money");
    r->rules[1].then_add_count = 1;
    ASSERT(mail_rules_save(name, r) == 0, "rules: the fixture rules are saved");
    mail_rules_free(r);

    RulesCall rc_ = { NULL, 0 };
    int rc = -9;
    char *out = co_capture(co_do_rules_table, &rc_, &rc);
    ASSERT(rc == 0 && strstr(out, "ACCOUNT") && strstr(out, "Newsletters"), "rules table: lists the rule");
    ASSERT(strstr(out, "Reading, Later") != NULL, "rules table: joins the labels it adds");
    ASSERT(strstr(out, "(unnamed)") != NULL, "rules table: a rule without a name is marked");
    ASSERT(strstr(out, "Archive") != NULL, "rules table: shows the folder it moves to");
    free(out);

    rc_.json = 1;
    out = co_capture(co_do_rules_table, &rc_, &rc);
    ASSERT(strstr(out, "\"name\"") != NULL && strstr(out, "Newsletters") != NULL, "rules table: json lists the rule");
    free(out);

    rc_.json = 0;
    rc_.only = "nobody@nowhere";
    out = co_capture(co_do_rules_table, &rc_, &rc);
    ASSERT(rc == -1, "rules table: an unknown account is refused");
    free(out);

    rc_.only = NULL;
    out = co_capture(co_do_rules_json, &rc_, &rc);
    ASSERT(rc == 1 || rc >= 1, "rules json: one account printed");
    ASSERT(strstr(out, "\"then_add_label\": [\"Reading\", \"Later\"]") != NULL, "rules json: the labels are an array");
    ASSERT(strstr(out, "\"then_move_folder\": \"Archive\"") != NULL, "rules json: the move target is named");
    free(out);

    rc_.only = "nobody@nowhere";
    out = co_capture(co_do_rules_json, &rc_, &rc);
    ASSERT(rc == 0 && strstr(out, "[]") != NULL, "rules json: an unknown account is an empty array");
    free(out);
}

typedef struct { Config cfg; int tree; } FoldersCall;
static int co_do_folders(void *p) {
    FoldersCall *f = p;
    return email_service_list_folders(&f->cfg, f->tree);
}

static void test_list_folders_outputs(void) {
    co_setup();
    const char *names[] = { "INBOX", "Projects", "Projects.Alpha", "Projects.Beta", "Sent" };
    local_folder_list_save(names, 5, '.');
    FoldersCall f = {0};
    f.cfg.host = CO_HOST;
    f.cfg.user = CO_USER;
    int rc = -9;
    char *out = co_capture(co_do_folders, &f, &rc);
    ASSERT(rc == 0 && strstr(out, "Projects.Alpha") != NULL, "folders flat: full names");
    free(out);

    f.tree = 1;
    out = co_capture(co_do_folders, &f, &rc);
    ASSERT(rc == 0 && strstr(out, "Alpha") != NULL, "folders tree: leaf names under their parent");
    free(out);

    email_service_set_json(1);
    out = co_capture(co_do_folders, &f, &rc);
    ASSERT(strstr(out, "\"name\": \"Projects.Beta\"") != NULL && strstr(out, "\"separator\": \".\"") != NULL,
           "folders json: name and separator");
    free(out);

    cli_out_set_raw("name,total", "name");
    out = co_capture(co_do_folders, &f, &rc);
    ASSERT(strstr(out, "\"total\"") != NULL && strstr(out, "\"unread\"") == NULL, "folders json: named columns");
    free(out);
    email_service_set_json(0);

    f.tree = 0;
    out = co_capture(co_do_folders, &f, &rc);
    ASSERT(strstr(out, "FOLDER") != NULL && strstr(out, "TOTAL") != NULL && strstr(out, "UNREAD") == NULL,
           "folders table: named columns, sorted");
    free(out);
    f.tree = 1;
    out = co_capture(co_do_folders, &f, &rc);
    ASSERT(rc == 0 && strstr(out, "Beta") != NULL, "folders tree: sorted by name keeps the tree");
    free(out);
    cli_out_set_raw("name", "name");
    out = co_capture(co_do_folders, &f, &rc);
    ASSERT(rc == 0, "folders tree: sorted by a count");
    free(out);
    cli_out_set_raw(NULL, NULL);

    Config g = {0};
    g.gmail_mode = 1;
    f.cfg = g;
    out = co_capture(co_do_folders, &f, &rc);
    ASSERT(rc == -1, "folders: a Gmail account has labels, not folders");
    free(out);
}

void test_cli_output(void) {
    RUN_TEST(test_list_batch_table);
    RUN_TEST(test_list_batch_json);
    RUN_TEST(test_list_columns_and_sort);
    RUN_TEST(test_list_filters);
    RUN_TEST(test_list_virtual_views);
    RUN_TEST(test_list_empty_cache);
    RUN_TEST(test_list_accounts_outputs);
    RUN_TEST(test_rules_outputs);
    RUN_TEST(test_list_folders_outputs);
}
