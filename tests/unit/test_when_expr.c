/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-05-05 */
/* Unit tests of the rule condition expressions. */
#include "test_helpers.h"
#include "when_expr.h"
#include "raii.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── when_parse tests ──────────────────────────────────────────────── */

static void test_parse_null_empty(void) {
    ASSERT(when_parse(NULL) == NULL, "NULL expr → NULL");
    ASSERT(when_parse("") == NULL,   "empty expr → NULL");
    ASSERT(when_parse("   ") == NULL, "whitespace-only → NULL");
}

static void test_parse_single_atom(void) {
    WhenNode *n = when_parse("from:*@github.com");
    ASSERT(n != NULL, "single from atom parses");
    ASSERT(n->type == WN_FROM, "node type is FROM");
    ASSERT(strcmp(n->pattern, "*@github.com") == 0, "pattern correct");
    when_node_free(n);
}

static void test_parse_all_fields(void) {
    WhenNode *n;

    n = when_parse("to:*boss*");
    ASSERT(n && n->type == WN_TO, "to: field");
    when_node_free(n);

    n = when_parse("subject:*urgent*");
    ASSERT(n && n->type == WN_SUBJECT, "subject: field");
    when_node_free(n);

    n = when_parse("label:UNREAD");
    ASSERT(n && n->type == WN_LABEL, "label: field");
    when_node_free(n);

    n = when_parse("body:*password*");
    ASSERT(n && n->type == WN_BODY, "body: field");
    when_node_free(n);

    n = when_parse("age-gt:30");
    ASSERT(n && n->type == WN_AGE_GT && n->age_val == 30, "age-gt: field");
    when_node_free(n);

    n = when_parse("age-lt:7");
    ASSERT(n && n->type == WN_AGE_LT && n->age_val == 7, "age-lt: field");
    when_node_free(n);
}

static void test_parse_not(void) {
    WhenNode *n = when_parse("!from:*spam*");
    ASSERT(n != NULL, "NOT parses");
    ASSERT(n->type == WN_NOT, "root is NOT");
    ASSERT(n->left != NULL && n->left->type == WN_FROM, "child is FROM");
    when_node_free(n);
}

static void test_parse_and(void) {
    WhenNode *n = when_parse("from:*@a.hu* and subject:*news*");
    ASSERT(n != NULL, "AND parses");
    ASSERT(n->type == WN_AND, "root is AND");
    ASSERT(n->left->type == WN_FROM, "left is FROM");
    ASSERT(n->right->type == WN_SUBJECT, "right is SUBJECT");
    when_node_free(n);
}

static void test_parse_or(void) {
    WhenNode *n = when_parse("from:*@a.hu* or from:*@b.hu*");
    ASSERT(n != NULL, "OR parses");
    ASSERT(n->type == WN_OR, "root is OR");
    when_node_free(n);
}

static void test_parse_precedence(void) {
    /* "A or B and C" → "A or (B and C)" */
    WhenNode *n = when_parse("from:*@a.hu* or from:*@b.hu* and !label:UNREAD");
    ASSERT(n != NULL, "complex expr parses");
    ASSERT(n->type == WN_OR, "root is OR (lower prec)");
    ASSERT(n->right->type == WN_AND, "right subtree is AND");
    ASSERT(n->right->right->type == WN_NOT, "NOT binds tightest");
    when_node_free(n);
}

static void test_parse_parens(void) {
    WhenNode *n = when_parse("(from:*@a.hu* or from:*@b.hu*) and !label:UNREAD");
    ASSERT(n != NULL, "parens expr parses");
    ASSERT(n->type == WN_AND, "parens override: root is AND");
    ASSERT(n->left->type == WN_OR, "left is OR group");
    when_node_free(n);
}

static void test_parse_double_not(void) {
    WhenNode *n = when_parse("!!from:*@a.hu*");
    ASSERT(n != NULL, "double NOT parses");
    ASSERT(n->type == WN_NOT, "outer NOT");
    ASSERT(n->left->type == WN_NOT, "inner NOT");
    when_node_free(n);
}

static void test_parse_error_unknown_field(void) {
    WhenNode *n = when_parse("unknownfield:*x*");
    ASSERT(n == NULL, "unknown field → parse error → NULL");
}

static void test_parse_error_missing_rparen(void) {
    WhenNode *n = when_parse("(from:*@a.hu*");
    ASSERT(n == NULL, "missing ) → parse error → NULL");
}

