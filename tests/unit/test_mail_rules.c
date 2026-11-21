/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-04-23 */
/* Unit tests of the mail rules engine. */
#include "test_helpers.h"
#include "mail_rules.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

static MailRules *make_rules(void) {
    return calloc(1, sizeof(MailRules));
}

static MailRule *add_rule(MailRules *r, const char *name) {
    if (r->count >= r->cap) {
        int nc = r->cap ? r->cap * 2 : 4;
        MailRule *tmp = realloc(r->rules, (size_t)nc * sizeof(MailRule));
        if (!tmp) return NULL;
        r->rules = tmp;
        r->cap   = nc;
    }
    MailRule *rule = &r->rules[r->count++];
    memset(rule, 0, sizeof(*rule));
    rule->name = strdup(name);
    return rule;
}

/* ── Tests ───────────────────────────────────────────────────────── */

/* ── EMAIL-314: rule validation ───────────────────────────────────── */

static MailRules *parse_one(const char *body) {
    char text[2048];
    snprintf(text, sizeof(text), "[rule \"R\"]\n%s\nthen-remove-label = INBOX\n", body);
    return mail_rules_parse_text(text);
}

static int never_fires(const MailRules *r) {
    char **a = NULL, **d = NULL;
    int ac = 0, dc = 0;
    int n = mail_rules_apply(r, "x@y.org", "s", "me@z.org", "INBOX", "b", 0, &a, &ac, &d, &dc);
    for (int i = 0; i < ac; i++) free(a[i]);
    for (int i = 0; i < dc; i++) free(d[i]);
    free(a); free(d);
    return n == 0 && dc == 0;
}

static void test_314_bad_rules_have_errors_and_never_fire(void) {
    static const char *const bad[] = {
        "if-from =", "when =", "when = from:", "when = from:\"\"",
        "if-form = x@y", "when = from:*x* andd", "when = ((from:*x*",
        "then-add-label =", "", "bogus line",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        MailRules *r = parse_one(bad[i]);
        ASSERT(r && r->count == 1, "314.1 the rule is parsed");
        ASSERT(r->rules[0].error != NULL, bad[i]);
        ASSERT(never_fires(r), "314.2 a rule with an error touches no mail");
        mail_rules_free(r);
    }
}

static void test_314_error_names_the_line(void) {
    MailRules *r = mail_rules_parse_text("[rule \"R\"]\nwhen = from:*x*\nif-form = x@y\n");
    ASSERT(r && r->rules[0].error && r->rules[0].line == 3, "314.3 the line of the unknown key");
    ASSERT(strstr(r->rules[0].error, "if-form") != NULL, "314.4 the error names the key");
    ASSERT(mail_rules_error_count(r) == 1, "314.5 one error counted");
    char *buf = NULL; size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    ASSERT(mail_rules_report_errors(fp, "acc", r) == 1, "314.6 one error reported");
    fclose(fp);
    ASSERT(buf && strstr(buf, "line 3") && strstr(buf, "acc"), "314.7 the report names account and line");
    free(buf);
    mail_rules_free(r);
}

static void test_314_when_all_is_explicit_match_all(void) {
    MailRules *r = parse_one("when = all");
    ASSERT(r && r->rules[0].error == NULL, "314.8 'when = all' is valid");
    char **a = NULL, **d = NULL;
    int ac = 0, dc = 0;
    ASSERT(mail_rules_apply(r, "x@y.org", "s", "t", "", "b", 0, &a, &ac, &d, &dc) == 1,
           "314.9 'when = all' fires");
    for (int i = 0; i < dc; i++) free(d[i]);
    free(a); free(d);
    mail_rules_free(r);
    r = parse_one("");
    ASSERT(r && r->rules[0].error && strstr(r->rules[0].error, "when = all"),
           "314.10 no condition: the error says how to match everything");
    mail_rules_free(r);
}

static void test_314_valid_rules_and_label_limit(void) {
    MailRules *r = parse_one("if-from = *@github.com");
    ASSERT(r && r->rules[0].error == NULL, "314.11 a legacy flat condition is valid");
    mail_rules_free(r);
    char body[2048] = "when = from:*x*\n";
    for (int i = 0; i < 33; i++) strcat(body, "then-add-label = L\n");
    r = parse_one(body);
    ASSERT(r && r->rules[0].error && strstr(r->rules[0].error, "more than"),
           "314.12 more than 32 labels is an error");
    mail_rules_free(r);
}

