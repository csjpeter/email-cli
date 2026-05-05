/**
 * @file when_expr.c
 * @brief Boolean condition expression parser, evaluator and builder (US-81).
 */

#include "when_expr.h"
#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Internal types ────────────────────────────────────────────────── */

typedef enum {
    T_EOF, T_AND, T_OR, T_NOT, T_LPAREN, T_RPAREN, T_ATOM, T_ERR
} TokType;

typedef struct {
    TokType type;
    char    field[32];
    char    pat[1024];
} Tok;

typedef struct {
    const char *p;
    Tok         cur;
    int         error;
} Lex;

/* ── Lexer ─────────────────────────────────────────────────────────── */

static void lex_skip_ws(Lex *l) {
    while (*l->p == ' ' || *l->p == '\t') l->p++;
}

static Tok lex_read(Lex *l) {
    Tok t;
    memset(&t, 0, sizeof(t));
    lex_skip_ws(l);

    if (!*l->p)               { t.type = T_EOF;    return t; }
    if (*l->p == '(')         { t.type = T_LPAREN; l->p++; return t; }
    if (*l->p == ')')         { t.type = T_RPAREN; l->p++; return t; }
    if (*l->p == '!')         { t.type = T_NOT;    l->p++; return t; }

    /* Read word up to ':', space, paren, '!', or EOF */
    const char *ws = l->p;
    while (*l->p && *l->p != ':' && *l->p != '(' && *l->p != ')' &&
           *l->p != '!' && *l->p != ' ' && *l->p != '\t')
        l->p++;
    size_t wlen = (size_t)(l->p - ws);

    if (wlen == 3 && strncasecmp(ws, "and", 3) == 0) { t.type = T_AND; return t; }
    if (wlen == 2 && strncasecmp(ws, "or",  2) == 0) { t.type = T_OR;  return t; }

    if (*l->p != ':') { t.type = T_ERR; return t; }
    l->p++; /* skip ':' */

    if (wlen >= sizeof(t.field)) wlen = sizeof(t.field) - 1;
    memcpy(t.field, ws, wlen);
    t.field[wlen] = '\0';

    /* Pattern: until whitespace, ')', '(', or EOF */
    const char *ps = l->p;
    while (*l->p && *l->p != ' ' && *l->p != '\t' &&
           *l->p != ')' && *l->p != '(')
        l->p++;
    size_t plen = (size_t)(l->p - ps);
    if (plen >= sizeof(t.pat)) plen = sizeof(t.pat) - 1;
    memcpy(t.pat, ps, plen);
    t.pat[plen] = '\0';

    t.type = T_ATOM;
    return t;
}

static void lex_advance(Lex *l) { l->cur = lex_read(l); }

/* ── AST helpers ───────────────────────────────────────────────────── */

static WhenNode *node_new(WhenNodeType type) {
    WhenNode *n = calloc(1, sizeof(WhenNode));
    if (n) n->type = type;
    return n;
}

void when_node_free(WhenNode *n) {
    if (!n) return;
    when_node_free(n->left);
    when_node_free(n->right);
    free(n->pattern);
    free(n);
}

/* ── Parser (recursive descent) ────────────────────────────────────── */

static WhenNode *parse_or(Lex *l);
static WhenNode *parse_and(Lex *l);
static WhenNode *parse_not(Lex *l);
static WhenNode *parse_primary(Lex *l);

static WhenNode *parse_or(Lex *l) {
    WhenNode *left = parse_and(l);
    if (!left) return NULL;
    while (l->cur.type == T_OR) {
        lex_advance(l);
        WhenNode *right = parse_and(l);
        if (!right) { when_node_free(left); return NULL; }
        WhenNode *n = node_new(WN_OR);
        if (!n) { when_node_free(left); when_node_free(right); return NULL; }
        n->left = left; n->right = right;
        left = n;
    }
    return left;
}

