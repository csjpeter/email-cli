#include "test_helpers.h"
#include "mail_rules.h"
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
    char tmppath[] = "/tmp/test-mail-rules-XXXXXX";
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
    RUN_TEST(test_body_condition);
    RUN_TEST(test_age_condition);
    RUN_TEST(test_age_lt_condition);
    RUN_TEST(test_negation_if_not_from);
    RUN_TEST(test_negation_if_not_subject);
    RUN_TEST(test_negation_combined_with_positive);
    RUN_TEST(test_load_utf7_folder_name);
}
