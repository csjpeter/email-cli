/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-23 */
/* The shared --columns / --sort implementation of the CLI tables. */
#include "cli_table.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *g_raw_columns;
static const char *g_raw_sort;

void cli_out_set_raw(const char *columns, const char *sort) {
    g_raw_columns = columns;
    g_raw_sort = sort;
}

int cli_out_extract(int *argc, char **argv) {
    int w = 1;
    for (int r = 1; r < *argc; r++) {
        const char *a = argv[r];
        const char **dst = NULL;
        const char *val = NULL;
        const char *name = NULL;
        if (strcmp(a, "--columns") == 0 || strcmp(a, "--sort") == 0) {
            name = a;
            dst = a[2] == 'c' ? &g_raw_columns : &g_raw_sort;
            if (r + 1 >= *argc) {
                fprintf(stderr, "Error: %s requires a value.\n", name);
                return -1;
            }
            val = argv[++r];
        } else if (strncmp(a, "--columns=", 10) == 0) {
            dst = &g_raw_columns; val = a + 10;
        } else if (strncmp(a, "--sort=", 7) == 0) {
            dst = &g_raw_sort; val = a + 7;
        }
        if (dst) { *dst = val; continue; }
        argv[w++] = argv[r];
    }
    *argc = w;
    argv[w] = NULL;
    return 0;
}

int cli_out_reject(const char *command) {
    if (!g_raw_columns && !g_raw_sort) return 0;
    fprintf(stderr, "Error: %s is only for the commands that print a table; '%s' prints none.\n",
            g_raw_columns ? "--columns" : "--sort", command);
    return -1;
}

int cli_out_requested(void) {
    return g_raw_columns != NULL || g_raw_sort != NULL;
}

void cli_columns_all(int ncols, CliColumns *out) {
    out->set = 0;
    out->n = ncols < CLI_MAX_COLS ? ncols : CLI_MAX_COLS;
    for (int i = 0; i < out->n; i++) out->idx[i] = i;
}

static const char *col_header(const CliCol *c) {
    return c->header ? c->header : c->name;
}

/* The column named @p name (n bytes, case-insensitive), or -1. */
static int col_find(const CliCol *cols, int ncols, const char *name, size_t n) {
    for (int i = 0; i < ncols; i++) {
        if (strlen(cols[i].name) == n && strncasecmp(cols[i].name, name, n) == 0)
            return i;
        if (cols[i].alias && strlen(cols[i].alias) == n &&
            strncasecmp(cols[i].alias, name, n) == 0)
            return i;
    }
    return -1;
}

static void err_unknown(char *err, size_t errsz, const char *opt, const char *name,
                        size_t n, const CliCol *cols, int ncols) {
    size_t off = (size_t)snprintf(err, errsz, "unknown column '%.*s' for %s; valid columns: ",
                                  (int)n, name, opt);
    for (int i = 0; i < ncols && off < errsz; i++) {
        char up[64];
        size_t k = 0;
        for (const char *p = cols[i].name; *p && k + 1 < sizeof up; p++)
            up[k++] = (char)toupper((unsigned char)*p);
        up[k] = '\0';
        off += (size_t)snprintf(err + off, errsz - off, "%s%s", i ? ", " : "", up);
    }
}