/* ── when_eval tests ──────────────────────────────────────────────── */

static void test_eval_null_ast_always_matches(void) {
    int r = when_eval(NULL, "a@b.com", "Hi", NULL, NULL, NULL, 0);
    ASSERT(r == 1, "NULL AST matches everything");
}

static void test_eval_from(void) {
    WhenNode *n = when_parse("from:*@github.com");
    ASSERT(n != NULL, "parse ok");
    ASSERT(when_eval(n, "notifications@github.com", "PR", NULL, NULL, NULL, 0) == 1, "from match");
    ASSERT(when_eval(n, "user@gmail.com",           "PR", NULL, NULL, NULL, 0) == 0, "from no match");
    when_node_free(n);
}

static void test_eval_subject(void) {
    WhenNode *n = when_parse("subject:*URGENT*");
    ASSERT(n != NULL, "parse ok");
    ASSERT(when_eval(n, NULL, "This is URGENT", NULL, NULL, NULL, 0) == 1, "subject match");
    ASSERT(when_eval(n, NULL, "Normal mail",    NULL, NULL, NULL, 0) == 0, "subject no match");
    when_node_free(n);
}

static void test_eval_label(void) {
    WhenNode *n = when_parse("label:UNREAD");
    ASSERT(n != NULL, "parse ok");
    ASSERT(when_eval(n, NULL, NULL, NULL, "INBOX,UNREAD",  NULL, 0) == 1, "label present");
    ASSERT(when_eval(n, NULL, NULL, NULL, "INBOX,GitHub",  NULL, 0) == 0, "label absent");
    ASSERT(when_eval(n, NULL, NULL, NULL, NULL,            NULL, 0) == 0, "NULL labels csv");
    when_node_free(n);
}

static void test_eval_not(void) {
    WhenNode *n = when_parse("!from:*@spam.com*");
    ASSERT(n != NULL, "parse ok");
    ASSERT(when_eval(n, "user@legit.org", NULL, NULL, NULL, NULL, 0) == 1, "NOT: non-spam matches");
    ASSERT(when_eval(n, "ad@spam.com",    NULL, NULL, NULL, NULL, 0) == 0, "NOT: spam rejected");
    when_node_free(n);
}

static void test_eval_and(void) {
    WhenNode *n = when_parse("from:*@a.hu* and subject:*news*");
    ASSERT(n != NULL, "parse ok");
    ASSERT(when_eval(n, "x@a.hu", "*news* report", NULL, NULL, NULL, 0) == 1, "AND: both match");
    ASSERT(when_eval(n, "x@a.hu", "hello",         NULL, NULL, NULL, 0) == 0, "AND: subj fails");
    ASSERT(when_eval(n, "x@b.hu", "*news*",        NULL, NULL, NULL, 0) == 0, "AND: from fails");
    when_node_free(n);
}

static void test_eval_or(void) {
    WhenNode *n = when_parse("from:*@a.hu* or from:*@b.hu*");
    ASSERT(n != NULL, "parse ok");
    ASSERT(when_eval(n, "x@a.hu", NULL, NULL, NULL, NULL, 0) == 1, "OR: first branch");
    ASSERT(when_eval(n, "x@b.hu", NULL, NULL, NULL, NULL, 0) == 1, "OR: second branch");
    ASSERT(when_eval(n, "x@c.hu", NULL, NULL, NULL, NULL, 0) == 0, "OR: neither");
    when_node_free(n);
}

static void test_eval_case_insensitive(void) {
    WhenNode *n = when_parse("from:*@GITHUB.com");
    ASSERT(n != NULL, "parse ok");
    ASSERT(when_eval(n, "notifications@github.com", NULL, NULL, NULL, NULL, 0) == 1,
           "case-insensitive match");
    when_node_free(n);
}

static void test_eval_age_gt(void) {
    WhenNode *n = when_parse("age-gt:10");
    ASSERT(n != NULL, "parse ok");
    time_t old  = time(NULL) - 15 * 86400;
    time_t fresh = time(NULL) - 5  * 86400;
    ASSERT(when_eval(n, NULL, NULL, NULL, NULL, NULL, old)   == 1, "age > 10 days matches");
    ASSERT(when_eval(n, NULL, NULL, NULL, NULL, NULL, fresh) == 0, "age < 10 days no match");
    ASSERT(when_eval(n, NULL, NULL, NULL, NULL, NULL, 0)     == 0, "unknown date no match");
    when_node_free(n);
}

