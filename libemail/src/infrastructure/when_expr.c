/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-05-05 */
/* Parser, evaluator and builder of the boolean conditions of mail rules. */
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
    T_EOF, T_AND, T_OR, T_NOT, T_LPAREN, T_RPAREN, T_ATOM, T_ALL, T_ERR
} TokType;

typedef struct {
    TokType type;
    char    field[32];
    char    pat[4096];
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

    if (wlen == 3 && strncasecmp(ws, "all", 3) == 0 && *l->p != ':') {
        t.type = T_ALL;   /* the explicit "matches everything" condition */
        return t;
    }

    if (*l->p != ':') { t.type = T_ERR; return t; }
    l->p++; /* skip ':' */

    if (wlen >= sizeof(t.field)) wlen = sizeof(t.field) - 1;
    memcpy(t.field, ws, wlen);
    t.field[wlen] = '\0';

    /* Pattern: either a double-quoted string ("..." with \" and \\ escapes,
     * which may hold spaces and parentheses) or a bare word that ends at
     * whitespace, ')', '(' or EOF. */
    size_t plen = 0;
    if (*l->p == '"') {
        l->p++;
        while (*l->p && *l->p != '"') {
            if (*l->p == '\\' && (l->p[1] == '"' || l->p[1] == '\\'))
                l->p++;
            if (plen >= sizeof(t.pat) - 1) { t.type = T_ERR; return t; }   /* too long: refused, never cut */
            t.pat[plen++] = *l->p;
            l->p++;
        }
        if (*l->p != '"') { t.type = T_ERR; return t; }   /* unterminated */
        l->p++;
    } else {
        const char *ps = l->p;
        while (*l->p && *l->p != ' ' && *l->p != '\t' &&
               *l->p != ')' && *l->p != '(')
            l->p++;
        plen = (size_t)(l->p - ps);
        if (plen >= sizeof(t.pat)) { t.type = T_ERR; return t; }   /* too long: refused, never cut */
        memcpy(t.pat, ps, plen);
    }
    t.pat[plen] = '\0';

    /* An empty pattern would match every message: a mistake, not a condition. */
    if (plen == 0) { t.type = T_ERR; return t; }

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
    if (l->cur.type == T_ALL) {
        WhenNode *n = node_new(WN_ALL);
        if (!n) return NULL;
        lex_advance(l);
        return n;
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

int when_addr_glob(const char *pattern, const char *val) {
    if (!pattern || !pattern[0]) return 1;
    if (!val || !val[0]) return 0;
    if (fnmatch(pattern, val, FNM_CASEFOLD) == 0) return 1;

    /* EMAIL-313: a header with display names ("GitHub <noreply@github.com>,
     * Bob <b@x.org>"): each bare address counts as well as the whole text. */
    char *copy = strdup(val);
    if (!copy) return 0;
    int found = 0;
    char *p = copy;
    while (*p && !found) {
        char *lt = strchr(p, '<');
        char *start, *end;
        if (lt) {
            end = strchr(lt, '>');
            if (!end) break;
            start = lt + 1;
            *end = '\0';
            p = end + 1;
        } else {
            /* no angle brackets left: comma-separated bare addresses */
            start = p;
            end = strchr(p, ',');
            if (end) { *end = '\0'; p = end + 1; } else p += strlen(p);
        }
        while (*start == ' ' || *start == '\t' || *start == ',') start++;
        size_t n = strlen(start);
        while (n > 0 && (start[n-1] == ' ' || start[n-1] == '\t')) start[--n] = '\0';
        if (n > 0 && fnmatch(pattern, start, FNM_CASEFOLD) == 0) found = 1;
    }
    free(copy);
    return found;
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
    case WN_ALL:
        return 1;
    case WN_OR:
        return when_eval(n->left,  from, subject, to, labels_csv, body, message_date) ||
               when_eval(n->right, from, subject, to, labels_csv, body, message_date);
    case WN_AND:
        return when_eval(n->left,  from, subject, to, labels_csv, body, message_date) &&
               when_eval(n->right, from, subject, to, labels_csv, body, message_date);
    case WN_NOT:
        return !when_eval(n->left, from, subject, to, labels_csv, body, message_date);
    case WN_FROM:    return when_addr_glob(n->pattern, from);
    case WN_TO:      return when_addr_glob(n->pattern, to);
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

/* Append a pattern; quote it when a bare word could not carry it. */
static void buf_append_pattern(char *buf, size_t cap, const char *pattern) {
    size_t used = strlen(buf);
    int quote = pattern[0] == '"';
    for (const char *q = pattern; *q && !quote; q++)
        if (*q == ' ' || *q == '\t' || *q == '(' || *q == ')') quote = 1;
    if (!quote) {
        strncat(buf, pattern, cap - used - 1);
        return;
    }
    if (used + 1 < cap) buf[used++] = '"';
    for (const char *q = pattern; *q && used + 3 < cap; q++) {
        if (*q == '"' || *q == '\\') buf[used++] = '\\';
        buf[used++] = *q;
    }
    if (used + 1 < cap) buf[used++] = '"';
    buf[used] = '\0';
}

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
    buf_append_pattern(buf, cap, pattern);
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
        buf_append_pattern(buf, cap, c->pattern);
    }

    return buf[0] ? strdup(buf) : NULL;
}

/* ── The form model (EMAIL-312) ────────────────────────────────────── */

void when_clauses_free(WhenClause *c, int n) {
    for (int i = 0; c && i < n; i++) free(c[i].value);
}

/** Heap copy of @p pattern, quoted when a bare word could not carry it. */
static char *quote_pattern(const char *pattern) {
    size_t n = strlen(pattern);
    char *out = malloc(n * 2 + 3);
    if (!out) return NULL;
    int quote = pattern[0] == '"';
    for (const char *q = pattern; *q && !quote; q++)
        if (*q == ' ' || *q == '\t' || *q == '(' || *q == ')') quote = 1;
    if (!quote) { memcpy(out, pattern, n + 1); return out; }
    char *o = out;
    *o++ = '"';
    for (const char *q = pattern; *q; q++) {
        if (*q == '"' || *q == '\\') *o++ = '\\';
        *o++ = *q;
    }
    *o++ = '"';
    *o = '\0';
    return out;
}

static int field_is_age(const char *f) {
    return strcmp(f, "age-gt") == 0 || strcmp(f, "age-lt") == 0;
}

/** The glob pattern of a clause (heap). */
static char *clause_pattern(const WhenClause *c) {
    const char *v = c->value ? c->value : "";
    if (c->op == WOP_CONTAINS && !field_is_age(c->field)) {
        size_t n = strlen(v);
        char *p = malloc(n + 3);
        if (!p) return NULL;
        p[0] = '*'; memcpy(p + 1, v, n); p[n + 1] = '*'; p[n + 2] = '\0';
        return p;
    }
    return strdup(v);
}

char *when_clauses_build(const WhenClause *c, int n) {
    size_t cap = 64;
    for (int i = 0; i < n; i++) cap += strlen(c[i].value ? c[i].value : "") * 2 + 48;
    char *buf = calloc(1, cap);
    if (!buf) return NULL;
    for (int i = 0; i < n; i++) {
        if (i > 0) strcat(buf, c[i].join_or ? " or " : " and ");
        if (c[i].negated) strcat(buf, "!");
        strcat(buf, c[i].field);
        strcat(buf, ":");
        char *pat = clause_pattern(&c[i]);
        char *q = pat ? quote_pattern(pat) : NULL;
        if (q) strcat(buf, q);
        free(q);
        free(pat);
    }
    return buf;
}

static int when_equal(const WhenNode *a, const WhenNode *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type || a->age_val != b->age_val) return 0;
    if ((a->pattern == NULL) != (b->pattern == NULL)) return 0;
    if (a->pattern && strcmp(a->pattern, b->pattern) != 0) return 0;
    return when_equal(a->left, b->left) && when_equal(a->right, b->right);
}

