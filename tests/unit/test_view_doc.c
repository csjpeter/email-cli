/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-11-22 */
/* Unit tests of the view descriptions (d on the help page, EMAIL-323). */
#include "test_helpers.h"
#include "key_table.h"
#include "view_doc.h"

#include <string.h>

void test_view_doc(void) {
    const char *views[] = { EKV_LIST_IMAP, EKV_LIST_GMAIL, EKV_LIST_GMAIL_VIRTUAL,
        EKV_LIST_GMAIL_ARCHIVE, EKV_LIST_GMAIL_TRASH, EKV_READ_IMAP, EKV_READ_GMAIL,
        EKV_LABELS, EKV_LABELS_SYSTEM, EKV_FOLDERS, EKV_REVIEW, EKV_ATTACH,
        EKV_ACCOUNTS, EKV_ACCOUNTS_GMAIL, EKV_RULES, EKV_RULE,
        EKV_BODY_EDITOR };
    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        const tui_viewdoc *d = evdoc_get(views[i]);
        ASSERT(d != NULL, "view doc: every view has a description");
        if (!d) continue;
        ASSERT(d->name && d->name[0], "view doc: a view has a name");
        ASSERT(d->statement && strlen(d->statement) > 20, "view doc: a view has a statement");
        for (int c = 0; c < d->ncols; c++)
            ASSERT(d->cols[c].title && d->cols[c].text && d->cols[c].text[0],
                   "view doc: a column has a title and a line");

        /* the column titles of the table the screen draws agree with the description */
        int n = 0;
        const tui_table_col *cols = evdoc_table_cols(views[i], &n);
        char msg[200] = "";
        ASSERT(tui_viewdoc_check(d, cols, n, msg, sizeof msg) == 0, msg);
        ASSERT(n == d->ncols, "view doc: as many described columns as table columns");

        /* the text of the description names every column */
        char text[4096];
        ASSERT(tui_viewdoc_text(d, text, sizeof text) > 0, "view doc: the text is built");
        for (int c = 0; c < d->ncols; c++)
            ASSERT(strstr(text, d->cols[c].title) && strstr(text, d->cols[c].text),
                   "view doc: the text has a line for each column");
    }
    ASSERT(evdoc_get("no-such-view") == NULL, "view doc: an unknown view has none");

    /* the tables of the views are the ones described: a changed title is caught */
    tui_table_col bad[4];
    memcpy(bad, EVD_LIST_COLS, sizeof bad);
    bad[2].title = "Topic";
    char msg[200] = "";
    ASSERT(tui_viewdoc_check(evdoc_get(EKV_LIST_IMAP), bad, 4, msg, sizeof msg) == -1 &&
           strstr(msg, "Topic"), "view doc: a renamed column is reported");
    ASSERT(tui_viewdoc_check(evdoc_get(EKV_LIST_IMAP), EVD_LIST_COLS, 3, msg, sizeof msg) == -1,
           "view doc: a column that is gone is reported");
    ASSERT(tui_viewdoc_check(evdoc_get(EKV_FOLDERS), EVD_FOLDERS_COLS, 4, msg, sizeof msg) == 0,
           "view doc: the folder browser agrees");
}
