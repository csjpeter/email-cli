/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-23 */
/* Unit tests of the shared --columns / --sort implementation (EMAIL-230). */
#include "test_helpers.h"
#include "test_scratch.h"
#include "cli_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static const char *const prio_vocab[] = { "CRITICAL", "HIGH", "MEDIUM", "LOW", NULL };

static const CliCol cols[] = {
    { "key",      "KEY",      NULL,  CLI_COL_KEY,   NULL },
    { "title",    "TITLE",    NULL,  CLI_COL_TEXT,  NULL },
    { "priority", "PRIORITY", "prio", CLI_COL_VOCAB, prio_vocab },
    { "count",    "COUNT",    NULL,  CLI_COL_NUM,   NULL },
    { "open",     "OPEN",     NULL,  CLI_COL_BOOL,  NULL },
    { "uid",      "UID",      NULL,  CLI_COL_TEXT,  NULL },
};
#define NC ((int)(sizeof(cols) / sizeof(cols[0])))

typedef struct { const char *v[6]; } Row;

static const Row rows[] = {
    { { "ABC-10", "ten",   "LOW",      "5",  "true",  "9"  } },
    { { "ABC-2",  "two",   "CRITICAL", "40", "false", "10" } },
    { { "ABC-1",  "one",   "MEDIUM",   "7",  "true",  "100" } },
    { { "ABC-33", "Alpha", "HIGH",     "7",  "false", "2"  } },
};
#define NR 4

static void cell(void *ctx, int row, int col, char *buf, size_t n) {
    (void)ctx;
    snprintf(buf, n, "%s", rows[row].v[col]);
}

/** Runs @p fn with stdout redirected to a file; returns the text in @p out. */
static void capture(void (*fn)(void), char *out, size_t size) {
    const char *path = TP("cli-table-capture.txt");
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    out[0] = '\0';
    if (fd < 0) return;
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    fn();
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    lseek(fd, 0, SEEK_SET);
    ssize_t r = read(fd, out, size - 1);
    out[r > 0 ? r : 0] = '\0';
    close(fd);
    unlink(path);
}

static CliColumns g_sel;
static int g_order[NR];

static void print_table(void) { cli_table_print(cols, &g_sel, g_order, 0, NR, cell, NULL); }
static void print_json(void)  { cli_json_print(cols, &g_sel, g_order, 0, NR, cell, NULL); }

static void sorted(const char *spec, int *order) {
    CliSort s;
    char err[200];
    for (int i = 0; i < NR; i++) order[i] = i;
    if (cli_sort_parse(spec, cols, NC, &s, err, sizeof err) != 0) return;
    cli_sort_rows(order, NR, cols, &s, cell, NULL);
}

static void test_columns_parse(void) {
    CliColumns c;
    char err[300];
    ASSERT(cli_columns_parse("title,KEY", cols, NC, &c, err, sizeof err) == 0, "valid list parses");
    ASSERT(c.set == 1 && c.n == 2 && c.idx[0] == 1 && c.idx[1] == 0,
           "columns keep the order given, names are case-insensitive");
    ASSERT(cli_columns_parse("prio", cols, NC, &c, err, sizeof err) == 0 && c.idx[0] == 2,
           "an alias is accepted");
    ASSERT(cli_columns_parse("title,bogus", cols, NC, &c, err, sizeof err) != 0,
           "an unknown name is refused");
    ASSERT(strstr(err, "unknown column 'bogus'") != NULL, "the error names the unknown column");
    ASSERT(strstr(err, "KEY") && strstr(err, "TITLE") && strstr(err, "PRIORITY") &&
           strstr(err, "UID"), "the error lists the valid names");
    ASSERT(cli_columns_parse("", cols, NC, &c, err, sizeof err) != 0, "an empty list is refused");
}

static void test_sort_parse(void) {
    CliSort s;
    char err[300];
    ASSERT(cli_sort_parse("priority:desc,key", cols, NC, &s, err, sizeof err) == 0, "valid sort");
    ASSERT(s.n == 2 && s.key[0].col == 2 && s.key[0].desc == 1 &&
           s.key[1].col == 0 && s.key[1].desc == 0, "directions and order parsed");
    ASSERT(cli_sort_parse("nope", cols, NC, &s, err, sizeof err) != 0, "unknown sort column refused");
    ASSERT(strstr(err, "valid columns") != NULL, "the error lists the valid names");
    ASSERT(cli_sort_parse("key:up", cols, NC, &s, err, sizeof err) != 0, "bad direction refused");
    ASSERT(strstr(err, "asc") && strstr(err, "desc"), "the error names the directions");
}