static void test_glob_match_basic(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "GitHub");
    rule->if_from = strdup("*@github.com");
    rule->then_add_label[rule->then_add_count++] = strdup("GitHub");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;
    int fired = mail_rules_apply(r,
                                  "noreply@github.com", "PR review", NULL, "INBOX",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "rule should fire for *@github.com");
    ASSERT(ac == 1,    "should add 1 label");
    ASSERT(strcmp(add[0], "GitHub") == 0, "added label should be GitHub");
    ASSERT(rc2 == 0,   "should remove 0 labels");

    for (int i = 0; i < ac; i++) free(add[i]);
    free(add);
    mail_rules_free(r);
}

static void test_glob_no_match(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "GitHub");
    rule->if_from = strdup("*@github.com");
    rule->then_add_label[rule->then_add_count++] = strdup("GitHub");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;
    int fired = mail_rules_apply(r,
                                  "user@example.com", "Hello", NULL, "INBOX",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "rule should NOT fire for non-github address");
    ASSERT(ac == 0,    "should add 0 labels");

    mail_rules_free(r);
}

static void test_subject_glob(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Invoices");
    rule->if_subject = strdup("*invoice*");
    rule->then_add_label[rule->then_add_count++] = strdup("Invoices");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;
    int fired = mail_rules_apply(r,
                                  "billing@acme.com", "Invoice April 2026", NULL, "",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "subject glob should match 'Invoice April 2026'");
    ASSERT(ac == 1,    "should add Invoices label");

    for (int i = 0; i < ac; i++) free(add[i]);
    free(add);
    mail_rules_free(r);
}

static void test_case_insensitive(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "CI");
    rule->if_from = strdup("*@GITHUB.COM");
    rule->then_add_label[rule->then_add_count++] = strdup("CI");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;
    int fired = mail_rules_apply(r,
                                  "noreply@github.com", "PR", NULL, "",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "glob match should be case-insensitive");

    for (int i = 0; i < ac; i++) free(add[i]);
    free(add);
    mail_rules_free(r);
}

static void test_remove_label(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Archive marketing");
    rule->if_from = strdup("*@marketing.example.com");
    rule->then_add_label[rule->then_add_count++]  = strdup("_spam");
    rule->then_rm_label[rule->then_rm_count++]    = strdup("INBOX");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;
    int fired = mail_rules_apply(r,
                                  "promo@marketing.example.com", "Big Sale!", NULL,
                                  "INBOX,UNREAD",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "rule should fire");
    ASSERT(ac == 1 && strcmp(add[0], "_spam") == 0, "should add _spam");
    ASSERT(rc2 == 1 && strcmp(rm[0], "INBOX") == 0, "should remove INBOX");

    for (int i = 0; i < ac; i++) free(add[i]);
    for (int i = 0; i < rc2; i++) free(rm[i]);
    free(add); free(rm);
    mail_rules_free(r);
}

static void test_multiple_rules_chained(void) {
    /* Rule 1: if from @acme.com → add Client
     * Rule 2: if-label=Client  → add Priority, remove INBOX  */
    MailRules *r = make_rules();

    MailRule *r1 = add_rule(r, "Acme");
    r1->if_from = strdup("*@acme.com");
    r1->then_add_label[r1->then_add_count++] = strdup("Client");

    MailRule *r2 = add_rule(r, "Priority");
    r2->if_label = strdup("Client");
    r2->then_add_label[r2->then_add_count++] = strdup("Priority");
    r2->then_rm_label[r2->then_rm_count++]   = strdup("INBOX");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;
    int fired = mail_rules_apply(r,
                                  "ceo@acme.com", "Quarterly results", NULL, "INBOX",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 2,   "both rules should fire");
    ASSERT(ac == 2,      "should add Client and Priority");
    ASSERT(rc2 == 1,     "should remove INBOX");

    int has_client = 0, has_priority = 0;
    for (int i = 0; i < ac; i++) {
        if (strcmp(add[i], "Client")   == 0) has_client   = 1;
        if (strcmp(add[i], "Priority") == 0) has_priority = 1;
        free(add[i]);
    }
    ASSERT(has_client,   "Client label should be added");
    ASSERT(has_priority, "Priority label should be added");
    for (int i = 0; i < rc2; i++) free(rm[i]);
    free(add); free(rm);
    mail_rules_free(r);
}

