/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-11-22 */
/* The description of every view of email-tui: what it is for, what each column holds. */
#include "view_doc.h"
#include "key_table.h"
#include "tui_frame.h"

#include <stdio.h>
#include <string.h>

/* The columns of the tables.  The views take these arrays, so a title is
 * written once; the descriptions below name the same titles. */
const tui_table_col EVD_LIST_COLS[4] = {
    { "Date",    16, 0, TUI_ALIGN_LEFT, 0, 0, 0 },
    { "Sts",      6, 0, TUI_ALIGN_LEFT, 0, 0, 0 },
    { "Subject", 10, 3, TUI_ALIGN_LEFT, 0, 0, 0 },
    { "From",    10, 2, TUI_ALIGN_LEFT, 0, 0, 0 },
};
const tui_table_col EVD_FOLDERS_COLS[4] = {
    { "Unread",  6, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
    { "Flagged", 7, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
    { "Folder", 20, 1, TUI_ALIGN_LEFT,  0, 0, 0 },
    { "Total",   7, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
};
const tui_table_col EVD_LABELS_COLS[2] = {
    { "Count",  6, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
    { "Label", 20, 1, TUI_ALIGN_LEFT,  0, 0, 0 },
};
const tui_table_col EVD_ACCOUNTS_COLS[6] = {
    { "Unread",   6, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
    { "Flagged",  7, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
    { "Account", 32, 0, TUI_ALIGN_LEFT,  0, 0, 0 },
    { "Type",     5, 0, TUI_ALIGN_LEFT,  0, 0, 0 },
    /* host:port is cut at the start, so the port stays visible */
    { "Server",  10, 1, TUI_ALIGN_LEFT,  0, 0, TUI_COL_CUT_START },
    { "Send via", 8, 1, TUI_ALIGN_LEFT,  0, 0, TUI_COL_CUT_START },
};
const tui_table_col EVD_RULES_COLS[6] = {
    { "#",           3, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
    { "Rule",        8, 1, TUI_ALIGN_LEFT,  0, 0, 0 },
    { "What it does", 20, 5, TUI_ALIGN_LEFT, 0, 0, 0 },
    { "On",          3, 0, TUI_ALIGN_LEFT,  0, 0, 0 },
    { "Matches",     7, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
    { "Last change", 16, 0, TUI_ALIGN_LEFT, 0, 0, 0 },
};
const tui_table_col EVD_ATTACH_COLS[3] = {
    { "Name", 20, 3, TUI_ALIGN_LEFT,  0, 0, 0 },
    { "Type", 12, 2, TUI_ALIGN_LEFT,  0, 0, 0 },
    { "Size",  8, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
};

/* The line of each column: what it holds. */
static const tui_viewdoc_col DOC_LIST[4] = {
    { "Date",    "when the message was sent" },
    { "Sts",     "six marks: P phishing, J junk or N new; star; D done; A attachment; "
                 "R replied or F forwarded; v/x the sender check passed or failed; - none" },
    { "Subject", "the subject of the message" },
    { "From",    "the sender" },
};
static const tui_viewdoc_col DOC_FOLDERS[4] = {
    { "Unread",  "the unread messages in the folder and its subfolders" },
    { "Flagged", "the starred messages in the folder and its subfolders" },
    { "Folder",  "the name of the folder (the first rows are views across every folder)" },
    { "Total",   "all the messages in the folder and its subfolders" },
};
static const tui_viewdoc_col DOC_LABELS[2] = {
    { "Count", "the messages that carry the label" },
    { "Label", "the name of the label; the system labels come first" },
};
static const tui_viewdoc_col DOC_ACCOUNTS[6] = {
    { "Unread",   "the unread messages of the account" },
    { "Flagged",  "the starred messages of the account" },
    { "Account",  "the user name of the account" },
    { "Type",     "IMAP or Gmail" },
    { "Server",   "the mail server and port the account reads from" },
    { "Send via", "the server that sends the mail; dim when none is set" },
};
static const tui_viewdoc_col DOC_RULES[6] = {
    { "#",            "the place of the rule: the rules run in this order" },
    { "Rule",         "the name of the rule" },
    { "What it does", "its conditions and its action in one line" },
    { "On",           "on, or off when the rule is switched off" },
    { "Matches",      "how many messages the rule has matched so far" },
    { "Last change",  "when the rule last changed a message, or never" },
};
static const tui_viewdoc_col DOC_ATTACH[3] = {
    { "Name", "the file name of the attachment" },
    { "Type", "its content type" },
    { "Size", "its size" },
};

typedef struct {
    const char          *view;
    tui_viewdoc          doc;
    const tui_table_col *tcols;   /* what the screen draws; NULL: no table */
    const char          *extra;   /* appended to the key table's description */
} Entry;

#define COLS(a) (a), (int)(sizeof(a) / sizeof((a)[0]))
#define LIST(v, n) { v, { n, NULL, COLS(DOC_LIST), NULL, 0 }, EVD_LIST_COLS, \
    "The Sts column holds the marks of the message; the status line under the list "  \
    "names the key of every action." }
#define NOCOLS NULL, 0

static Entry g_entries[] = {
    LIST(EKV_LIST_IMAP, "Message list"),
    LIST(EKV_LIST_GMAIL, "Message list (Gmail)"),
    LIST(EKV_LIST_GMAIL_VIRTUAL, "Message list (Gmail)"),
    LIST(EKV_LIST_GMAIL_ARCHIVE, "Message list (Gmail)"),
    LIST(EKV_LIST_GMAIL_TRASH, "Message list (Gmail)"),
    { EKV_READ_IMAP,  { "Message reader", NULL, NOCOLS, NULL, 0 }, NULL,
      "Not a table: the top shows the headers (From, To, Date, Subject), "
      "under them the text of the message, at the end the list of attachments." },
    { EKV_READ_GMAIL, { "Message reader (Gmail)", NULL, NOCOLS, NULL, 0 }, NULL,
      "Not a table: the top shows the headers (From, To, Date, Subject), "
      "under them the text of the message, at the end the list of attachments." },
    { EKV_LABELS,        { "Label browser", NULL, COLS(DOC_LABELS), NULL, 0 }, EVD_LABELS_COLS, NULL },
    { EKV_LABELS_SYSTEM, { "Label browser", NULL, COLS(DOC_LABELS), NULL, 0 }, EVD_LABELS_COLS, NULL },
    { EKV_FOLDERS,       { "Folder browser", NULL, COLS(DOC_FOLDERS), NULL, 0 }, EVD_FOLDERS_COLS, NULL },
    { EKV_REVIEW,        { "Review", NULL, NOCOLS, NULL, 0 }, NULL,
      "Not a table: the headers (From, To, Cc, Subject, attachments) come first, "
      "then the body exactly as it will be sent." },
    { EKV_ATTACH,        { "Attachment picker", NULL, COLS(DOC_ATTACH), NULL, 0 }, EVD_ATTACH_COLS, NULL },
    { EKV_ACCOUNTS,      { "Accounts", NULL, COLS(DOC_ACCOUNTS), NULL, 0 }, EVD_ACCOUNTS_COLS, NULL },
    { EKV_ACCOUNTS_GMAIL, { "Accounts", NULL, COLS(DOC_ACCOUNTS), NULL, 0 }, EVD_ACCOUNTS_COLS, NULL },
    { EKV_RULES,         { "Rules", NULL, COLS(DOC_RULES), NULL, 0 }, EVD_RULES_COLS, NULL },
    { EKV_RULE,          { "Rule form", NULL, NOCOLS, NULL, 0 }, NULL,
      "Not a table: a label and its value in each row, in sections for the name, "
      "the conditions, the actions and the preview of the cached messages the rule matches." },
    { EKV_BODY_EDITOR,   { "Body editor", NULL, NOCOLS, NULL, 0 }, NULL,
      "Not a table: the text of the message, with the cursor in it." },
};
#define N_ENTRIES ((int)(sizeof g_entries / sizeof g_entries[0]))

static char g_statement[N_ENTRIES][512];

static Entry *find(const char *view) {
    for (int i = 0; view && i < N_ENTRIES; i++)
        if (strcmp(g_entries[i].view, view) == 0) {
            Entry *e = &g_entries[i];
            if (!e->doc.statement) {
                const char *d = ekeys_view_desc(view);
                if (!d && strcmp(view, EKV_ACCOUNTS_GMAIL) == 0)   /* the same screen */
                    d = ekeys_view_desc(EKV_ACCOUNTS);
                snprintf(g_statement[i], sizeof g_statement[i], "%s%s%s", d ? d : "",
                         d && e->extra ? " " : "", e->extra ? e->extra : "");
                e->doc.statement = g_statement[i];
            }
            return e;
        }
    return NULL;
}

const tui_viewdoc *evdoc_get(const char *view) {
    Entry *e = find(view);
    return e ? &e->doc : NULL;
}

const tui_table_col *evdoc_table_cols(const char *view, int *n) {
    Entry *e = find(view);
    if (n) *n = e && e->tcols ? e->doc.ncols : 0;
    return e ? e->tcols : NULL;
}

void evdoc_help(const char *view, const char *title, const char *const *keys, int npairs) {
    (void)title;
    Entry *e = find(view);
    if (!e) return;
    tui_viewdoc doc = e->doc;       /* the keys are this call's */
    doc.keys = keys;
    doc.nkeys = npairs;
    static const char *const common[] = {
        "d", "On this page: what the view is and what each column means",
    };
    tui_viewdoc_page p;
    if (tui_viewdoc_open(&p, &doc, common, 1) != 0) return;
    tui_frame_run_viewdoc(&p);
    tui_viewdoc_free(&p);
}