static void test_sort_orders(void) {
    int o[NR];
    sorted("priority", o);   /* CRITICAL, HIGH, MEDIUM, LOW */
    ASSERT(o[0] == 1 && o[1] == 3 && o[2] == 2 && o[3] == 0,
           "priority sorts CRITICAL, HIGH, MEDIUM, LOW (not alphabetically)");
    sorted("priority:desc", o);
    ASSERT(o[0] == 0 && o[3] == 1, "priority descending reverses the vocabulary order");
    sorted("key", o);        /* ABC-1, ABC-2, ABC-10, ABC-33 */
    ASSERT(o[0] == 2 && o[1] == 1 && o[2] == 0 && o[3] == 3, "keys sort numerically");
    sorted("count", o);      /* 5, 7, 7, 40 : stable for the tie */
    ASSERT(o[0] == 0 && o[1] == 2 && o[2] == 3 && o[3] == 1, "numbers sort as numbers, stably");
    sorted("count:desc,key", o);
    ASSERT(o[0] == 1 && o[1] == 2 && o[2] == 3 && o[3] == 0, "a second key breaks the tie");
    sorted("uid", o);        /* digit strings compare as numbers: 2, 9, 10, 100 */
    ASSERT(o[0] == 3 && o[1] == 0 && o[2] == 1 && o[3] == 2, "all-digit text sorts numerically");
    sorted("title", o);      /* case-insensitive: Alpha, one, ten, two */
    ASSERT(o[0] == 3 && o[1] == 2 && o[2] == 0 && o[3] == 1, "text sorts without regard to case");
    sorted("open,key", o);   /* false before true */
    ASSERT(o[0] == 1 && o[1] == 3 && o[2] == 2 && o[3] == 0, "booleans sort false first");
}

static void test_table_columns(void) {
    char out[2048];
    cli_columns_parse("uid,key", cols, NC, &g_sel, out, sizeof out);
    sorted("priority", g_order);   /* sort column is not displayed */
    capture(print_table, out, sizeof out);
    char *l1 = strtok(out, "\n");
    char *l2 = strtok(NULL, "\n");
    char *l3 = strtok(NULL, "\n");
    ASSERT(l1 && strncmp(l1, "  UID", 5) == 0 && strstr(l1, "KEY") && !strstr(l1, "TITLE"),
           "header has only the chosen columns, in the chosen order");
    ASSERT(l2 && strstr(l2, "\xe2\x95\x90"), "a rule of box characters under the header");
    ASSERT(l3 && strstr(l3, "10") && strstr(l3, "ABC-2"),
           "first row is the CRITICAL one although PRIORITY is not shown");
}

static void test_json_columns(void) {
    char out[2048];
    cli_columns_parse("count,key,open", cols, NC, &g_sel, out, sizeof out);
    sorted("key", g_order);
    capture(print_json, out, sizeof out);
    ASSERT(strstr(out, "{\"count\": 7, \"key\": \"ABC-1\", \"open\": true}") != NULL,
           "json has only the chosen fields, in order; numbers and booleans bare");
    ASSERT(strstr(out, "title") == NULL && strstr(out, "priority") == NULL,
           "json carries no unchosen field");
    ASSERT(strstr(out, "ABC-1\"") < strstr(out, "ABC-33\""), "json rows follow --sort");
    cli_columns_all(NC, &g_sel);
    ASSERT(g_sel.n == NC && g_sel.idx[0] == 0, "cli_columns_all selects every column");
}

static void test_extract(void) {
    char *argv[] = { "email-cli", "list", "--columns", "uid,from", "--limit", "3",
                     "--sort=date:desc", NULL };
    int argc = 7;
    ASSERT(cli_out_extract(&argc, argv) == 0, "extract succeeds");
    ASSERT(argc == 4 && strcmp(argv[2], "--limit") == 0 && argv[4] == NULL,
           "the options are taken out of argv");
    ASSERT(cli_out_requested() == 1, "requested after extract");
    CliColumns c;
    CliSort s;
    ASSERT(cli_out_resolve(cols, NC, &c, &s) != 0 || c.set, "resolve runs on the remembered text");
    cli_out_set_raw(NULL, NULL);
    ASSERT(cli_out_requested() == 0, "cleared");
    ASSERT(cli_out_reject("mark-read") == 0, "no option, no rejection");
    cli_out_set_raw("a", NULL);
    ASSERT(cli_out_reject("mark-read") != 0, "--columns on a command with no table is refused");
    cli_out_set_raw(NULL, NULL);
    char *bad[] = { "email-cli", "list", "--sort", NULL };
    int bc = 3;
    ASSERT(cli_out_extract(&bc, bad) != 0, "a missing value is an error");
    cli_out_set_raw(NULL, NULL);
}

void test_cli_table(void) {
    test_columns_parse();
    test_sort_parse();
    test_sort_orders();
    test_table_columns();
    test_json_columns();
    test_extract();
}