static WhenNode *parse_and(Lex *l) {
    WhenNode *left = parse_not(l);
    if (!left) return NULL;
    while (l->cur.type == T_AND) {
        lex_advance(l);
        WhenNode *right = parse_not(l);
        if (!right) { when_node_free(left); return NULL; }
        WhenNode *n = node_new(WN_AND);
        if (!n) { when_node_free(left); when_node_free(right); return NULL; }
        n->left = left; n->right = right;
        left = n;
    }
    return left;
}

static WhenNode *parse_not(Lex *l) {
    if (l->cur.type == T_NOT) {
        lex_advance(l);
        WhenNode *child = parse_not(l);
        if (!child) return NULL;
        WhenNode *n = node_new(WN_NOT);
        if (!n) { when_node_free(child); return NULL; }
        n->left = child;
        return n;
    }
    return parse_primary(l);
}

static WhenNode *parse_primary(Lex *l) {
    if (l->cur.type == T_LPAREN) {
        lex_advance(l);
        WhenNode *inner = parse_or(l);
        if (!inner) return NULL;
        if (l->cur.type != T_RPAREN) { when_node_free(inner); return NULL; }
        lex_advance(l);
        return inner;
    }
    if (l->cur.type == T_ATOM) {
        WhenNodeType nt;
        if      (strcmp(l->cur.field, "from")    == 0) nt = WN_FROM;
        else if (strcmp(l->cur.field, "to")      == 0) nt = WN_TO;
        else if (strcmp(l->cur.field, "subject") == 0) nt = WN_SUBJECT;
        else if (strcmp(l->cur.field, "label")   == 0) nt = WN_LABEL;
        else if (strcmp(l->cur.field, "body")    == 0) nt = WN_BODY;
        else if (strcmp(l->cur.field, "age-gt")  == 0) nt = WN_AGE_GT;
        else if (strcmp(l->cur.field, "age-lt")  == 0) nt = WN_AGE_LT;
        else { l->error = 1; return NULL; }

        WhenNode *n = node_new(nt);
        if (!n) return NULL;

        if (nt == WN_AGE_GT || nt == WN_AGE_LT) {
            n->age_val = atoi(l->cur.pat);
        } else {
            n->pattern = strdup(l->cur.pat);
            if (!n->pattern) { free(n); return NULL; }
        }
        lex_advance(l);
        return n;
    }
    return NULL;
}

WhenNode *when_parse(const char *expr) {
    if (!expr || !*expr) return NULL;
    Lex l;
    memset(&l, 0, sizeof(l));
    l.p = expr;
    lex_advance(&l);
    if (l.cur.type == T_EOF) return NULL;
    WhenNode *tree = parse_or(&l);
    if (!tree || l.cur.type != T_EOF || l.error) {
        when_node_free(tree);
        return NULL;
    }
    return tree;
}

/* ── Evaluator ─────────────────────────────────────────────────────── */

static int when_glob(const char *pattern, const char *val) {
    if (!pattern || !pattern[0]) return 1;
    if (!val || !val[0]) return 0;
    return fnmatch(pattern, val, FNM_CASEFOLD) == 0;
}

static int when_label_match(const char *pattern, const char *csv) {
    if (!pattern || !pattern[0]) return 1;
    if (!csv || !csv[0]) return 0;
    char *copy = strdup(csv);
    if (!copy) return 0;
    int found = 0;
    char *tok = copy;
    while (tok && *tok) {
        char *sep = strchr(tok, ',');
        if (sep) *sep = '\0';
        if (fnmatch(pattern, tok, FNM_CASEFOLD) == 0) { found = 1; break; }
        tok = sep ? sep + 1 : NULL;
    }
    free(copy);
    return found;
}

