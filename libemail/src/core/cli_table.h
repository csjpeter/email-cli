/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-23 */
/* Interface of the shared --columns / --sort implementation of the CLI tables. */
#ifndef CLI_TABLE_H
#define CLI_TABLE_H

#include <stddef.h>

/**
 * @file cli_table.h
 * @brief --columns and --sort for every command that prints rows (EMAIL-230).
 *
 * One implementation serves every table and tree of email-cli, email-cli-ro:
 * a command describes its columns (CliCol), the module parses the two options
 * against them, sorts the rows (vocabulary order, numeric keys, numbers,
 * text) and prints them as an aligned table or as JSON that carries only the
 * chosen fields (policies/cli-output-model.md).
 */

#define CLI_MAX_COLS 24
#define CLI_CELL_MAX 1024

/** How the values of a column compare and how --json prints them. */
typedef enum {
    CLI_COL_TEXT,    /**< text, compared case-insensitively */
    CLI_COL_NUM,     /**< a number, compared and printed as one */
    CLI_COL_KEY,     /**< PROJECT-12: project part, then the number */
    CLI_COL_VOCAB,   /**< a fixed vocabulary: sorts by its order */
    CLI_COL_BOOL     /**< "true" / "false", false first, printed bare in JSON */
} CliColKind;

/** One column of a command's table. */
typedef struct {
    const char        *name;    /**< field name (case-insensitive on the command line; the JSON key, lower case) */
    const char        *header;  /**< header text of the table; NULL = @c name */
    const char        *alias;   /**< a second accepted name, or NULL */
    CliColKind         kind;
    const char *const *vocab;   /**< CLI_COL_VOCAB: the values in order, NULL-terminated */
} CliCol;

/** The columns to print, in order. */
typedef struct {
    int set;                    /**< 1 when --columns was given */
    int n;
    int idx[CLI_MAX_COLS];      /**< indexes into the command's CliCol array */
} CliColumns;

/** One sort key. */
typedef struct { int col; int desc; } CliSortKey;

/** The sort keys, most significant first. */
typedef struct {
    int        n;
    CliSortKey key[CLI_MAX_COLS];
} CliSort;

/** Fills @p buf with the text of column @p col of row @p row. */
typedef void (*CliCellFn)(void *ctx, int row, int col, char *buf, size_t n);

/**
 * @brief Remember the raw option values of the command line (NULL = not given).
 *
 * The commands resolve them against their own columns with cli_out_resolve().
 */
void cli_out_set_raw(const char *columns, const char *sort);

/**
 * @brief Take `--columns V`, `--columns=V`, `--sort V` and `--sort=V` out of
 *        argv[1..*argc-1] (so that no command has to know them) and remember them.
 * @return 0, or -1 (message on stderr) when an option has no value.
 */
int cli_out_extract(int *argc, char **argv);

/**
 * @brief The usage error of a command that has no table: --columns / --sort given to it.
 * @return 0 when neither was given, else -1 with a message on stderr.
 */
int cli_out_reject(const char *command);

/** @brief 1 when --columns or --sort was given. */
int cli_out_requested(void);

/**
 * @brief Resolve the remembered --columns / --sort against @p cols.
 *
 * On a bad name prints "Error: ..." with the valid names to stderr.
 * @return 0 on success, -1 on a usage error (nothing to do but exit non-zero).
 */
int cli_out_resolve(const CliCol *cols, int ncols, CliColumns *sel, CliSort *sort);

/** @brief Parse @p spec ("A,B,C") against @p cols; 0 or -1 with @p err filled. */
int cli_columns_parse(const char *spec, const CliCol *cols, int ncols,
                      CliColumns *out, char *err, size_t errsz);

/** @brief Parse @p spec ("COL[:asc|:desc],...") against @p cols; 0 or -1 with @p err. */
int cli_sort_parse(const char *spec, const CliCol *cols, int ncols,
                   CliSort *out, char *err, size_t errsz);

/** @brief Compare rows @p a and @p b by @p sort (0 when the keys tie). */
int cli_row_compare(const CliCol *cols, const CliSort *sort, CliCellFn get,
                    void *ctx, int a, int b);

/**
 * @brief Order @p order[0..nrows-1] (row indexes) by @p sort; stable.
 *
 * With no sort keys the order is left as it is.
 */
void cli_sort_rows(int *order, int nrows, const CliCol *cols, const CliSort *sort,
                   CliCellFn get, void *ctx);

/**
 * @brief Print rows order[from..to-1] as an aligned table to stdout.
 *
 * Header, a rule of ═ under it, two-space indent, two spaces between the
 * columns, numbers right-aligned.  @p order may be NULL (rows in index order).
 */
void cli_table_print(const CliCol *cols, const CliColumns *sel, const int *order,
                     int from, int to, CliCellFn get, void *ctx);

/**
 * @brief Print rows order[from..to-1] as a JSON array to stdout.
 *
 * Only the chosen columns, in that order; numbers and booleans bare, the rest
 * as JSON strings.  Prints "[]\n" for no rows.
 */
void cli_json_print(const CliCol *cols, const CliColumns *sel, const int *order,
                    int from, int to, CliCellFn get, void *ctx);

/** @brief Write @p s as a JSON string (quotes included) to stdout. */
void cli_json_string(const char *s);

/** @brief Number of columns shown in the default view: all of @p ncols. */
void cli_columns_all(int ncols, CliColumns *out);

#endif /* CLI_TABLE_H */