static const char *node_field(WhenNodeType t) {
    switch (t) {
    case WN_FROM: return "from";       case WN_TO: return "to";
    case WN_SUBJECT: return "subject"; case WN_LABEL: return "label";
    case WN_BODY: return "body";       case WN_AGE_GT: return "age-gt";
    case WN_AGE_LT: return "age-lt";   default: return NULL;
    }
}

typedef struct { WhenClause *c; int n, cap, pending_or; } Flat;

static int flat_add(Flat *f, const WhenNode *atom, int negated) {
    const char *field = node_field(atom->type);
    if (!field) return -1;
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 8;
        WhenClause *t = realloc(f->c, (size_t)nc * sizeof(WhenClause));
        if (!t) return -1;
        f->c = t; f->cap = nc;
    }
    WhenClause *c = &f->c[f->n];
    memset(c, 0, sizeof(*c));
    snprintf(c->field, sizeof(c->field), "%s", field);
    c->join_or = f->n > 0 ? f->pending_or : 0;
    c->negated = negated;
    if (field_is_age(field)) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%d", atom->age_val);
        c->op = WOP_IS;
        c->value = strdup(tmp);
    } else {
        const char *p = atom->pattern ? atom->pattern : "";
        size_t n = strlen(p);
        int has_glob = strpbrk(p, "*?[") != NULL;
        int inner_plain = n >= 3 && p[0] == '*' && p[n - 1] == '*';
        for (size_t k = 1; inner_plain && k + 1 < n; k++)
            if (p[k] == '*' || p[k] == '?' || p[k] == '[') inner_plain = 0;
        if (inner_plain) {
            c->op = WOP_CONTAINS;
            c->value = strndup(p + 1, n - 2);
        } else {
            c->op = has_glob ? WOP_GLOB : WOP_IS;
            c->value = strdup(p);
        }
    }
    if (!c->value) return -1;
    f->n++;
    return 0;
}

