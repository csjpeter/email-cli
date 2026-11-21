/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-05-05 */
/* Interface of the rule condition expressions. */
/**
 * @file when_expr.h
 * @brief Boolean condition expression parser and evaluator for mail rules (US-81).
 *
 * Grammar (precedence: ! > and > or):
 *   expr     ::= and_expr ('or' and_expr)*
 *   and_expr ::= not_expr ('and' not_expr)*
 *   not_expr ::= '!' not_expr | primary
 *   primary  ::= atom | 'all' | '(' expr ')'
 *   atom     ::= field ':' pattern   (the pattern must not be empty)
 *   field    ::= 'from' | 'to' | 'subject' | 'label' | 'body' | 'age-gt' | 'age-lt'
 *
 * Example expressions:
 *   from:*@spam.com
 *   from:*@a.hu* or from:*@b.hu*
 *   (from:*alice* or from:*bob*) and !label:UNREAD
 *   age-gt:30 and !from:*boss*
 */

#ifndef WHEN_EXPR_H
#define WHEN_EXPR_H

#include <stddef.h>
#include <time.h>

typedef enum {
    WN_OR,
    WN_AND,
    WN_NOT,
    WN_FROM,
    WN_TO,
    WN_SUBJECT,
    WN_LABEL,
    WN_BODY,
    WN_AGE_GT,
    WN_AGE_LT,
    WN_ALL,   /**< the explicit "all": matches every message */
} WhenNodeType;

typedef struct WhenNode WhenNode;
struct WhenNode {
    WhenNodeType  type;
    WhenNode     *left;    /**< Left operand (OR/AND) or child expression (NOT) */
    WhenNode     *right;   /**< Right operand (OR/AND); NULL for NOT */
    char         *pattern; /**< Glob pattern for leaf atoms; NULL for age/binary nodes */
    int           age_val; /**< Day count for AGE_GT / AGE_LT */
};

/**
 * @brief Parse a when-expression string into an AST.
 *
 * @param expr  The expression string (e.g. "from:*@a.hu* or subject:*news*").
 *              NULL or empty string returns NULL (always-true / no condition).
 * @return Heap-allocated AST, or NULL on syntax error or empty input.
 *         Free with when_node_free().
 */
WhenNode *when_parse(const char *expr);

/**
 * @brief Free an AST created by when_parse().
 */
void when_node_free(WhenNode *n);

/**
 * @brief Evaluate a when-expression AST against a message.
 *
 * @param n             AST to evaluate (NULL = always matches).
 * @param from          Decoded From header (may be NULL).
 * @param subject       Decoded Subject header (may be NULL).
 * @param to            Decoded To header (may be NULL).
 * @param labels_csv    Comma-separated current labels (may be NULL).
 * @param body          Plain-text body (may be NULL; body: atom fails if NULL).
 * @param message_date  Message timestamp (0 = unknown; age atoms fail if 0).
 * @return 1 if condition matches, 0 otherwise.
 */
int when_eval(const WhenNode *n,
              const char *from, const char *subject,
              const char *to, const char *labels_csv,
              const char *body, time_t message_date);

/**
 * @brief Glob match of an address header (From/To), case-insensitive.
 *
 * Matches when the pattern fits the whole header text or any one bare address
 * in it ("GitHub <noreply@github.com>" matches "*@github.com").  An empty
 * pattern matches everything; an empty value matches nothing.
 */
int when_addr_glob(const char *pattern, const char *val);

/**
 * @brief Build a when-expression string from old flat if_* fields (AND chain).
 *
 * Converts the legacy per-field conditions into a single `when` expression.
 * Returns NULL if all fields are empty/zero (no condition = always matches).
 * Caller must free() the returned string.
 */
char *when_from_flat(const char *if_from, const char *if_subject,
                     const char *if_to, const char *if_label,
                     const char *if_not_from, const char *if_not_subject,
                     const char *if_not_to, const char *if_body,
                     int if_age_gt, int if_age_lt);

/**
 * @brief Condition term for when_from_conds().
 */
typedef struct {
    const char *field;    /**< "from","to","subject","body","age-gt","age-lt" */
    const char *pattern;  /**< glob or integer string */
    int         negated;
} WhenCond;

/**
 * @brief Build a when-expression from an array of WhenCond terms.
 *
 * @param conds   Array of condition terms.
 * @param nconds  Number of terms.
 * @param is_or   1 = join with "or"; 0 = join with "and".
 * @return Heap-allocated string, or NULL if nconds == 0. Caller must free().
 */
char *when_from_conds(const WhenCond *conds, int nconds, int is_or);

/* ── The form model (EMAIL-312) ─────────────────────────────────────── */

/** Operators of a clause of the rule form. */
typedef enum {
    WOP_IS = 0,    /**< the field is exactly the value (pattern = value) */
    WOP_CONTAINS,  /**< the field contains the value (pattern = *value*) */
    WOP_GLOB       /**< the value is a glob pattern, used as it is */
} WhenOp;

/** One clause of a flat condition: "[and|or] [not] FIELD OP VALUE". */
typedef struct {
    int    join_or;    /**< 1: joined to the previous clause with "or" (0: "and"; ignored on the first) */
    int    negated;    /**< 1: the clause is negated ("!") */
    char   field[16];  /**< from, to, subject, label, body, age-gt, age-lt */
    int    op;         /**< WhenOp (always WOP_IS for the age fields) */
    char  *value;      /**< heap */
} WhenClause;

/** @brief Frees the values of @p n clauses (not the array). */
void when_clauses_free(WhenClause *c, int n);

/**
 * @brief Splits a condition into clauses when it is flat: atoms (each maybe
 *        negated) joined by and/or, whose meaning the clause list keeps.
 *
 * Anything else ("all", parentheses that change the precedence, a negated
 * group) is not flat.  Every clause is the same text the rule file holds.
 * @param expr   The condition text.
 * @param out    Receives a heap array (free the values with when_clauses_free(), then the array).
 * @param n      Receives the number of clauses.
 * @return 0 when flat, -1 when not (or invalid).
 */
int when_clauses_parse(const char *expr, WhenClause **out, int *n);

/**
 * @brief The condition text of @p n clauses ("a and b or !c").
 * @return Heap string (the empty string for n == 0), or NULL.
 */
char *when_clauses_build(const WhenClause *c, int n);

/**
 * @brief A one-line plain-language description of a condition
 *        ("from contains github.com and subject does not contain digest").
 * @return Heap string, or NULL when @p expr is empty or invalid.
 */
char *when_describe(const char *expr);

/**
 * @brief Checks a condition and says why it is refused.
 * @param reason  Receives the reason when 0 is returned.
 * @return 1 when valid, 0 when not.
 */
int when_check(const char *expr, char *reason, size_t size);

#endif /* WHEN_EXPR_H */