static void test_eval_age_lt(void) {
    WhenNode *n = when_parse("age-lt:7");
    ASSERT(n != NULL, "parse ok");
    time_t fresh = time(NULL) - 3 * 86400;
    time_t old   = time(NULL) - 9 * 86400;
    ASSERT(when_eval(n, NULL, NULL, NULL, NULL, NULL, fresh) == 1, "age < 7 days matches");
    ASSERT(when_eval(n, NULL, NULL, NULL, NULL, NULL, old)   == 0, "age > 7 days no match");
    when_node_free(n);
}

static void test_eval_body_null(void) {
    WhenNode *n = when_parse("body:*password*");
    ASSERT(n != NULL, "parse ok");
    ASSERT(when_eval(n, NULL, NULL, NULL, NULL, "please enter your password", 0) == 1, "body match");
    ASSERT(when_eval(n, NULL, NULL, NULL, NULL, NULL, 0) == 0, "NULL body no match");
    when_node_free(n);
}

static void test_eval_complex(void) {
    WhenNode *n = when_parse("(from:*@a.hu* or from:*@b.hu*) and !label:UNREAD");
    ASSERT(n != NULL, "complex parse ok");
    ASSERT(when_eval(n, "x@a.hu", NULL, NULL, "INBOX",       NULL, 0) == 1, "from-a, no UNREAD");
    ASSERT(when_eval(n, "x@b.hu", NULL, NULL, "INBOX",       NULL, 0) == 1, "from-b, no UNREAD");
    ASSERT(when_eval(n, "x@a.hu", NULL, NULL, "INBOX,UNREAD",NULL, 0) == 0, "from-a, has UNREAD");
    ASSERT(when_eval(n, "x@c.hu", NULL, NULL, "INBOX",       NULL, 0) == 0, "other from");
    when_node_free(n);
}

/* ── when_from_flat tests ────────────────────────────────────────── */

static void test_from_flat_null_all(void) {
    char *w = when_from_flat(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0);
    ASSERT(w == NULL, "all-null flat → NULL");
}

static void test_from_flat_single(void) {
    RAII_STRING char *w = when_from_flat("*@github.com", NULL, NULL, NULL,
                                          NULL, NULL, NULL, NULL, 0, 0);
    ASSERT(w != NULL, "single from_flat not NULL");
    ASSERT(strcmp(w, "from:*@github.com") == 0, "single from_flat value");
}

static void test_from_flat_and_chain(void) {
    RAII_STRING char *w = when_from_flat("*@a.hu*", "*news*", NULL, NULL,
                                          NULL, NULL, NULL, NULL, 0, 0);
    ASSERT(w != NULL, "and-chain not NULL");
    ASSERT(strcmp(w, "from:*@a.hu* and subject:*news*") == 0, "and-chain value");
}

static void test_from_flat_negated(void) {
    RAII_STRING char *w = when_from_flat(NULL, NULL, NULL, NULL,
                                          "*@spam.com*", NULL, NULL, NULL, 0, 0);
    ASSERT(w != NULL, "negated from_flat not NULL");
    ASSERT(strcmp(w, "!from:*@spam.com*") == 0, "negated value");
}

static void test_from_flat_age(void) {
    RAII_STRING char *w = when_from_flat(NULL, NULL, NULL, NULL,
                                          NULL, NULL, NULL, NULL, 30, 0);
    ASSERT(w != NULL, "age-gt flat not NULL");
    ASSERT(strcmp(w, "age-gt:30") == 0, "age-gt value");
}

/* ── when_from_conds tests ──────────────────────────────────────── */

static void test_from_conds_empty(void) {
    char *w = when_from_conds(NULL, 0, 1);
    ASSERT(w == NULL, "empty conds → NULL");
}

static void test_from_conds_or(void) {
    WhenCond cs[] = {
        { "from", "*@a.hu*", 0 },
        { "from", "*@b.hu*", 0 },
        { "from", "*@c.hu*", 0 },
    };
    RAII_STRING char *w = when_from_conds(cs, 3, 1);
    ASSERT(w != NULL, "OR conds not NULL");
    ASSERT(strcmp(w, "from:*@a.hu* or from:*@b.hu* or from:*@c.hu*") == 0, "OR chain value");
}