static void test_no_rules(void) {
    MailRules *r = make_rules();
    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;
    int fired = mail_rules_apply(r, "x@y.com", "Hi", NULL, "",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "empty rule set fires 0 rules");
    mail_rules_free(r);
}

static void test_no_condition_matches_all(void) {
    /* Rule with no conditions should match any message */
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Catchall");
    rule->then_add_label[rule->then_add_count++] = strdup("Processed");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;
    int fired = mail_rules_apply(r, "any@example.com", "Whatever", NULL, "",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "rule with no conditions should always fire");
    ASSERT(ac == 1,    "should add Processed label");

    for (int i = 0; i < ac; i++) free(add[i]);
    free(add);
    mail_rules_free(r);
}

static void test_body_condition(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Newsletter");
    rule->if_body = strdup("*unsubscribe*");
    rule->then_add_label[rule->then_add_count++] = strdup("Newsletter");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;

    int fired = mail_rules_apply(r, "news@example.com", "Weekly", NULL, "",
                                  "Please unsubscribe here if needed", (time_t)0,
                                  &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "if-body should match");
    ASSERT(ac == 1,    "should add Newsletter label");
    for (int i = 0; i < ac; i++) free(add[i]);
    free(add); free(rm); add = NULL; rm = NULL; ac = 0; rc2 = 0;

    fired = mail_rules_apply(r, "news@example.com", "Weekly", NULL, "",
                              "Hello world", (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "if-body should not match");

    fired = mail_rules_apply(r, "news@example.com", "Weekly", NULL, "",
                              NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "if-body with NULL body should not fire");

    mail_rules_free(r);
}

static void test_age_condition(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Old");
    rule->if_age_gt = 30;
    rule->then_add_label[rule->then_add_count++] = strdup("Old");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;

    time_t now     = time(NULL);
    time_t recent  = now - (1  * 86400);
    time_t old_msg = now - (60 * 86400);

    int fired = mail_rules_apply(r, "x@y.com", "Hi", NULL, "",
                                  NULL, recent, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "if-age-gt=30 should not fire for 1-day-old message");

    fired = mail_rules_apply(r, "x@y.com", "Hi", NULL, "",
                              NULL, old_msg, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "if-age-gt=30 should fire for 60-day-old message");
    for (int i = 0; i < ac; i++) free(add[i]);
    free(add); free(rm); add = NULL; rm = NULL; ac = 0; rc2 = 0;

    fired = mail_rules_apply(r, "x@y.com", "Hi", NULL, "",
                              NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "if-age-gt with unknown date should fire (age check skipped)");
    for (int i = 0; i < ac; i++) free(add[i]);
    free(add); free(rm);

    mail_rules_free(r);
}

static void test_age_lt_condition(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Recent");
    rule->if_age_lt = 7;
    rule->then_add_label[rule->then_add_count++] = strdup("Recent");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;

    time_t now     = time(NULL);
    time_t new_msg = now - (1  * 86400);
    time_t old_msg = now - (30 * 86400);

    int fired = mail_rules_apply(r, "x@y.com", "Hi", NULL, "",
                                  NULL, new_msg, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "if-age-lt=7 should fire for 1-day-old message");
    for (int i = 0; i < ac; i++) free(add[i]);
    free(add); free(rm); add = NULL; rm = NULL; ac = 0; rc2 = 0;

    fired = mail_rules_apply(r, "x@y.com", "Hi", NULL, "",
                              NULL, old_msg, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "if-age-lt=7 should not fire for 30-day-old message");

    mail_rules_free(r);
}

static void test_negation_if_not_from(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Not spam");
    rule->if_not_from = strdup("*@spam.example.com*");
    rule->then_add_label[rule->then_add_count++] = strdup("Legit");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;

    /* Should NOT match spam sender */
    int fired = mail_rules_apply(r, "bad@spam.example.com", "Hi", NULL, "",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "if-not-from should reject matching sender");

    /* Should match non-spam sender */
    fired = mail_rules_apply(r, "good@legit.example.com", "Hi", NULL, "",
                              NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "if-not-from should pass non-matching sender");
    ASSERT(ac == 1,    "should add Legit label");
    for (int i = 0; i < ac; i++) free(add[i]);
    free(add); free(rm);
    mail_rules_free(r);
}

static void test_negation_if_not_subject(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Not newsletter");
    rule->if_not_subject = strdup("*newsletter*");
    rule->then_add_label[rule->then_add_count++] = strdup("Regular");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;

    int fired = mail_rules_apply(r, "x@y.com", "Weekly newsletter digest", NULL, "",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "if-not-subject should reject matching subject");

    fired = mail_rules_apply(r, "x@y.com", "Hello world", NULL, "",
                              NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "if-not-subject should pass non-matching subject");
    for (int i = 0; i < ac; i++) free(add[i]);
    free(add); free(rm);
    mail_rules_free(r);
}

static void test_negation_combined_with_positive(void) {
    /* if-from = *@legit.com AND if-not-subject = *spam* */
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Legit non-spam");
    rule->if_from        = strdup("*@legit.com*");
    rule->if_not_subject = strdup("*spam*");
    rule->then_add_label[rule->then_add_count++] = strdup("OK");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;

    /* Matches: legit domain, non-spam subject */
    int fired = mail_rules_apply(r, "user@legit.com", "Hello", NULL, "",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "positive+negation: should match");
    for (int i = 0; i < ac; i++) free(add[i]);
    free(add); free(rm); add = NULL; rm = NULL; ac = 0; rc2 = 0;

    /* Fails positive: wrong domain */
    fired = mail_rules_apply(r, "user@other.com", "Hello", NULL, "",
                              NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "positive+negation: wrong domain should not match");

    /* Fails negation: spam subject */
    fired = mail_rules_apply(r, "user@legit.com", "Big spam offer", NULL, "",
                              NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "positive+negation: spam subject should not match");
    mail_rules_free(r);
}

static void test_load_utf7_folder_name(void) {
    /* Write a temp rules.ini with a then-move-folder in IMAP modified UTF-7 */
    char tmppath[PATH_MAX];
    snprintf(tmppath, sizeof(tmppath), "%s/test-mail-rules-XXXXXX", test_scratch_dir());
    int fd = mkstemp(tmppath);
    ASSERT(fd >= 0, "utf7 load: mkstemp failed");
    const char *ini =
        "[rule \"hivataos\"]\n"
        "if-from = *@gov.hu*\n"
        /* "hivatalos és pénzügy" in IMAP modified UTF-7 */
        "then-move-folder = hivatalos &AOk-s p&AOk-nz&APw-gy\n";
    ssize_t written = write(fd, ini, strlen(ini));
    (void)written;
    close(fd);

    MailRules *r = mail_rules_load_path(tmppath);
    unlink(tmppath);
    ASSERT(r != NULL, "utf7 load: load_path returned NULL");
    ASSERT(r->count == 1, "utf7 load: expected 1 rule");
    ASSERT(r->rules[0].then_move_folder != NULL, "utf7 load: then_move_folder is NULL");
    /* é = U+00E9 = C3 A9; ü = U+00FC = C3 BC */
    ASSERT(strcmp(r->rules[0].then_move_folder,
                  "hivatalos \xC3\xA9s p\xC3\xA9nz\xC3\xBCgy") == 0,
           "utf7 load: then_move_folder not decoded to UTF-8");
    mail_rules_free(r);
}

static void test_rules_parse_file_features(void) {
    /* Rules file with unnamed rule, when=, then-add/remove-label, then-forward-to */
    char tmppath[PATH_MAX];
    snprintf(tmppath, sizeof(tmppath), "%s/test-mail-rules-features-XXXXXX", test_scratch_dir());
    int fd = mkstemp(tmppath);
    ASSERT(fd >= 0, "parse features: mkstemp failed");

    const char *ini =
        "[rule]\n"
        "when = from:*@test.com\n"
        "then-add-label    = TestLabel\n"
        "then-remove-label = INBOX\n"
        "then-forward-to   = fwd@example.com\n";
    ssize_t w = write(fd, ini, strlen(ini)); (void)w;
    close(fd);

    MailRules *r = mail_rules_load_path(tmppath);
    unlink(tmppath);

    ASSERT(r != NULL, "parse features: load returned non-NULL");
    ASSERT(r->count == 1, "parse features: 1 rule loaded");
    ASSERT(r->rules[0].name != NULL, "parse features: name not NULL");
    ASSERT(strcmp(r->rules[0].name, "(unnamed)") == 0, "parse features: unnamed rule name");
    ASSERT(r->rules[0].when != NULL, "parse features: when set");
    ASSERT(strcmp(r->rules[0].when, "from:*@test.com") == 0, "parse features: when value");
    ASSERT(r->rules[0].then_add_count == 1, "parse features: then_add_count=1");
    ASSERT(strcmp(r->rules[0].then_add_label[0], "TestLabel") == 0, "parse features: then_add_label");
    ASSERT(r->rules[0].then_rm_count == 1, "parse features: then_rm_count=1");
    ASSERT(strcmp(r->rules[0].then_rm_label[0], "INBOX") == 0, "parse features: then_rm_label");
    ASSERT(r->rules[0].then_forward_to != NULL, "parse features: then_forward_to set");
    ASSERT(strcmp(r->rules[0].then_forward_to, "fwd@example.com") == 0,
           "parse features: then_forward_to value");
    mail_rules_free(r);
}

static void test_rules_space_in_glob_fires(void) {
    /* EMAIL-45: both spellings of a subject glob with a space must fire */
    char tmppath[PATH_MAX];
    snprintf(tmppath, sizeof(tmppath), "%s/test-mail-rules-space-XXXXXX", test_scratch_dir());
    int fd = mkstemp(tmppath);
    ASSERT(fd >= 0, "space glob: mkstemp failed");
    const char *ini =
        "[rule \"flat\"]\n"
        "if-subject = *Message 1*\n"
        "then-add-label = L1\n"
        "[rule \"quoted\"]\n"
        "when = subject:\"*Message 1*\"\n"
        "then-add-label = L2\n";
    ssize_t w = write(fd, ini, strlen(ini)); (void)w;
    close(fd);
    MailRules *r = mail_rules_load_path(tmppath);
    unlink(tmppath);
    ASSERT(r != NULL && r->count == 2, "space glob: 2 rules loaded");
    if (r && r->count == 2) {
        ASSERT(mail_rule_matches(&r->rules[0], "a@x", "Message 1", "", "", NULL, 0) == 1,
               "space glob: if-subject = *Message 1* fires");
        ASSERT(mail_rule_matches(&r->rules[1], "a@x", "Message 1", "", "", NULL, 0) == 1,
               "space glob: when = subject:\"*Message 1*\" fires");
        ASSERT(mail_rule_matches(&r->rules[0], "a@x", "Message 2", "", "", NULL, 0) == 0,
               "space glob: flat rule does not match another subject");
    }
    mail_rules_free(r);
}

static void test_rules_when_expression(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "When test");
    rule->when = strdup("from:*@corp.com");
    rule->then_add_label[rule->then_add_count++] = strdup("Corp");

    char **add = NULL, **rm = NULL;
    int ac = 0, rc2 = 0;

    int fired = mail_rules_apply(r, "boss@corp.com", "Hello", NULL, "",
                                  NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 1, "when expr: fires for matching from");
    ASSERT(ac == 1, "when expr: adds 1 label");
    for (int i = 0; i < ac; i++) free(add[i]);
    free(add); free(rm); add = NULL; rm = NULL; ac = 0; rc2 = 0;

    fired = mail_rules_apply(r, "other@example.com", "Hi", NULL, "",
                              NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "when expr: does not fire for non-matching from");

    /* Invalid when expression → rule skipped */
    free(rule->when);
    rule->when = strdup("!!!invalid!!!");
    fired = mail_rules_apply(r, "boss@corp.com", "Hello", NULL, "",
                              NULL, (time_t)0, &add, &ac, &rm, &rc2);
    ASSERT(fired == 0, "when expr: invalid expr → rule skipped");

    mail_rules_free(r);
}

