/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-19 */
/* Unit tests of email-tui's key table: the keyboard policy as a table. */
#include "test_helpers.h"
#include "key_table.h"
#include <string.h>

static EmailAction find(const char *view, int ch) {
    return ekeys_find(view, TERM_KEY_IGNORE, ch);
}

void test_key_table(void) {
    char why[200] = "";
    ASSERT(ekeys_init(why, sizeof(why)) == 0, "key table: passes the policy check");
    ASSERT(why[0] == '\0', "key table: no reason given");

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

    /* the labels picker is Gmail's alone */
    ASSERT(find(EKV_LIST_GMAIL, 't') == EA_LABELS, "key table: t is the Gmail label picker");
    ASSERT(find(EKV_LIST_IMAP, 't') == EA_NONE, "key table: t is unbound on IMAP");

    /* the folder browser (EMAIL-129): creating and deleting are in the menu only */
    ASSERT(find(EKV_FOLDERS, 'c') == EA_NONE, "key table: c composes, creates no folder");
    ASSERT(find(EKV_FOLDERS, 'd') == EA_NONE, "key table: d does not delete a folder");
    ASSERT(find(EKV_FOLDERS, 'n') == EA_NONE, "key table: n does not create a folder");
    ASSERT(ekeys_find(EKV_FOLDERS, TERM_KEY_DELETE, 0) == EA_NONE,
           "key table: Delete does not delete a folder");

    /* the label browser and the review screen have no direct state keys but e */
    ASSERT(find(EKV_LABELS, 'c') == EA_NONE, "key table: c does not create a label");
    ASSERT(find(EKV_LABELS, 'd') == EA_NONE, "key table: d does not delete a label");
    ASSERT(find(EKV_ACCOUNTS_GMAIL, 'r') == EA_NONE, "key table: r does not renew an authorization");
    ASSERT(find(EKV_ACCOUNTS_GMAIL, ':') == EA_NONE, "key table: ':' on the accounts screen is a harmless key");
    ASSERT(find(EKV_REVIEW, 's') == EA_NONE, "key table: s does not send");
    ASSERT(find(EKV_REVIEW, 'd') == EA_NONE, "key table: d does not remove an attachment");
    ASSERT(find(EKV_REVIEW, 'e') == EA_EDIT_HEADERS, "key table: e edits the headers");

    /* a key the table does not know changes nothing */
    ASSERT(find(EKV_LIST_IMAP, 'Z') == EA_NONE, "key table: an unknown key is nothing");
    ASSERT(ekeys_find(EKV_LIST_IMAP, TERM_KEY_ESC, 0) == EA_NONE, "key table: Esc is not an action");
}