static void test_from_conds_and(void) {
    WhenCond cs[] = {
        { "from",    "*@a.hu*", 0 },
        { "subject", "*news*",  0 },
    };
    RAII_STRING char *w = when_from_conds(cs, 2, 0);
    ASSERT(w != NULL, "AND conds not NULL");
    ASSERT(strcmp(w, "from:*@a.hu* and subject:*news*") == 0, "AND chain value");
}

static void test_from_conds_negated(void) {
    WhenCond cs[] = {
        { "from", "*@spam.com*", 1 },
    };
    RAII_STRING char *w = when_from_conds(cs, 1, 0);
    ASSERT(w != NULL, "negated cond not NULL");
    ASSERT(strcmp(w, "!from:*@spam.com*") == 0, "negated cond value");
}

/* Round-trip: build from_conds string, parse it, eval it */
static void test_roundtrip_or_eval(void) {
    WhenCond cs[] = {
        { "from", "*@a.hu*", 0 },
        { "from", "*@b.hu*", 0 },
    };
    RAII_STRING char *w = when_from_conds(cs, 2, 1);
    ASSERT(w != NULL, "roundtrip: conds → string ok");
    WhenNode *tree = when_parse(w);
    ASSERT(tree != NULL, "roundtrip: parse ok");
    ASSERT(when_eval(tree, "x@a.hu", NULL, NULL, NULL, NULL, 0) == 1, "roundtrip: a matches");
    ASSERT(when_eval(tree, "x@b.hu", NULL, NULL, NULL, NULL, 0) == 1, "roundtrip: b matches");
    ASSERT(when_eval(tree, "x@c.hu", NULL, NULL, NULL, NULL, 0) == 0, "roundtrip: c no match");
    when_node_free(tree);
}

/* ── Registration ───────────────────────────────────────────────── */

/* ── quoted patterns (EMAIL-45) ────────────────────────────────────── */

static void test_quoted_pattern_with_space(void) {
    WhenNode *n = when_parse("subject:\"*Message 1*\"");
    ASSERT(n != NULL, "quoted: parses");
    ASSERT(when_eval(n, "a@x", "Hello Message 1 here", "", "", NULL, 0) == 1,
           "quoted: matches a subject with a space");
    ASSERT(when_eval(n, "a@x", "Message 2", "", "", NULL, 0) == 0,
           "quoted: does not match another subject");
    when_node_free(n);
}

static void test_quoted_pattern_with_paren_and_escape(void) {
    WhenNode *n = when_parse("subject:\"*(a \\\"b\\\")*\" and from:*@x");
    ASSERT(n != NULL, "quoted escape: parses");
    ASSERT(when_eval(n, "u@x", "re (a \"b\") ok", "", "", NULL, 0) == 1,
           "quoted escape: paren and embedded quote match");
    when_node_free(n);
}

static void test_quoted_pattern_unterminated(void) {
    ASSERT(when_parse("subject:\"*Message 1*") == NULL, "quoted: unterminated → NULL");
}

static void test_unquoted_space_still_two_terms(void) {
    ASSERT(when_parse("subject:*Message 1*") == NULL,
           "bare word ends at the space: the rest is a syntax error");
}

static void test_from_flat_quotes_space(void) {
    char *w = when_from_flat(NULL, "*Message 1*", NULL, NULL, NULL, NULL, NULL,
                             NULL, 0, 0);
    ASSERT(w != NULL && strcmp(w, "subject:\"*Message 1*\"") == 0,
           "from_flat: pattern with a space is quoted");
    WhenNode *n = when_parse(w);
    ASSERT(n != NULL, "from_flat: quoted result parses");
    ASSERT(when_eval(n, "", "Message 1", "", "", NULL, 0) == 1,
           "from_flat: rule with a space fires");
    when_node_free(n);
    free(w);
}

static void test_from_conds_quotes_space_and_roundtrips(void) {
    WhenCond c[2] = {
        { "subject", "*a \"b\"*", 0 },
        { "from",    "*@x",       0 },
    };
    char *w = when_from_conds(c, 2, 0);
    ASSERT(w != NULL, "from_conds quoting: built");
    WhenNode *n = when_parse(w);
    ASSERT(n != NULL, "from_conds quoting: built expression parses");
    ASSERT(when_eval(n, "u@x", "see a \"b\" now", "", "", NULL, 0) == 1,
           "from_conds quoting: embedded quote round-trips");
    when_node_free(n);
    free(w);
}