static void test_rules_then_move_folder(void) {
    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Move Rule");
    rule->if_from = strdup("*@move.test");
    rule->then_move_folder = strdup("Processed");

    char **add = NULL, **rm = NULL;
    char *move_folder = NULL;
    int ac = 0, rc2 = 0;

    int fired = mail_rules_apply_ex(r, "x@move.test", "Msg", NULL, "",
                                     NULL, (time_t)0, &add, &ac, &rm, &rc2,
                                     &move_folder);
    ASSERT(fired == 1, "move_folder rule: fired");
    ASSERT(move_folder != NULL, "move_folder rule: move_folder set");
    if (move_folder)
        ASSERT(strcmp(move_folder, "Processed") == 0, "move_folder rule: value");
    free(move_folder);
    mail_rules_free(r);
}

static void test_rules_save_load_account(void) {
    char *old_home = getenv("HOME");
    char *old_xdg  = getenv("XDG_CONFIG_HOME");
    setenv("HOME", TP("email-cli-rules-acct-test"), 1);
    unsetenv("XDG_CONFIG_HOME");

    MailRules *r = make_rules();
    MailRule  *rule = add_rule(r, "Save Rule");
    rule->if_from = strdup("*@save.test");
    rule->when    = strdup("from:*@save.test");
    rule->then_add_label[rule->then_add_count++] = strdup("Saved");
    rule->then_rm_label[rule->then_rm_count++]   = strdup("INBOX");
    rule->then_move_folder = strdup("Archive");

    int rc = mail_rules_save("save-test@save.test", r);
    ASSERT(rc == 0, "rules_save: returns 0");
    mail_rules_free(r);

    MailRules *loaded = mail_rules_load("save-test@save.test");
    ASSERT(loaded != NULL, "rules_load: not NULL");
    if (loaded) {
        ASSERT(loaded->count == 1, "rules_load: count=1");
        ASSERT(loaded->rules[0].when != NULL, "rules_load: when set");
        if (loaded->rules[0].when)
            ASSERT(strcmp(loaded->rules[0].when, "from:*@save.test") == 0,
                   "rules_load: when value");
        ASSERT(loaded->rules[0].then_add_count == 1, "rules_load: then_add_count");
        ASSERT(loaded->rules[0].then_rm_count  == 1, "rules_load: then_rm_count");
        ASSERT(loaded->rules[0].then_move_folder != NULL, "rules_load: move_folder");
        mail_rules_free(loaded);
    }

    /* Load non-existent account → NULL */
    MailRules *miss = mail_rules_load("nobody@nowhere.invalid");
    ASSERT(miss == NULL, "rules_load: missing account returns NULL");

    if (old_home) setenv("HOME", old_home, 1); else unsetenv("HOME");
    if (old_xdg)  setenv("XDG_CONFIG_HOME", old_xdg, 1); else unsetenv("XDG_CONFIG_HOME");
}