int when_eval(const WhenNode *n,
              const char *from, const char *subject,
              const char *to, const char *labels_csv,
              const char *body, time_t message_date)
{
    if (!n) return 1;
    switch (n->type) {
    case WN_OR:
        return when_eval(n->left,  from, subject, to, labels_csv, body, message_date) ||
               when_eval(n->right, from, subject, to, labels_csv, body, message_date);
    case WN_AND:
        return when_eval(n->left,  from, subject, to, labels_csv, body, message_date) &&
               when_eval(n->right, from, subject, to, labels_csv, body, message_date);
    case WN_NOT:
        return !when_eval(n->left, from, subject, to, labels_csv, body, message_date);
    case WN_FROM:    return when_glob(n->pattern, from);
    case WN_TO:      return when_glob(n->pattern, to);
    case WN_SUBJECT: return when_glob(n->pattern, subject);
    case WN_LABEL:   return when_label_match(n->pattern, labels_csv);
    case WN_BODY:
        if (!body) return 0;
        return when_glob(n->pattern, body);
    case WN_AGE_GT:
        if (message_date <= 0) return 0;
        return (int)((time(NULL) - message_date) / 86400) > n->age_val;
    case WN_AGE_LT:
        if (message_date <= 0) return 0;
        return (int)((time(NULL) - message_date) / 86400) < n->age_val;
    }
    return 0;
}

/* ── Expression builders ───────────────────────────────────────────── */

/* Append an atom "field:pattern" (optionally negated) to a buffer. */
static void buf_append_atom(char *buf, size_t cap,
                            const char *field, const char *pattern,
                            int negated, int *first)
{
    if (!pattern || !pattern[0]) return;
    size_t used = strlen(buf);
    if (!*first && used + 5 < cap)
        strncat(buf, " and ", cap - used - 1);
    used = strlen(buf);
    if (negated && used + 2 < cap) {
        strncat(buf, "!", cap - used - 1);
        used++;
    }
    strncat(buf, field, cap - used - 1);
    used = strlen(buf);
    strncat(buf, ":", cap - used - 1);
    used = strlen(buf);
    strncat(buf, pattern, cap - used - 1);
    *first = 0;
}

char *when_from_flat(const char *if_from, const char *if_subject,
                     const char *if_to, const char *if_label,
                     const char *if_not_from, const char *if_not_subject,
                     const char *if_not_to, const char *if_body,
                     int if_age_gt, int if_age_lt)
{
    char buf[8192] = {0};
    int first = 1;
    char tmp[32];

    buf_append_atom(buf, sizeof(buf), "from",    if_from,        0, &first);
    buf_append_atom(buf, sizeof(buf), "subject", if_subject,     0, &first);
    buf_append_atom(buf, sizeof(buf), "to",      if_to,          0, &first);
    buf_append_atom(buf, sizeof(buf), "label",   if_label,       0, &first);
    buf_append_atom(buf, sizeof(buf), "from",    if_not_from,    1, &first);
    buf_append_atom(buf, sizeof(buf), "subject", if_not_subject, 1, &first);
    buf_append_atom(buf, sizeof(buf), "to",      if_not_to,      1, &first);
    buf_append_atom(buf, sizeof(buf), "body",    if_body,        0, &first);

    if (if_age_gt > 0) {
        snprintf(tmp, sizeof(tmp), "%d", if_age_gt);
        buf_append_atom(buf, sizeof(buf), "age-gt", tmp, 0, &first);
    }
    if (if_age_lt > 0) {
        snprintf(tmp, sizeof(tmp), "%d", if_age_lt);
        buf_append_atom(buf, sizeof(buf), "age-lt", tmp, 0, &first);
    }

    return first ? NULL : strdup(buf);
}

char *when_from_conds(const WhenCond *conds, int nconds, int is_or) {
    if (nconds == 0) return NULL;
    const char *sep = is_or ? " or " : " and ";
    char buf[8192] = {0};
    size_t cap = sizeof(buf);

    for (int i = 0; i < nconds; i++) {
        const WhenCond *c = &conds[i];
        if (!c->field || !c->pattern) continue;

        size_t used = strlen(buf);
        if (i > 0 && used + strlen(sep) + 1 < cap) {
            strncat(buf, sep, cap - used - 1);
            used = strlen(buf);
        }
        if (c->negated && used + 2 < cap) {
            strncat(buf, "!", cap - used - 1);
            used = strlen(buf);
        }
        strncat(buf, c->field, cap - used - 1);
        used = strlen(buf);
        strncat(buf, ":", cap - used - 1);
        used = strlen(buf);
        strncat(buf, c->pattern, cap - used - 1);
    }

    return buf[0] ? strdup(buf) : NULL;
}
