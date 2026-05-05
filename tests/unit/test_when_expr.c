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

void test_when_expr(void) {
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
}