/* EMAIL-316: rules.ini keeps long lines, refuses injection, round-trips folders, saves atomically */
static void test_316_save_load_roundtrip(void) {
    char *old_home = getenv("HOME");
    char *old_xdg  = getenv("XDG_CONFIG_HOME");
    setenv("HOME", TP("email-cli-rules-316-test"), 1);
    unsetenv("XDG_CONFIG_HOME");

    /* a 2000-byte when expression */
    char when[2048] = "subject:";
    size_t n = strlen(when);
    memset(when + n, 'a', 1992);
    when[n + 1992] = '\0';

    MailRules *r = make_rules();
    MailRule *rule = add_rule(r, "Long");
    rule->when = strdup(when);
    rule->then_move_folder = strdup("R&D Team");
    ASSERT(mail_rules_save("rt@316.test", r) == 0, "316: save");
    ASSERT(mail_rules_save("rt@316.test", r) == 0, "316: second save");
    mail_rules_free(r);

    MailRules *l = mail_rules_load("rt@316.test");
    ASSERT(l && l->count == 1, "316: one rule loaded");
    if (l && l->count == 1) {
        ASSERT(l->rules[0].when && strcmp(l->rules[0].when, when) == 0, "316: long when intact");
        ASSERT(!l->rules[0].error, "316: no error on long line");
        ASSERT(l->rules[0].then_move_folder &&
               strcmp(l->rules[0].then_move_folder, "R&D Team") == 0, "316: folder round trip");
        ASSERT(mail_rules_save("rt@316.test", l) == 0, "316: resave loaded");
    }
    mail_rules_free(l);
    l = mail_rules_load("rt@316.test");
    ASSERT(l && l->count == 1 && l->rules[0].then_move_folder &&
           strcmp(l->rules[0].then_move_folder, "R&D Team") == 0, "316: folder unchanged after resave");
    mail_rules_free(l);

    char path[512];
    snprintf(path, sizeof(path), "%s/.config/email-cli/accounts/rt@316.test/rules.ini.tmp", getenv("HOME"));
    ASSERT(access(path, F_OK) != 0, "316: no temp file left");

    if (old_home) setenv("HOME", old_home, 1); else unsetenv("HOME");
    if (old_xdg)  setenv("XDG_CONFIG_HOME", old_xdg, 1); else unsetenv("XDG_CONFIG_HOME");
}