static void test_314_empty_pattern_and_all(void) {
    ASSERT(when_parse("from:") == NULL, "314 when: empty pattern is a syntax error");
    ASSERT(when_parse("from:\"\"") == NULL, "314 when: empty quoted pattern is a syntax error");
    ASSERT(when_parse("from:*a* and to:") == NULL, "314 when: empty pattern in an and is an error");
    WhenNode *n = when_parse("all");
    ASSERT(n != NULL && n->type == WN_ALL, "314 when: 'all' parses");
    ASSERT(when_eval(n, "a", "b", "c", "", NULL, 0) == 1, "314 when: 'all' matches");
    when_node_free(n);
    n = when_parse("!all");
    ASSERT(n != NULL && when_eval(n, "a", "b", "c", "", NULL, 0) == 0, "314 when: '!all' matches nothing");
    when_node_free(n);
}


/* ── EMAIL-312: the form model ───────────────────────────────────────── */

static void test_312_clauses_roundtrip(void) {
    WhenClause *c = NULL;
    int n = 0;
    ASSERT(when_clauses_parse("from:*github* and !subject:*digest* or label:INBOX", &c, &n) == 0,
           "clauses: a flat condition has clauses");
    ASSERT(n == 3, "clauses: three clauses");
    ASSERT(strcmp(c[0].field, "from") == 0 && c[0].op == WOP_CONTAINS && strcmp(c[0].value, "github") == 0,
           "clauses: *github* is 'from contains github'");
    ASSERT(!c[0].negated && !c[0].join_or, "clauses: the first is plain");
    ASSERT(c[1].negated && !c[1].join_or && strcmp(c[1].field, "subject") == 0, "clauses: !subject, joined with and");
    ASSERT(c[2].join_or && c[2].op == WOP_IS && strcmp(c[2].value, "INBOX") == 0,
           "clauses: label:INBOX is 'label is INBOX', joined with or");
    char *back = when_clauses_build(c, n);
    ASSERT(back && strcmp(back, "from:*github* and !subject:*digest* or label:INBOX") == 0,
           "clauses: the text is built back unchanged");
    free(back);
    when_clauses_free(c, n);
    free(c);
}

static void test_312_clauses_glob_and_quote(void) {
    WhenClause *c = NULL;
    int n = 0;
    ASSERT(when_clauses_parse("from:*@a.org", &c, &n) == 0 && n == 1 && c[0].op == WOP_GLOB &&
           strcmp(c[0].value, "*@a.org") == 0, "clauses: a pattern with a wildcard but no *x* shape is a glob");
    when_clauses_free(c, n); free(c);
    ASSERT(when_clauses_parse("subject:\"*big deal*\"", &c, &n) == 0 && c[0].op == WOP_CONTAINS &&
           strcmp(c[0].value, "big deal") == 0, "clauses: a quoted pattern with a space is read");
    char *back = when_clauses_build(c, n);
    ASSERT(back && strcmp(back, "subject:\"*big deal*\"") == 0, "clauses: a value with a space is quoted again");
    free(back);
    when_clauses_free(c, n); free(c);
    ASSERT(when_clauses_parse("age-gt:30 and from:x", &c, &n) == 0 && c[0].op == WOP_IS &&
           strcmp(c[0].value, "30") == 0, "clauses: an age is 'is N'");
    when_clauses_free(c, n); free(c);
}

static void test_312_clauses_refuse_what_they_cannot_hold(void) {
    WhenClause *c = NULL;
    int n = 0;
    ASSERT(when_clauses_parse("from:a and (from:b or from:c)", &c, &n) != 0 && !c && n == 0,
           "clauses: parentheses that change the precedence are not a clause list");
    ASSERT(when_clauses_parse("all", &c, &n) != 0, "clauses: 'all' is not a clause list");
    ASSERT(when_clauses_parse("!(from:a or from:b)", &c, &n) != 0, "clauses: a negated group is not a clause list");
    ASSERT(when_clauses_parse("from:a or", &c, &n) != 0, "clauses: an invalid text is not a clause list");
    ASSERT(when_clauses_parse("(from:a and subject:b) or label:c", &c, &n) == 0 && n == 3,
           "clauses: parentheses that match the precedence are read as the same flat list");
    when_clauses_free(c, n); free(c);
}

