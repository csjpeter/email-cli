/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-11-22 */
/* The description of every view of email-tui: what it is for, what each column holds. */
#ifndef CORE_VIEW_DOC_H
#define CORE_VIEW_DOC_H

#include <tuikit/table.h>
#include <tuikit/viewdoc.h>

/**
 * @file view_doc.h
 * @brief The view descriptions the d key of the help page opens (EMAIL-323).
 *
 * policies/tui-interface-model.md, "The help page and the view description".
 * Every view of email-tui (the ekeys view names of key_table.h) has one
 * entry here: a statement of what the view is for and, for a table, one
 * line for each column.  The tables of the views take their columns from the
 * arrays below, so the title a column shows and the title its description
 * names are written once; a unit test compares them with tui_viewdoc_check().
 */

/** The message list (every list view, IMAP and Gmail): Date, Sts, Subject, From. */
extern const tui_table_col EVD_LIST_COLS[4];
/** The IMAP folder browser: Unread, Flagged, Folder, Total. */
extern const tui_table_col EVD_FOLDERS_COLS[4];
/** The Gmail label browser: Count, Label. */
extern const tui_table_col EVD_LABELS_COLS[2];
/** The accounts screen: Unread, Flagged, Account, Type, Server, Send via. */
extern const tui_table_col EVD_ACCOUNTS_COLS[6];
/** The rules overview: #, Rule, What it does, On, Matches, Last change. */
extern const tui_table_col EVD_RULES_COLS[6];
/** The attachment picker: Name, Type, Size. */
extern const tui_table_col EVD_ATTACH_COLS[3];

/**
 * @brief The description of @p view (an EKV_* name), without its keys; NULL
 *        for a name that is no view.  The statement is the key table's
 *        description of the view followed by what its parts mean.
 */
const tui_viewdoc *evdoc_get(const char *view);

/**
 * @brief The table columns of @p view (the ones its screen draws), or NULL
 *        with *n == 0 for a view without a table.  For the tests.
 */
const tui_table_col *evdoc_table_cols(const char *view, int *n);

/**
 * @brief The help page of @p view: @p npairs rows of key and text in @p keys,
 *        the line that reminds of d, then the view's description on d, and
 *        Esc closes.  Returns when it is closed.
 */
void evdoc_help(const char *view, const char *title, const char *const *keys, int npairs);

#endif /* CORE_VIEW_DOC_H */