static void test_316_injection_refused(void) {
    char *old_home = getenv("HOME");
    char *old_xdg  = getenv("XDG_CONFIG_HOME");
    setenv("HOME", TP("email-cli-rules-316b-test"), 1);
    unsetenv("XDG_CONFIG_HOME");

    MailRules *r = make_rules();
    MailRule *rule = add_rule(r, "a\"b");
    rule->when = strdup("from:x");
    ASSERT(mail_rules_save("inj@316.test", r) != 0, "316: quote in name refused");
    mail_rules_free(r);

    r = make_rules();
    rule = add_rule(r, "Ok");
    rule->when = strdup("from:x");
    rule->then_add_label[rule->then_add_count++] = strdup("Foo\nthen-move-folder = Trash");
    ASSERT(mail_rules_save("inj@316.test", r) != 0, "316: newline in label refused");
    ASSERT(mail_rules_load("inj@316.test") == NULL, "316: nothing written on refusal");
    mail_rules_free(r);

    MailRules *p = mail_rules_parse_text("[rule \"x\"]\nwhen = from:a\r\n");
    ASSERT(p && p->count == 1 && !p->rules[0].error, "316: trailing CR is a line end");
    mail_rules_free(p);
    p = mail_rules_parse_text("[rule \"x\"]\nwhen = from:a\rthen-move-folder = Trash\n");
    ASSERT(p && p->count == 1 && p->rules[0].error, "316: inner CR is an error");
    mail_rules_free(p);

    if (old_home) setenv("HOME", old_home, 1); else unsetenv("HOME");
    if (old_xdg)  setenv("XDG_CONFIG_HOME", old_xdg, 1); else unsetenv("XDG_CONFIG_HOME");
}