static void test_312_describe(void) {
    char *d = when_describe("from:*github* and !subject:*digest*");
    ASSERT(d && strcmp(d, "from contains github and subject does not contain digest") == 0, "describe: contains / does not contain");
    free(d);
    d = when_describe("from:a@b.org or label:UNREAD");
    ASSERT(d && strcmp(d, "from is a@b.org or label is UNREAD") == 0, "describe: is");
    free(d);
    d = when_describe("from:*@x.org and (subject:a or subject:b)");
    ASSERT(d && strstr(d, "(subject is a or subject is b)"), "describe: an or under an and keeps its parentheses");
    free(d);
    d = when_describe("age-gt:30");
    ASSERT(d && strcmp(d, "older than 30 days") == 0, "describe: age");
    free(d);
    d = when_describe("all");
    ASSERT(d && strcmp(d, "every message") == 0, "describe: all");
    free(d);
    ASSERT(when_describe("from:") == NULL, "describe: an invalid condition has none");
}

static void test_312_long_pattern_refused_not_cut(void) {
    char expr[4400];
    int n = snprintf(expr, sizeof(expr), "from:");
    for (int i = 0; i < 4200; i++) expr[n++] = 'a';
    expr[n] = '\0';
    ASSERT(when_parse(expr) == NULL, "long: a pattern over 4095 characters is refused, not cut");
    char why[300];
    ASSERT(when_check(expr, why, sizeof(why)) == 0 && strstr(why, "4095"), "long: the reason names the limit");
    char ok[4300];
    n = snprintf(ok, sizeof(ok), "from:");
    for (int i = 0; i < 4000; i++) ok[n++] = 'b';
    ok[n] = '\0';
    WhenNode *t = when_parse(ok);
    ASSERT(t && strlen(t->pattern) == 4000, "long: 4000 characters are kept whole");
    when_node_free(t);
}

static void test_312_check_reasons(void) {
    char why[300];
    ASSERT(when_check("from:*a*", why, sizeof(why)) == 1, "check: a valid condition");
    ASSERT(when_check("", why, sizeof(why)) == 0 && strstr(why, "empty"), "check: empty is refused with a reason");
    ASSERT(when_check("colour:red", why, sizeof(why)) == 0 && strstr(why, "field:pattern"), "check: an unknown field says how it is written");
}

void test_when_expr(void) {
    RUN_TEST(test_314_empty_pattern_and_all);
    RUN_TEST(test_parse_null_empty);
    RUN_TEST(test_parse_single_atom);
    RUN_TEST(test_parse_all_fields);
    RUN_TEST(test_parse_not);
    RUN_TEST(test_parse_and);
    RUN_TEST(test_parse_or);
    RUN_TEST(test_parse_precedence);
    RUN_TEST(test_parse_parens);
    RUN_TEST(test_parse_double_not);
    RUN_TEST(test_parse_error_unknown_field);
    RUN_TEST(test_parse_error_missing_rparen);

    RUN_TEST(test_eval_null_ast_always_matches);
    RUN_TEST(test_eval_from);
    RUN_TEST(test_eval_subject);
    RUN_TEST(test_eval_label);
    RUN_TEST(test_eval_not);
    RUN_TEST(test_eval_and);
    RUN_TEST(test_eval_or);
    RUN_TEST(test_eval_case_insensitive);
    RUN_TEST(test_eval_age_gt);
    RUN_TEST(test_eval_age_lt);
    RUN_TEST(test_eval_body_null);
    RUN_TEST(test_eval_complex);

    RUN_TEST(test_from_flat_null_all);
    RUN_TEST(test_from_flat_single);
    RUN_TEST(test_from_flat_and_chain);
    RUN_TEST(test_from_flat_negated);
    RUN_TEST(test_from_flat_age);

    RUN_TEST(test_from_conds_empty);
    RUN_TEST(test_from_conds_or);
    RUN_TEST(test_from_conds_and);
    RUN_TEST(test_from_conds_negated);
    RUN_TEST(test_roundtrip_or_eval);

    RUN_TEST(test_quoted_pattern_with_space);
    RUN_TEST(test_quoted_pattern_with_paren_and_escape);
    RUN_TEST(test_quoted_pattern_unterminated);
    RUN_TEST(test_unquoted_space_still_two_terms);
    RUN_TEST(test_from_flat_quotes_space);
    RUN_TEST(test_from_conds_quotes_space_and_roundtrips);

    RUN_TEST(test_312_clauses_roundtrip);
    RUN_TEST(test_312_clauses_glob_and_quote);
    RUN_TEST(test_312_clauses_refuse_what_they_cannot_hold);
    RUN_TEST(test_312_describe);
    RUN_TEST(test_312_long_pattern_refused_not_cut);
    RUN_TEST(test_312_check_reasons);
}
