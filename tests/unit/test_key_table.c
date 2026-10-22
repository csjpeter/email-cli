/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-19 */
/* Unit tests of email-tui's key table: the keyboard policy as a table. */
#include "test_helpers.h"
#include "key_table.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static EmailAction find(const char *view, int ch) {
    return ekeys_find(view, TERM_KEY_IGNORE, ch);
}

static int menu_has(const char *view, EmailAction a) {
    return ekeys_menu_has(view, a);
}

void test_key_table(void) {
    char why[200] = "";
    ASSERT(ekeys_init(why, sizeof(why)) == 0, "key table: passes the policy check");
    ASSERT(why[0] == '\0', "key table: no reason given");

    /* every view with a help overlay has a description of one to three lines
     * (64 columns each), EMAIL-181 */
    const char *views[] = { EKV_LIST_IMAP, EKV_LIST_GMAIL, EKV_LIST_GMAIL_VIRTUAL,
        EKV_LIST_GMAIL_ARCHIVE, EKV_LIST_GMAIL_TRASH, EKV_READ_IMAP, EKV_READ_GMAIL,
        EKV_LABELS, EKV_LABELS_SYSTEM, EKV_FOLDERS, EKV_REVIEW, EKV_ATTACH,
        EKV_ACCOUNTS, EKV_RULES, EKV_RULE, EKV_CONDITIONS, EKV_BODY_EDITOR };
    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        const char *d = ekeys_view_desc(views[i]);
        ASSERT(d != NULL && d[0], "key table: a view has a description");
        ASSERT(d == NULL || strlen(d) <= 3 * 64, "key table: a description fits three lines");
    }
    ASSERT(ekeys_view_desc("no-such-view") == NULL, "key table: unknown view has none");

    /* the registry keys, in both kinds of list */
    const char *lists[] = { EKV_LIST_IMAP, EKV_LIST_GMAIL, EKV_READ_IMAP, EKV_READ_GMAIL };
    for (int i = 0; i < 4; i++) {
        ASSERT(find(lists[i], '*') == EA_STAR, "key table: * stars");
        ASSERT(find(lists[i], '+') == EA_READ, "key table: + marks read");
        ASSERT(find(lists[i], '-') == EA_UNREAD, "key table: - marks unread");
        ASSERT(find(lists[i], '!') == EA_JUNK, "key table: ! toggles junk");
        ASSERT(find(lists[i], 'd') == EA_DELETE, "key table: d deletes");
        ASSERT(ekeys_find(lists[i], TERM_KEY_DELETE, 0) == EA_DELETE,
               "key table: Delete deletes");
    }

    /* the letters that used to write to the server are not bound */
    const char *old = "fnDjuasmkh";
    for (int i = 0; i < 4; i++)
        for (const char *c = old; *c; c++)
            ASSERT(find(lists[i], *c) == EA_NONE, "key table: an old state-changing letter is unbound");

    /* EMAIL-156: no key stands for "save all" or for a second label removal;
     * both are in the ':' menu, in IMAP and Gmail readers alike */
    ASSERT(find(EKV_READ_IMAP, 'A') == EA_NONE, "key table: A is no key in the IMAP reader");
    ASSERT(find(EKV_READ_GMAIL, 'A') == EA_NONE, "key table: A is no key in the Gmail reader");
    ASSERT(find(EKV_READ_GMAIL, 'r') == EA_NONE, "key table: r removes no label in the Gmail reader");
    ASSERT(find(EKV_READ_GMAIL, 'a') == EA_NONE, "key table: a is a plain key (save), not an action");

    /* EMAIL-206: the Gmail lists differ in what the menu offers */
    ASSERT(menu_has(EKV_LIST_GMAIL_TRASH, EA_RESTORE), "key table: the Trash menu restores");
    ASSERT(!menu_has(EKV_LIST_GMAIL, EA_RESTORE), "key table: a label view cannot restore");
    ASSERT(!menu_has(EKV_LIST_GMAIL_VIRTUAL, EA_RESTORE), "key table: Unread cannot restore");
    ASSERT(!menu_has(EKV_LIST_GMAIL_ARCHIVE, EA_RESTORE), "key table: the Archive cannot restore");
    ASSERT(menu_has(EKV_LIST_GMAIL, EA_ARCHIVE), "key table: a label view archives");
    ASSERT(menu_has(EKV_LIST_GMAIL_VIRTUAL, EA_ARCHIVE), "key table: Unread archives");
    ASSERT(menu_has(EKV_LIST_GMAIL_TRASH, EA_ARCHIVE), "key table: the Trash archives");
    ASSERT(!menu_has(EKV_LIST_GMAIL_ARCHIVE, EA_ARCHIVE), "key table: the Archive cannot archive");
    ASSERT(menu_has(EKV_LIST_GMAIL, EA_RM_LABEL), "key table: a label view takes its label off");
    ASSERT(!menu_has(EKV_LIST_GMAIL_VIRTUAL, EA_RM_LABEL), "key table: Unread has no label to take off");
    ASSERT(!menu_has(EKV_LIST_GMAIL_ARCHIVE, EA_RM_LABEL), "key table: the Archive has no label to take off");
    ASSERT(!menu_has(EKV_LIST_GMAIL_TRASH, EA_RM_LABEL), "key table: the Trash has no label to take off");
    const char *glists[] = { EKV_LIST_GMAIL, EKV_LIST_GMAIL_VIRTUAL, EKV_LIST_GMAIL_ARCHIVE, EKV_LIST_GMAIL_TRASH };
    for (int i = 0; i < 4; i++) {
        ASSERT(find(glists[i], 't') == EA_LABELS, "key table: t is the label picker in every Gmail list");
        ASSERT(find(glists[i], 'd') == EA_DELETE, "key table: d deletes in every Gmail list");
        ASSERT(find(glists[i], 'x') == EA_NONE, "key table: x is no action");
        ASSERT(menu_has(glists[i], EA_ADD_LABEL), "key table: every Gmail list adds a label");
        ASSERT(menu_has(glists[i], EA_SYNC), "key table: every Gmail list starts a sync");
        ASSERT(ekeys_help_has(glists[i], TUI_KEY_HOME) && ekeys_help_has(glists[i], '/'),
               "key table: every Gmail list has the list keys");
    }

    /* the labels picker is Gmail's alone */
    ASSERT(find(EKV_LIST_GMAIL, 't') == EA_LABELS, "key table: t is the Gmail label picker");
    ASSERT(find(EKV_LIST_IMAP, 't') == EA_NONE, "key table: t is unbound on IMAP");

    /* the folder browser (EMAIL-129): creating and deleting are in the menu only */
    ASSERT(find(EKV_FOLDERS, 'c') == EA_NONE, "key table: c composes, creates no folder");
    ASSERT(find(EKV_FOLDERS, 'd') == EA_NONE, "key table: d does not delete a folder");
    ASSERT(find(EKV_FOLDERS, 'n') == EA_NONE, "key table: n does not create a folder");
    ASSERT(ekeys_find(EKV_FOLDERS, TERM_KEY_DELETE, 0) == EA_NONE,
           "key table: Delete does not delete a folder");
    /* EMAIL-199: t (tree / flat) is a row of the folder help and of the menu */
    ASSERT(find(EKV_FOLDERS, 't') == EA_FOLDER_VIEW, "key table: t is the folder view switch");
    ASSERT(ekeys_help_has(EKV_FOLDERS, 't'), "key table: the folder help lists t");
    ASSERT(ekeys_help_has(EKV_FOLDERS, 'h'), "key table: the folder help lists h (EMAIL-197)");
    ASSERT(ekeys_help_has(EKV_FOLDERS, 'l'), "key table: the folder help lists l (EMAIL-197)");

    /* the label browser and the review screen have no direct state keys but e */
    ASSERT(find(EKV_LABELS, 'c') == EA_NONE, "key table: c does not create a label");
    ASSERT(find(EKV_LABELS, 'd') == EA_NONE, "key table: d does not delete a label");
    /* EMAIL-218: only a label of the user's own can be deleted from the menu */
    ASSERT(ekeys_menu_has(EKV_LABELS, EA_LABEL_DELETE), "key table: the menu deletes a user label");
    ASSERT(!ekeys_menu_has(EKV_LABELS_SYSTEM, EA_LABEL_DELETE), "key table: the menu does not delete a system label");
    ASSERT(ekeys_menu_has(EKV_LABELS, EA_LABEL_CREATE) && ekeys_menu_has(EKV_LABELS_SYSTEM, EA_LABEL_CREATE),
           "key table: the menu creates a label on every row");
    ASSERT(ekeys_help_has(EKV_LABELS_SYSTEM, TUI_KEY_HOME), "key table: the system label view has the same help");
    ASSERT(find(EKV_ACCOUNTS_GMAIL, 'r') == EA_NONE, "key table: r does not renew an authorization");
    ASSERT(find(EKV_ACCOUNTS_GMAIL, ':') == EA_NONE, "key table: ':' on the accounts screen is a harmless key");
    /* EMAIL-147: the attachment picker has keys, none of them changes state */
    for (const char *c = "jkgGq?a"; *c; c++)
        ASSERT(find(EKV_ATTACH, *c) == EA_NONE, "key table: a picker key changes no state");
    ASSERT(find(EKV_REVIEW, 's') == EA_NONE, "key table: s does not send");
    ASSERT(find(EKV_REVIEW, 'd') == EA_NONE, "key table: d does not remove an attachment");
    ASSERT(find(EKV_REVIEW, 'e') == EA_EDIT_HEADERS, "key table: e edits the headers");

    /* a key the table does not know changes nothing */
    ASSERT(find(EKV_LIST_IMAP, 'Z') == EA_NONE, "key table: an unknown key is nothing");
    ASSERT(ekeys_find(EKV_LIST_IMAP, TERM_KEY_ESC, 0) == EA_NONE, "key table: Esc is not an action");

    /* EMAIL-158: the help is made from the table, so a key the handler of a
     * view has must be a row of that view's table (and so of its help). */
    const char *movers[] = { EKV_LIST_IMAP, EKV_LIST_GMAIL, EKV_READ_IMAP, EKV_READ_GMAIL,
                             EKV_LABELS, EKV_FOLDERS, EKV_ACCOUNTS_GMAIL };
    for (int i = 0; i < 7; i++) {
        ASSERT(ekeys_help_has(movers[i], TUI_KEY_HOME), "key table: help lists Home");
        ASSERT(ekeys_help_has(movers[i], TUI_KEY_END), "key table: help lists End");
        ASSERT(ekeys_help_has(movers[i], TUI_KEY_PGUP), "key table: help lists PgUp");
        ASSERT(ekeys_help_has(movers[i], TUI_KEY_PGDN), "key table: help lists PgDn");
        ASSERT(ekeys_help_has(movers[i], '?'), "key table: help lists ?");
    }
    const char *finders[] = { EKV_LIST_IMAP, EKV_LIST_GMAIL, EKV_READ_IMAP, EKV_READ_GMAIL,
                              EKV_LABELS, EKV_FOLDERS };
    for (int i = 0; i < 6; i++)
        ASSERT(ekeys_help_has(finders[i], '/'), "key table: help lists / (filter, search)");
    ASSERT(ekeys_help_has(EKV_REVIEW, '?'), "key table: the review screen has a help");
    ASSERT(!ekeys_help_has(EKV_FOLDERS, 'd'), "key table: help lists no key that does nothing");
}