static void test_rules_find_by_name_and_action(void) {
    MailRules *r = make_rules();
    add_rule(r, "x")->action_index = 0;
    add_rule(r, "x")->action_index = 2;
    add_rule(r, "y");
    ASSERT(mail_rules_find(r, "x", 0) == 0, "find: action 0 is the default group");
    ASSERT(mail_rules_find(r, "x", 1) == 0, "find: action 1 is the default group");
    ASSERT(mail_rules_find(r, "x", 2) == 1, "find: action 2 hits the second rule");
    ASSERT(mail_rules_find(r, "x", 3) == -1, "find: unknown action");
    ASSERT(mail_rules_find(r, "nope", 1) == -1, "find: unknown name");
    mail_rules_free(r);
}

static void test_rules_save_refuses_duplicates(void) {
    char *old_home = getenv("HOME");
    char *old_xdg  = getenv("XDG_CONFIG_HOME");
    setenv("HOME", TP("email-cli-rules-dup-test"), 1);
    unsetenv("XDG_CONFIG_HOME");

    MailRules *r = make_rules();
    add_rule(r, "dup");
    add_rule(r, "dup");
    ASSERT(mail_rules_save("dup@save.test", r) != 0, "save: duplicate (name, action) refused");
    ASSERT(mail_rules_load("dup@save.test") == NULL, "save: nothing written on refusal");
    r->rules[1].action_index = 2;
    ASSERT(mail_rules_save("dup@save.test", r) == 0, "save: same name, other action is fine");
    mail_rules_free(r);

    if (old_home) setenv("HOME", old_home, 1); else unsetenv("HOME");
    if (old_xdg)  setenv("XDG_CONFIG_HOME", old_xdg, 1); else unsetenv("XDG_CONFIG_HOME");
}