int cli_columns_parse(const char *spec, const CliCol *cols, int ncols,
                      CliColumns *out, char *err, size_t errsz) {
    out->set = 1;
    out->n = 0;
    const char *p = spec;
    for (;;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 0) {
            snprintf(err, errsz, "empty column name in --columns '%s'", spec);
            return -1;
        }
        int ci = col_find(cols, ncols, p, n);
        if (ci < 0) {
            err_unknown(err, errsz, "--columns", p, n, cols, ncols);
            return -1;
        }
        if (out->n >= CLI_MAX_COLS) {
            snprintf(err, errsz, "too many columns in --columns");
            return -1;
        }
        out->idx[out->n++] = ci;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int cli_sort_parse(const char *spec, const CliCol *cols, int ncols,
                   CliSort *out, char *err, size_t errsz) {
    out->n = 0;
    const char *p = spec;
    for (;;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 0) {
            snprintf(err, errsz, "empty column name in --sort '%s'", spec);
            return -1;
        }
        const char *colon = memchr(p, ':', n);
        size_t cn = colon ? (size_t)(colon - p) : n;
        int desc = 0;
        if (colon) {
            size_t dn = n - cn - 1;
            if (dn == 3 && strncasecmp(colon + 1, "asc", 3) == 0) desc = 0;
            else if (dn == 4 && strncasecmp(colon + 1, "desc", 4) == 0) desc = 1;
            else {
                snprintf(err, errsz, "bad direction '%.*s' in --sort (use :asc or :desc)",
                         (int)dn, colon + 1);
                return -1;
            }
        }
        int ci = col_find(cols, ncols, p, cn);
        if (ci < 0) {
            err_unknown(err, errsz, "--sort", p, cn, cols, ncols);
            return -1;
        }
        if (out->n >= CLI_MAX_COLS) {
            snprintf(err, errsz, "too many keys in --sort");
            return -1;
        }
        out->key[out->n].col = ci;
        out->key[out->n].desc = desc;
        out->n++;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int cli_out_resolve(const CliCol *cols, int ncols, CliColumns *sel, CliSort *sort) {
    char err[512];
    cli_columns_all(ncols, sel);
    sort->n = 0;
    if (g_raw_columns && cli_columns_parse(g_raw_columns, cols, ncols, sel, err, sizeof err) != 0) {
        fprintf(stderr, "Error: %s\n", err);
        return -1;
    }
    if (g_raw_sort && cli_sort_parse(g_raw_sort, cols, ncols, sort, err, sizeof err) != 0) {
        fprintf(stderr, "Error: %s\n", err);
        return -1;
    }
    return 0;
}

/* ── comparison ──────────────────────────────────────────────────────── */

static int cmp_int(long a, long b) { return a < b ? -1 : a > b ? 1 : 0; }

static int all_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return 0;
    return 1;
}

static int cmp_text(const char *a, const char *b) {
    /* a number sorts as a number, also in a text column (a UID) */
    if (all_digits(a) && all_digits(b)) {
        while (*a == '0' && a[1]) a++;
        while (*b == '0' && b[1]) b++;
        size_t la = strlen(a), lb = strlen(b);
        if (la != lb) return la < lb ? -1 : 1;
        int d = strcmp(a, b);
        return d < 0 ? -1 : d > 0;
    }
    int c = strcasecmp(a, b);
    return c ? c : strcmp(a, b);
}

static int cmp_cell(const CliCol *c, const char *a, const char *b) {
    switch (c->kind) {
    case CLI_COL_NUM: {
        double x = strtod(a, NULL), y = strtod(b, NULL);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    case CLI_COL_BOOL:
        return cmp_int(a[0] == 't', b[0] == 't');
    case CLI_COL_VOCAB: {
        int ia = 0, ib = 0, n = 0;
        if (c->vocab) {
            for (; c->vocab[n]; n++) {
                if (strcasecmp(c->vocab[n], a) == 0) ia = n + 1;
                if (strcasecmp(c->vocab[n], b) == 0) ib = n + 1;
            }
        }
        /* an unknown value sorts after every known one */
        if (!ia) ia = n + 1;
        if (!ib) ib = n + 1;
        return cmp_int(ia, ib);
    }
    case CLI_COL_KEY: {
        const char *da = strrchr(a, '-'), *db = strrchr(b, '-');
        if (!da || !db) return cmp_text(a, b);
        size_t la = (size_t)(da - a), lb = (size_t)(db - b);
        size_t m = la < lb ? la : lb;
        int pc = strncasecmp(a, b, m);
        if (pc) return pc;
        if (la != lb) return la < lb ? -1 : 1;
        return cmp_int(strtol(da + 1, NULL, 10), strtol(db + 1, NULL, 10));
    }
    case CLI_COL_TEXT:
    default:
        return cmp_text(a, b);
    }
}

int cli_row_compare(const CliCol *cols, const CliSort *sort, CliCellFn get,
                    void *ctx, int a, int b) {
    char ba[CLI_CELL_MAX], bb[CLI_CELL_MAX];
    for (int k = 0; k < sort->n; k++) {
        int ci = sort->key[k].col;
        ba[0] = bb[0] = '\0';
        get(ctx, a, ci, ba, sizeof ba);
        get(ctx, b, ci, bb, sizeof bb);
        int c = cmp_cell(&cols[ci], ba, bb);
        if (c) return sort->key[k].desc ? -c : c;
    }
    return 0;
}

typedef struct {
    const CliCol  *cols;
    const CliSort *sort;
    CliCellFn      get;
    void          *ctx;
} SortCtx;

static SortCtx g_sc;

static int sort_cmp(const void *pa, const void *pb) {
    int a = *(const int *)pa, b = *(const int *)pb;
    int c = cli_row_compare(g_sc.cols, g_sc.sort, g_sc.get, g_sc.ctx, a, b);
    return c ? c : (a < b ? -1 : a > b ? 1 : 0);   /* stable */
}

void cli_sort_rows(int *order, int nrows, const CliCol *cols, const CliSort *sort,
                   CliCellFn get, void *ctx) {
    if (!sort || sort->n == 0 || nrows < 2) return;
    g_sc = (SortCtx){ cols, sort, get, ctx };
    qsort(order, (size_t)nrows, sizeof(int), sort_cmp);
}

/* ── printing ────────────────────────────────────────────────────────── */

/* Visible width: one column per code point. */
static int vis_width(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) w++;
    return w;
}

void cli_table_print(const CliCol *cols, const CliColumns *sel, const int *order,
                     int from, int to, CliCellFn get, void *ctx) {
    int w[CLI_MAX_COLS];
    for (int c = 0; c < sel->n; c++) w[c] = vis_width(col_header(&cols[sel->idx[c]]));
    for (int r = from; r < to; r++) {
        int row = order ? order[r] : r;
        for (int c = 0; c < sel->n; c++) {
            char buf[CLI_CELL_MAX];
            buf[0] = '\0';
            get(ctx, row, sel->idx[c], buf, sizeof buf);
            int vw = vis_width(buf);
            if (vw > w[c]) w[c] = vw;
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        fputs("  ", stdout);
        for (int c = 0; c < sel->n; c++) {
            const CliCol *col = &cols[sel->idx[c]];
            const char *h = col_header(col);
            int right = col->kind == CLI_COL_NUM;
            if (c) fputs("  ", stdout);
            if (pass == 0) {
                int pad = w[c] - vis_width(h);
                if (right) printf("%*s%s", pad, "", h);
                else if (c == sel->n - 1) fputs(h, stdout);
                else printf("%s%*s", h, pad, "");
            } else {
                for (int i = 0; i < w[c]; i++) fputs("\xe2\x95\x90", stdout);
            }
        }
        fputc('\n', stdout);
    }
    for (int r = from; r < to; r++) {
        int row = order ? order[r] : r;
        fputs("  ", stdout);
        for (int c = 0; c < sel->n; c++) {
            const CliCol *col = &cols[sel->idx[c]];
            char buf[CLI_CELL_MAX];
            buf[0] = '\0';
            get(ctx, row, sel->idx[c], buf, sizeof buf);
            int pad = w[c] - vis_width(buf);
            if (c) fputs("  ", stdout);
            if (col->kind == CLI_COL_NUM) printf("%*s%s", pad, "", buf);
            else if (c == sel->n - 1) fputs(buf, stdout);
            else printf("%s%*s", buf, pad, "");
        }
        fputc('\n', stdout);
    }
}

void cli_json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout);  break;
        case '\r': fputs("\\r", stdout);  break;
        case '\t': fputs("\\t", stdout);  break;
        default:
            if (*p < 0x20) printf("\\u%04x", *p);
            else putchar(*p);
        }
    }
    putchar('"');
}

