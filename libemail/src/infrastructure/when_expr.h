/**
 * @file when_expr.h
 * @brief Boolean condition expression parser and evaluator for mail rules (US-81).
 *
 * Grammar (precedence: ! > and > or):
 *   expr     ::= and_expr ('or' and_expr)*
 *   and_expr ::= not_expr ('and' not_expr)*
 *   not_expr ::= '!' not_expr | primary
 *   primary  ::= atom | '(' expr ')'
 *   atom     ::= field ':' pattern
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

#endif /* WHEN_EXPR_H */