static void test_rules_history_pairs_action_group(void) {
    char *old_home = getenv("HOME");
    char *old_xdg  = getenv("XDG_CONFIG_HOME");
    char *old_data = getenv("XDG_DATA_HOME");
    setenv("HOME", TP("email-cli-rules-hist-test"), 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");

    MailRules *r = make_rules();
    add_rule(r, "x")->when = strdup("from:one@h.test");
    MailRule *b = add_rule(r, "x");
    b->action_index = 2;
    b->when = strdup("from:two@h.test");
    ASSERT(mail_rules_save("hist@save.test", r) == 0, "history: save");
    free(r->rules[1].when);
    r->rules[1].when = strdup("from:three@h.test");
    ASSERT(mail_rules_save("hist@save.test", r) == 0, "history: edit of group 2 saved");
    mail_rules_free(r);

    MailRules *l = mail_rules_load("hist@save.test");
    ASSERT(l && l->count == 2, "history: both rules kept");
    if (l) {
        ASSERT(strcmp(l->rules[0].when, "from:one@h.test") == 0, "history: rule 1 untouched");
        ASSERT(l->rules[1].action_index == 2, "history: action index kept");
        mail_rules_free(l);
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/.local/share/email-cli/history.tsv", getenv("HOME"));
    FILE *fp = fopen(path, "r");
    ASSERT(fp != NULL, "history: file exists");
    if (fp) {
        char line[4096];
        int edit2 = 0, edit1 = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "rule x action 2") && strstr(line, "edit")
                && strstr(line, "two@h.test") && strstr(line, "three@h.test")) edit2 = 1;
            if (strstr(line, "rule x\t") && strstr(line, "edit")
                && strstr(line, "three@h.test")) edit1 = 1;
        }
        fclose(fp);
        ASSERT(edit2, "history: the edit of group 2 is recorded as group 2 text to group 2 text");
        ASSERT(!edit1, "history: the edit is not paired with rule 1");
    }

    if (old_home) setenv("HOME", old_home, 1); else unsetenv("HOME");
    if (old_xdg)  setenv("XDG_CONFIG_HOME", old_xdg, 1); else unsetenv("XDG_CONFIG_HOME");
    if (old_data) setenv("XDG_DATA_HOME", old_data, 1);
}

/* ── Registration ────────────────────────────────────────────────── */

void test_mail_rules(void) {
    RUN_TEST(test_glob_match_basic);
    RUN_TEST(test_glob_no_match);
    RUN_TEST(test_subject_glob);
    RUN_TEST(test_case_insensitive);
    RUN_TEST(test_remove_label);
    RUN_TEST(test_multiple_rules_chained);
    RUN_TEST(test_no_rules);
    RUN_TEST(test_no_condition_matches_all);
    RUN_TEST(test_314_bad_rules_have_errors_and_never_fire);
    RUN_TEST(test_314_error_names_the_line);
    RUN_TEST(test_314_when_all_is_explicit_match_all);
    RUN_TEST(test_314_valid_rules_and_label_limit);
    RUN_TEST(test_body_condition);
    RUN_TEST(test_age_condition);
    RUN_TEST(test_age_lt_condition);
    RUN_TEST(test_negation_if_not_from);
    RUN_TEST(test_negation_if_not_subject);
    RUN_TEST(test_negation_combined_with_positive);
    RUN_TEST(test_load_utf7_folder_name);
    RUN_TEST(test_rules_parse_file_features);
    RUN_TEST(test_rules_when_expression);
    RUN_TEST(test_rules_space_in_glob_fires);
    RUN_TEST(test_rules_then_move_folder);
    RUN_TEST(test_rules_save_load_account);
    RUN_TEST(test_316_save_load_roundtrip);
    RUN_TEST(test_316_injection_refused);
    RUN_TEST(test_rules_find_by_name_and_action);
    RUN_TEST(test_rules_save_refuses_duplicates);
    RUN_TEST(test_rules_history_pairs_action_group);
}