static int is_number(const char *s) {
    if (!*s) return 0;
    char *end;
    strtod(s, &end);
    return *end == '\0';
}

void cli_json_print(const CliCol *cols, const CliColumns *sel, const int *order,
                    int from, int to, CliCellFn get, void *ctx) {
    fputs("[", stdout);
    for (int r = from; r < to; r++) {
        int row = order ? order[r] : r;
        printf("%s\n  {", r > from ? "," : "");
        for (int c = 0; c < sel->n; c++) {
            const CliCol *col = &cols[sel->idx[c]];
            char buf[CLI_CELL_MAX];
            buf[0] = '\0';
            get(ctx, row, sel->idx[c], buf, sizeof buf);
            if (c) fputs(", ", stdout);
            char key[64];
            size_t k = 0;
            for (const char *p = col->name; *p && k + 1 < sizeof key; p++)
                key[k++] = (char)tolower((unsigned char)*p);
            key[k] = '\0';
            cli_json_string(key);
            fputs(": ", stdout);
            if (col->kind == CLI_COL_BOOL && (!strcmp(buf, "true") || !strcmp(buf, "false")))
                fputs(buf, stdout);
            else if (col->kind == CLI_COL_NUM && is_number(buf))
                fputs(buf, stdout);
            else
                cli_json_string(buf);
        }
        fputc('}', stdout);
    }
    fputs(to > from ? "\n]\n" : "]\n", stdout);
}