/* EMAIL-225: every entry of every ':' menu has a unique quick key, and
 * docs/spec/tui-keys.md lists each of them with its letter and class. */
static char g_doc[200000];

static char *read_doc(void) {
    const char *paths[] = { "docs/spec/tui-keys.md", "../docs/spec/tui-keys.md",
                            "../../docs/spec/tui-keys.md" };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        size_t n = fread(g_doc, 1, sizeof g_doc - 1, f);
        g_doc[n] = '\0';
        fclose(f);
        return g_doc;
    }
    return NULL;
}

void test_key_table_menu_quick_keys(void) {
    ASSERT(ekeys_init(NULL, 0) == 0, "menu keys: key table is ready");
    const char *views[] = { EKV_LIST_IMAP, EKV_LIST_GMAIL, EKV_LIST_GMAIL_VIRTUAL,
        EKV_LIST_GMAIL_ARCHIVE, EKV_LIST_GMAIL_TRASH, EKV_READ_IMAP, EKV_READ_GMAIL,
        EKV_LABELS, EKV_LABELS_SYSTEM, EKV_FOLDERS, EKV_REVIEW, EKV_ACCOUNTS_GMAIL };
    char *doc = read_doc();
    ASSERT(doc != NULL, "menu keys: docs/spec/tui-keys.md can be read");
    int total = 0;
    for (size_t v = 0; v < sizeof views / sizeof views[0] && getenv("EMENU_DUMP"); v++) {
        EkeysMenuRow rows[64];
        int n = ekeys_menu_rows(views[v], rows, 64);
        for (int i = 0; i < n; i++)
            printf("| `%s` | `%c` | %s | %s |\n", views[v], rows[i].letter, rows[i].name, rows[i].cls);
    }
    for (size_t v = 0; v < sizeof views / sizeof views[0]; v++) {
        EkeysMenuRow rows[64];
        int n = ekeys_menu_rows(views[v], rows, 64);
        ASSERT(n > 0, "menu keys: a view has a menu");
        total += n;
        for (int i = 0; i < n; i++) {
            ASSERT(rows[i].letter != 0, "menu keys: every entry has a quick key");
            for (int j = i + 1; j < n; j++)
                ASSERT(rows[i].letter != rows[j].letter, "menu keys: quick keys are unique in a menu");
            if (!doc) continue;
            char row[300];
            snprintf(row, sizeof row, "| `%s` | `%c` | %s | %s |", views[v], rows[i].letter,
                     rows[i].name, rows[i].cls);
            ASSERT(strstr(doc, row) != NULL, "menu keys: the docs list the entry, its letter and class");
        }
    }
    ASSERT(total > 30, "menu keys: the menus have their entries");
}