/** Walks the tree in order; -1 when it is not a flat list of (negated) atoms. */
static int flat_walk(Flat *f, const WhenNode *n) {
    if (!n) return -1;
    if (n->type == WN_OR || n->type == WN_AND) {
        if (flat_walk(f, n->left) != 0) return -1;
        f->pending_or = n->type == WN_OR;
        return flat_walk(f, n->right);
    }
    if (n->type == WN_NOT)
        return n->left && node_field(n->left->type) ? flat_add(f, n->left, 1) : -1;
    return flat_add(f, n, 0);
}

int when_clauses_parse(const char *expr, WhenClause **out, int *n) {
    *out = NULL; *n = 0;
    WhenNode *tree = when_parse(expr);
    if (!tree) return -1;
    Flat f = {0};
    int rc = flat_walk(&f, tree);
    if (rc == 0) {
        /* The list must mean what the text means (parentheses may differ). */
        char *back = when_clauses_build(f.c, f.n);
        WhenNode *again = back ? when_parse(back) : NULL;
        if (!again || !when_equal(tree, again)) rc = -1;
        when_node_free(again);
        free(back);
    }
    when_node_free(tree);
    if (rc != 0) { when_clauses_free(f.c, f.n); free(f.c); return -1; }
    *out = f.c; *n = f.n;
    return 0;
}

/* ── Plain-language description ────────────────────────────────────── */

typedef struct { char *s; size_t len, cap; } Sb;

static void sb_add(Sb *b, const char *t) {
    size_t n = strlen(t);
    if (b->len + n + 1 > b->cap) {
        size_t nc = (b->len + n + 1) * 2;
        char *x = realloc(b->s, nc);
        if (!x) return;
        b->s = x; b->cap = nc;
    }
    memcpy(b->s + b->len, t, n + 1);
    b->len += n;
}

static void describe_atom(Sb *b, const WhenNode *n, int negated) {
    const char *field = node_field(n->type);
    char tmp[64];
    if (n->type == WN_AGE_GT || n->type == WN_AGE_LT) {
        const char *what = n->type == WN_AGE_GT ? (negated ? "not older than" : "older than")
                                                : (negated ? "not newer than" : "newer than");
        snprintf(tmp, sizeof(tmp), "%s %d days", what, n->age_val);
        sb_add(b, tmp);
        return;
    }
    const char *p = n->pattern ? n->pattern : "";
    size_t len = strlen(p);
    int contains = len >= 3 && p[0] == '*' && p[len - 1] == '*';
    for (size_t k = 1; contains && k + 1 < len; k++)
        if (p[k] == '*' || p[k] == '?' || p[k] == '[') contains = 0;
    const char *verb;
    char *val;
    if (contains) { verb = negated ? "does not contain" : "contains"; val = strndup(p + 1, len - 2); }
    else if (strpbrk(p, "*?[")) { verb = negated ? "does not match" : "matches"; val = strdup(p); }
    else { verb = negated ? "is not" : "is"; val = strdup(p); }
    sb_add(b, field); sb_add(b, " "); sb_add(b, verb); sb_add(b, " ");
    sb_add(b, val ? val : "");
    free(val);
}

static void describe_node(Sb *b, const WhenNode *n, int parent_and) {
    switch (n->type) {
    case WN_ALL: sb_add(b, "every message"); break;
    case WN_OR:
        if (parent_and) sb_add(b, "(");
        describe_node(b, n->left, 0);
        sb_add(b, " or ");
        describe_node(b, n->right, 0);
        if (parent_and) sb_add(b, ")");
        break;
    case WN_AND:
        describe_node(b, n->left, 1);
        sb_add(b, " and ");
        describe_node(b, n->right, 1);
        break;
    case WN_NOT:
        if (n->left && node_field(n->left->type)) describe_atom(b, n->left, 1);
        else { sb_add(b, "not ("); describe_node(b, n->left, 0); sb_add(b, ")"); }
        break;
    default: describe_atom(b, n, 0); break;
    }
}

char *when_describe(const char *expr) {
    WhenNode *t = when_parse(expr);
    if (!t) return NULL;
    Sb b = {0};
    describe_node(&b, t, 0);
    when_node_free(t);
    return b.s;
}

int when_check(const char *expr, char *reason, size_t size) {
    if (!expr || !*expr) {
        snprintf(reason, size, "the condition is empty (write 'all' for every message)");
        return 0;
    }
    WhenNode *t = when_parse(expr);
    if (t) { when_node_free(t); return 1; }
    /* Say why: a pattern the parser cannot hold, or the shape. */
    size_t run = 0, longest = 0;
    int quoted = 0;
    for (const char *p = expr; *p; p++) {
        if (quoted) {
            if (*p == '\\' && p[1]) { p++; run++; }
            else if (*p == '"') quoted = 0;
            else run++;
        } else if (*p == ':') {
            run = 0;
            if (p[1] == '"') { quoted = 1; p++; }
            else {
                const char *q = p + 1;
                while (*q && *q != ' ' && *q != '\t' && *q != '(' && *q != ')') q++;
                run = (size_t)(q - (p + 1));
                p = q - 1;
            }
        } else run = 0;
        if (run > longest) longest = run;
    }
    if (longest >= 1024)
        snprintf(reason, size, "a pattern is longer than 4095 characters (it would be cut, so it is refused)");
    else
        snprintf(reason, size, "not a valid condition; write field:pattern joined by and / or, "
                 "with ! for not (fields: from, to, subject, label, body, age-gt, age-lt)");
    return 0;
}
