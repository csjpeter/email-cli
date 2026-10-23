/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-19 */
/* The key table of email-tui: one table for the key handler, the help and the ':' menu. */
#include "key_table.h"
#include "key_events.h"
#include "tui_frame.h"

#include <tuikit/actions.h>
#include <tuikit/dialog.h>
#include <tuikit/event.h>
#include <tuikit/tty.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* A key that changes nothing outside the view. */
#define H(V, K, N, D) \
    { (V), (tui_keycode)(K), TUI_VERB_NONE, (N), (D), TUI_ACT_HARMLESS, 0, NULL, EA_NONE }
/* A routine, reversible change on a registry key: no confirmation. */
#define R(V, K, VB, N, D, ID) \
    { (V), (tui_keycode)(K), (VB), (N), (D), TUI_ACT_ROUTINE, 0, NULL, (ID) }
/* A serious change: a dialog naming the target, No as the default. */
#define S(V, K, VB, N, D, CONF, ID) \
    { (V), (tui_keycode)(K), (VB), (N), (D), TUI_ACT_SERIOUS, 1, (CONF), (ID) }
/* A rare or unusual change: the menu only. */
#define M(V, N, D, ID) \
    { (V), TUI_KC_NONE, TUI_VERB_NONE, (N), (D), TUI_ACT_RARE, 0, NULL, (ID) }

/* A reversible change that only the menu offers (no registry verb fits): it
 * runs on its quick key, like a routine one (EMAIL-225). */
#define RM(V, N, D, ID) \
    { (V), TUI_KC_NONE, TUI_VERB_NONE, (N), (D), TUI_ACT_ROUTINE, 0, NULL, (ID) }
/* The description of a view: a row with no key and no action (libtuikit,
 * "The description of a view"); the help overlay shows it above the keys. */
#define DESC(V, T) \
    { (V), TUI_KC_NONE, TUI_VERB_NONE, NULL, (T), TUI_ACT_HARMLESS, 0, NULL, EA_NONE }

#define D_LIST \
    "The messages of one folder, newest first. The cursor is on one message. " \
    "Enter reads it; space marks several to act on together."
#define D_READER \
    "One message, rendered as text. The keys scroll it, search in it and reply to it; " \
    "q goes back to the list."

#define CONF_DELETE "%s?"
#define CONF_SYNC   "Start a background sync of %s?"

/* The keys the message list has in both kinds of account. */
#define LIST_COMMON(V)                                                              \
    H(V, 'j', "Down", "move the cursor down (also Down)"),                          \
    H(V, 'k', "Up", "move the cursor up (also Up)"),                                \
    H(V, TUI_KEY_PGDN, "Page down", NULL),                                          \
    H(V, TUI_KEY_PGUP, "Page up", NULL),                                            \
    H(V, TUI_KEY_HOME, "First message", "also g"),                                  \
    H(V, TUI_KEY_END, "Last message", "also G"),                                    \
    H(V, TUI_KEY_ENTER, "Open", "read the message"),                                \
    H(V, '/', "Filter", "type to filter; Esc clears it"),                           \
    H(V, ' ', "Mark", "mark the message and step down"),                            \
    H(V, 'x', "Clear marks", NULL),                                                 \
    H(V, 'c', "Compose", "a new message"),                                          \
    H(V, 'r', "Reply", NULL),                                                       \
    H(V, 'A', "Reply-all", NULL),                                                   \
    H(V, 'F', "Forward", NULL),                                                     \
    H(V, 'U', "Refresh", "after a sync"),                                           \
    H(V, 'l', "Rules", "the rules editor"),                                         \
    H(V, 'H', "History", "recent changes, with Revert"),                            \
    R(V, '*', TUI_VERB_STAR, "Star", "toggle the star (flag)", EA_STAR),            \
    R(V, '+', TUI_VERB_READ, "Mark read", NULL, EA_READ),                           \
    R(V, '-', TUI_VERB_UNREAD, "Mark unread", NULL, EA_UNREAD),                     \
    R(V, '!', TUI_VERB_JUNK, "Junk", "toggle junk", EA_JUNK),                       \
    S(V, 'd', TUI_VERB_DELETE, "Delete", "to Trash; in Trash for good",             \
      CONF_DELETE, EA_DELETE),                                                      \
    S(V, TUI_KC_DELETE, TUI_VERB_DELETE, "Delete (Delete key)", NULL,               \
      CONF_DELETE, EA_DELETE),                                                      \
    S(V, TUI_KC_NONE, TUI_VERB_NONE, "Start a sync", "in the background",           \
      CONF_SYNC, EA_SYNC),                                                          \
    M(V, "Move to...", "a folder or label, asked for", EA_MOVE),                    \
    H(V, '?', "Help", NULL),                                                        \
    H(V, ':', "Actions", "the menu of everything that changes state"),              \
    H(V, 'q', "Back", "also h, Left and Backspace"),                                           \
    H(V, TUI_KEY_ESC, "Back", "first clears marks and filter"),                     \
    H(V, TUI_KC_QUIT, "Quit", NULL)

/* The entries only the Gmail list has (EMAIL-206). */
#define GMAIL_LIST_T(V)                                                             \
    R(V, 't', TUI_VERB_MOVE, "Labels", "pick the labels of the message", EA_LABELS)
#define GMAIL_LIST_ARCHIVE(V)                                                       \
    RM(V, "Archive", "remove every label", EA_ARCHIVE)
#define GMAIL_LIST_RESTORE(V)                                                       \
    RM(V, "Restore from Trash", "back to the inbox", EA_RESTORE)
#define GMAIL_LIST_RM_LABEL(V)                                                      \
    RM(V, "Remove this label", "from the message", EA_RM_LABEL)
#define GMAIL_LIST_LABELS(V)                                                        \
    M(V, "Add a label...", "to the messages, asked for", EA_ADD_LABEL),             \
    M(V, "Remove a label...", "from the messages, asked for", EA_REMOVE_LABEL)

#define READER_COMMON(V)                                                            \
    H(V, 'j', "Down", "scroll a line (also Down)"),                                 \
    H(V, 'k', "Up", "scroll a line (also Up)"),                                     \
    H(V, TUI_KEY_PGDN, "Page down", NULL),                                          \
    H(V, TUI_KEY_PGUP, "Page up", NULL),                                            \
    H(V, TUI_KEY_HOME, "Top", "also g"),                                            \
    H(V, TUI_KEY_END, "Bottom", "also G"),                                         \
    H(V, 'r', "Reply", NULL),                                                       \
    H(V, 'A', "Reply-all", NULL),                                                   \
    H(V, 'F', "Forward", NULL),                                                     \
    H(V, 'v', "Source", "toggle rendered / raw source"),                            \
    H(V, '/', "Search", "in the message body"),                                     \
    H(V, 'N', "Previous match", "n: next match"),                                   \
    H(V, 'a', "Save attachment", NULL),                                             \
    H(V, 'H', "History", "of this message, with Revert"),                           \
    R(V, '*', TUI_VERB_STAR, "Star", "toggle the star (flag)", EA_STAR),            \
    R(V, '+', TUI_VERB_READ, "Mark read", NULL, EA_READ),                           \
    R(V, '-', TUI_VERB_UNREAD, "Mark unread", NULL, EA_UNREAD),                     \
    R(V, '!', TUI_VERB_JUNK, "Junk", "toggle junk", EA_JUNK),                       \
    S(V, 'd', TUI_VERB_DELETE, "Delete", "to Trash; in Trash for good",             \
      CONF_DELETE, EA_DELETE),                                                      \
    S(V, TUI_KC_DELETE, TUI_VERB_DELETE, "Delete (Delete key)", NULL,               \
      CONF_DELETE, EA_DELETE),                                                      \
    M(V, "Save all attachments", "to a directory, asked for", EA_SAVE_ALL),         \
    H(V, '?', "Help", NULL),                                                        \
    H(V, ':', "Actions", "the menu of everything that changes state"),              \
    H(V, 'q', "Back", "to the list (also h, Left, Backspace)"),                     \
    H(V, TUI_KEY_ESC, "Back", "to the list"),                                       \
    H(V, TUI_KC_QUIT, "Quit", NULL)

/* The entries of the Gmail label browser, for a label of the user's own and
 * for a system label alike (EMAIL-208, EMAIL-209, EMAIL-218). */
#define LABELS_COMMON(V)                                                            \
    H(V, 'j', "Down", "move down (also Down)"),                                     \
    H(V, 'k', "Up", "move up (also Up)"),                                           \
    H(V, TUI_KEY_PGDN, "Page down", NULL),                                          \
    H(V, TUI_KEY_PGUP, "Page up", NULL),                                            \
    H(V, TUI_KEY_HOME, "First label", "also g"),                                    \
    H(V, TUI_KEY_END, "Last label", "also G"),                                      \
    H(V, TUI_KEY_ENTER, "Open", "the messages of the label"),                       \
    H(V, '/', "Search", "cross-folder content search"),                             \
    H(V, '?', "Help", NULL),                                                        \
    H(V, ':', "Actions", "the menu of everything that changes state"),              \
    H(V, 'q', "Back", "to the accounts (also h, Left, Backspace)"),                         \
    H(V, TUI_KEY_ESC, "Back", "to the accounts"),                                   \
    M(V, "Create a label...", "on the server, name asked for", EA_LABEL_CREATE)

static const tui_keyentry g_entries[] = {
    /* The message list of an IMAP account. */
    DESC(EKV_LIST_IMAP, D_LIST),
    LIST_COMMON(EKV_LIST_IMAP),
    RM(EKV_LIST_IMAP, "Toggle Done", "the $Done flag", EA_DONE),

    /* The message list of a Gmail account.  The menu lists what applies at
     * the cursor and no more (EMAIL-206): Restore from Trash only in the
     * Trash, Archive in every view but the Archive, Remove this label only
     * in the view of a real label.  The view follows the list's folder. */
    DESC(EKV_LIST_GMAIL, D_LIST),
    LIST_COMMON(EKV_LIST_GMAIL),
    GMAIL_LIST_T(EKV_LIST_GMAIL),
    GMAIL_LIST_ARCHIVE(EKV_LIST_GMAIL),
    GMAIL_LIST_RM_LABEL(EKV_LIST_GMAIL),
    GMAIL_LIST_LABELS(EKV_LIST_GMAIL),

    /* A view that is no label: Unread, Flagged, a search result. */
    DESC(EKV_LIST_GMAIL_VIRTUAL,
         "The messages of a Gmail view that is no label (Unread, Flagged or a search result). "
         "The cursor is on one message; Enter reads it."),
    LIST_COMMON(EKV_LIST_GMAIL_VIRTUAL),
    GMAIL_LIST_T(EKV_LIST_GMAIL_VIRTUAL),
    GMAIL_LIST_ARCHIVE(EKV_LIST_GMAIL_VIRTUAL),
    GMAIL_LIST_LABELS(EKV_LIST_GMAIL_VIRTUAL),

    /* The Archive: a message there is archived already and has no label. */
    DESC(EKV_LIST_GMAIL_ARCHIVE,
         "The archived messages of the Gmail account: they have no label. "
         "The cursor is on one message; Enter reads it."),
    LIST_COMMON(EKV_LIST_GMAIL_ARCHIVE),
    GMAIL_LIST_T(EKV_LIST_GMAIL_ARCHIVE),
    GMAIL_LIST_LABELS(EKV_LIST_GMAIL_ARCHIVE),

    /* The Trash: the only view that can restore. */
    DESC(EKV_LIST_GMAIL_TRASH,
         "The messages in the Gmail Trash. The cursor is on one message; "
         "Enter reads it, the : menu restores it to the inbox."),
    LIST_COMMON(EKV_LIST_GMAIL_TRASH),
    GMAIL_LIST_T(EKV_LIST_GMAIL_TRASH),
    GMAIL_LIST_ARCHIVE(EKV_LIST_GMAIL_TRASH),
    GMAIL_LIST_RESTORE(EKV_LIST_GMAIL_TRASH),
    GMAIL_LIST_LABELS(EKV_LIST_GMAIL_TRASH),

    /* The message reader. */
    DESC(EKV_READ_IMAP, D_READER),
    READER_COMMON(EKV_READ_IMAP),
    RM(EKV_READ_IMAP, "Toggle Done", "the $Done flag", EA_DONE),
    DESC(EKV_READ_GMAIL, D_READER),
    READER_COMMON(EKV_READ_GMAIL),
    R(EKV_READ_GMAIL, 't', TUI_VERB_MOVE, "Labels", "pick the labels of the message",
      EA_LABELS),
    RM(EKV_READ_GMAIL, "Archive", "remove every label", EA_ARCHIVE),
    RM(EKV_READ_GMAIL, "Remove this label", "from the message", EA_RM_LABEL),

    /* The Gmail label browser.  On a label of the user's own the menu also
     * deletes it; on a system label (Unread, Inbox, Trash...) it cannot be
     * deleted, so the menu does not offer it (EMAIL-218). */
    DESC(EKV_LABELS,
         "The labels of the Gmail account. The cursor is on one of your own labels; "
         "Enter opens its messages, the : menu can delete it."),
    LABELS_COMMON(EKV_LABELS),
    S(EKV_LABELS, TUI_KC_NONE, TUI_VERB_NONE, "Delete the label", "on the server",
      "Delete the label %s on the server? Its messages keep their other labels.",
      EA_LABEL_DELETE),
    DESC(EKV_LABELS_SYSTEM,
         "The labels of the Gmail account. The cursor is on a system label, "
         "which cannot be deleted; Enter opens its messages."),
    LABELS_COMMON(EKV_LABELS_SYSTEM),

    /* The IMAP folder browser (EMAIL-129): creating and deleting a folder are
     * rare, so they are in the menu only; deleting names the folder and asks.
     * 't' (tree / flat) is a registry key (move): the table refuses it as a
     * harmless key, so the browser handles it outside the table. */
    DESC(EKV_FOLDERS,
         "The folders of the IMAP account, as a tree or one level at a time. "
         "The cursor is on one folder; Enter opens its messages."),
    H(EKV_FOLDERS, 'j', "Down", "move down (also Down)"),
    H(EKV_FOLDERS, 'k', "Up", "move up (also Up)"),
    H(EKV_FOLDERS, TUI_KEY_PGDN, "Page down", NULL),
    H(EKV_FOLDERS, TUI_KEY_PGUP, "Page up", NULL),
    H(EKV_FOLDERS, TUI_KEY_HOME, "First row", "also g"),
    H(EKV_FOLDERS, TUI_KEY_END, "Last row", "also G"),
    H(EKV_FOLDERS, TUI_KEY_ENTER, "Open", "the folder, or step into its subfolders"),
    H(EKV_FOLDERS, 'l', "Open branch", "tree: open; flat view: as Enter (Right)"),
    H(EKV_FOLDERS, 'h', "Close", "tree: close, or go up; flat view: one level up"),
    H(EKV_FOLDERS, '/', "Search", "cross-folder content search"),
    H(EKV_FOLDERS, 'c', "Compose", "a new message"),
    H(EKV_FOLDERS, '?', "Help", NULL),
    H(EKV_FOLDERS, ':', "Actions", "the menu of everything that changes state"),
    H(EKV_FOLDERS, 'q', "Back", "up one level; the accounts at the top (also Backspace)"),
    H(EKV_FOLDERS, TUI_KEY_ESC, "Back", "up one level; the accounts at the top"),
    M(EKV_FOLDERS, "Create a folder...", "on the server, name asked for", EA_FOLDER_CREATE),
    S(EKV_FOLDERS, TUI_KC_NONE, TUI_VERB_NONE, "Delete the folder", "on the server",
      "Delete the folder %s on the server? Its messages are lost there.",
      EA_FOLDER_DELETE),
    RM(EKV_FOLDERS, "Open all folders", "tree: every branch", EA_FOLDER_OPEN_ALL),
    RM(EKV_FOLDERS, "Close all folders", "tree: every branch", EA_FOLDER_CLOSE_ALL),
    R(EKV_FOLDERS, 't', TUI_VERB_MOVE, "Tree / flat", "switch between the folder tree and one level at a time",
      EA_FOLDER_VIEW),

    /* The accounts screen, a Gmail account under the cursor (EMAIL-130).  The
     * other keys of that screen (n, d, i, e) are the screen's own. */
    DESC(EKV_ACCOUNTS,
         "The accounts of the program. The cursor is on one account; "
         "Enter opens it, n adds a new one."),
    DESC(EKV_RULES,
         "The mail rules that sort incoming mail. The cursor is on one rule; "
         "Enter opens it, a adds a new one."),
    DESC(EKV_RULE,
         "One mail rule: its conditions and its actions. "
         "e edits it, d deletes it."),
    DESC(EKV_CONDITIONS,
         "The conditions of the rule being edited. The cursor is on one condition; "
         "Enter edits it, Tab goes on to the actions."),
    DESC(EKV_BODY_EDITOR,
         "The body of the message being composed, as plain text. "
         "Esc saves it and goes on to the review."),
    H(EKV_ACCOUNTS_GMAIL, 'j', "Down", "move down (also Down)"),
    H(EKV_ACCOUNTS_GMAIL, 'k', "Up", "move up (also Up)"),
    H(EKV_ACCOUNTS_GMAIL, TUI_KEY_PGDN, "Page down", NULL),
    H(EKV_ACCOUNTS_GMAIL, TUI_KEY_PGUP, "Page up", NULL),
    H(EKV_ACCOUNTS_GMAIL, TUI_KEY_HOME, "First account", NULL),
    H(EKV_ACCOUNTS_GMAIL, TUI_KEY_END, "Last account", NULL),
    H(EKV_ACCOUNTS_GMAIL, TUI_KEY_ENTER, "Open", "the labels of the account"),
    H(EKV_ACCOUNTS_GMAIL, '?', "Help", NULL),
    H(EKV_ACCOUNTS_GMAIL, ':', "Actions", "the menu of everything that changes state"),
    H(EKV_ACCOUNTS_GMAIL, TUI_KEY_ESC, "Quit", NULL),
    /* rare, and it replaces the saved token: a dialog names the account (EMAIL-225) */
    { EKV_ACCOUNTS_GMAIL, TUI_KC_NONE, TUI_VERB_NONE, "Renew the authorization...",
      "Gmail access, in the browser or by paste", TUI_ACT_RARE, 1,
      "Renew the Gmail authorization of %s? The saved token is replaced.", EA_REAUTH },

    /* The attachment picker of the reader (EMAIL-147): no key changes state. */
    DESC(EKV_ATTACH,
         "The attachments of the message. The cursor is on one; "
         "Enter chooses it and asks where to save it."),
    H(EKV_ATTACH, 'j', "Down", "move down (also Down)"),
    H(EKV_ATTACH, 'k', "Up", "move up (also Up)"),
    H(EKV_ATTACH, TUI_KEY_PGDN, "Page down", NULL),
    H(EKV_ATTACH, TUI_KEY_PGUP, "Page up", NULL),
    H(EKV_ATTACH, TUI_KEY_HOME, "First attachment", "also g"),
    H(EKV_ATTACH, TUI_KEY_END, "Last attachment", "also G"),
    H(EKV_ATTACH, TUI_KEY_ENTER, "Choose", "then asked where to save it"),
    H(EKV_ATTACH, '?', "Help", NULL),
    H(EKV_ATTACH, 'q', "Back", "to the message"),
    H(EKV_ATTACH, TUI_KEY_ESC, "Back", "to the message"),
    H(EKV_ATTACH, TUI_KEY_BACKSPACE, "Back", "to the message"),
    H(EKV_ATTACH, TUI_KC_QUIT, "Quit", NULL),

    /* The review of a composed message. */
    DESC(EKV_REVIEW,
         "The composed message, ready to send. Check the headers and the body; "
         "e, b and a change them, the : menu sends the message."),
    S(EKV_REVIEW, TUI_KC_NONE, TUI_VERB_NONE, "Send", "the message",
      "Send the message to %s?", EA_SEND),
    R(EKV_REVIEW, 'e', TUI_VERB_EDIT, "Edit headers", "To, Cc, Bcc, Subject, attachments",
      EA_EDIT_HEADERS),
    H(EKV_REVIEW, 'b', "Edit body", NULL),
    H(EKV_REVIEW, 'a', "Add attachment", NULL),
    H(EKV_REVIEW, '?', "Help", NULL),
    H(EKV_REVIEW, ':', "Actions", "send the message, remove an attachment"),
    H(EKV_REVIEW, 'q', "Discard", "asks first"),
    H(EKV_REVIEW, TUI_KEY_ESC, "Discard", "asks first"),
    RM(EKV_REVIEW, "Remove an attachment", NULL, EA_UNATTACH),
};

#define N_ENTRIES ((int)(sizeof g_entries / sizeof g_entries[0]))

static tui_keytable g_table;
static int g_ready;

int ekeys_init(char *why, size_t size) {
    if (g_ready) return 0;
    if (tui_keytable_init(&g_table, g_entries, N_ENTRIES, why, size) != 0) return -1;
    g_ready = 1;
    return 0;
}

EmailAction ekeys_find(const char *view, TermKey key, int ch) {
    if (!g_ready && ekeys_init(NULL, 0) != 0) return EA_NONE;
    tui_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = TUI_EV_KEY;
    if (key == TERM_KEY_DELETE) {
        ev.key = TUI_KEY_DELETE;
    } else if (key == TERM_KEY_IGNORE && ch > 0) {
        ev.key = TUI_KEY_CHAR;
        ev.cp = (uint32_t)ch;
    } else {
        return EA_NONE;
    }
    int i = tui_keytable_find(&g_table, view, &ev);
    return i < 0 ? EA_NONE : (EmailAction)g_entries[i].id;
}

static void draw_menu(tui_canvas c, void *ctx) {
    tui_actionmenu_render(ctx, c);
}

/* Runs the menu or the confirmation @p m over the screen until it is decided.
 * @p targets (may be NULL) gives each action its own target: the menu has one
 * target for all its confirmations, so it is set to the chosen row's before
 * Enter is handled. */
static void run_actionmenu(tui_actionmenu *m, const char *const targets[EA_COUNT],
                           const char *letters) {
    tui_decoder dec;
    tui_decoder_init(&dec);
    KeyEvents winch;
    key_events_begin(&winch);
    while (m->state == TUI_ACTION_OPEN) {
        int rows = terminal_rows(), cols = terminal_cols();
        if (rows <= 0) rows = 24;
        if (cols <= 0) cols = 80;
        if (m->confirming) {
            static const char *const v[][2] = {
                { "y", "yes" }, { "n", "no" }, { "Esc/q/BS", "cancel" } };
            tui_frame_modal_status(v, 3, 0, 0);
        } else {
            static const char *const v[][2] = {
                { "letter", "run" }, { "j/k", "select" }, { "Enter", "choose" },
                { "Esc/q/BS", "close" } };
            tui_frame_modal_status(v, 4, m->dialog.list.selected + 1, m->dialog.nitems);
        }
        tui_frame_overlay(draw_menu, m, rows, cols);
        tui_event ev;
        int r = key_events_next(STDIN_FILENO, &dec, &ev, -1);
        if (r <= 0 || ev.type == TUI_EV_HANGUP) {
            m->state = TUI_ACTION_CANCELLED;
            break;
        }
        if (ev.type == TUI_EV_RESIZE) continue;
        tui_frame_modal_key(&ev, !m->confirming);
        if (targets && !m->confirming && m->idx && m->dialog.list.selected >= 0) {
            int row = m->dialog.list.selected;
            /* a quick key runs the entry of its letter, not the row under the
             * cursor: name that entry's target in its confirmation */
            if (letters && ev.type == TUI_EV_KEY && ev.key == TUI_KEY_CHAR &&
                ev.mods == 0 && ev.cp > ' ' && ev.cp < 128) {
                for (int i = 0; i < g_table.n; i++)
                    if (letters[i] && (unsigned char)letters[i] == ev.cp) {
                        for (int k = 0; k < m->dialog.nitems; k++)
                            if (m->idx[k] == i) row = k;
                        break;
                    }
            }
            int id = g_entries[m->idx[row]].id;
            if (id > 0 && id < EA_COUNT && targets[id]) m->target = targets[id];
        }
        tui_actionmenu_handle(m, &ev);
    }
    key_events_end(&winch);
    tui_decoder_free(&dec);
}

EmailAction ekeys_menu(const char *view, const char *const targets[EA_COUNT]) {
    if (!g_ready && ekeys_init(NULL, 0) != 0) return EA_NONE;
    tui_actionmenu m;
    /* the quick keys are the library's assignment (TUI-62, EMAIL-225); the
     * same call gives the letters the menu draws, for the targets and the help */
    char letters[N_ENTRIES];
    if (tui_keytable_assign_quick(&g_table, view, NULL, letters) < 0) return EA_NONE;
    if (tui_actionmenu_open(&m, &g_table, view, "") != 0) return EA_NONE;
    run_actionmenu(&m, targets, letters);
    EmailAction a = m.state == TUI_ACTION_CHOSEN ? (EmailAction)g_entries[m.entry].id
                                                  : EA_NONE;
    tui_actionmenu_free(&m);
    return a;
}

int ekeys_confirm(const char *view, EmailAction a, const char *target) {
    if (!g_ready && ekeys_init(NULL, 0) != 0) return 0;
    int entry = -1;
    for (int i = 0; i < N_ENTRIES; i++) {
        if ((int)g_entries[i].id == (int)a && g_entries[i].view &&
            strcmp(g_entries[i].view, view) == 0) {
            entry = i;
            break;
        }
    }
    if (entry < 0) return 0;
    tui_actionmenu m;
    if (tui_actionmenu_run(&m, &g_table, entry, target ? target : "") != 0) return 0;
    run_actionmenu(&m, NULL, NULL);
    int yes = m.state == TUI_ACTION_CHOSEN;
    tui_actionmenu_free(&m);
    return yes;
}

const char *ekeys_view_desc(const char *view) {
    for (int i = 0; view && i < N_ENTRIES; i++) {
        const tui_keyentry *e = &g_entries[i];
        if (!e->action && e->key == TUI_KC_NONE && e->desc && e->view &&
            strcmp(e->view, view) == 0)
            return e->desc;
    }
    return NULL;
}

#define DESC_WIDTH 64
#define DESC_LINES 3

void ekeys_help_rows(const char *view, const char *title, const char *const rows[][2], int n) {
    if (n < 0 || n > 64) return;
    const char *pairs[2 * (64 + DESC_LINES + 1)];
    char lines[DESC_LINES][DESC_WIDTH + 1];
    int np = 0;
    const char *desc = ekeys_view_desc(view);
    if (desc) {
        /* the description, wrapped at words to the width libtuikit uses */
        int nl = 0;
        const char *p = desc;
        while (*p && nl < DESC_LINES) {
            while (*p == ' ') p++;
            size_t len = strlen(p);
            size_t take = len <= DESC_WIDTH ? len : DESC_WIDTH;
            if (take < len) {
                size_t k = take;
                while (k > 0 && p[k] != ' ') k--;
                if (k > 0) take = k;
            }
            memcpy(lines[nl], p, take);
            lines[nl][take] = '\0';
            pairs[np++] = "";
            pairs[np++] = lines[nl];
            nl++;
            p += take;
        }
        pairs[np++] = "";
        pairs[np++] = "";
    }
    for (int i = 0; i < n; i++) {
        pairs[np++] = rows[i][0];
        pairs[np++] = rows[i][1];
    }
    tui_dialog d;
    if (tui_dialog_help(&d, title, pairs, np / 2) != 0) return;
    tui_frame_run_dialog(&d);
    tui_dialog_free(&d);
}

/* The help of a table view is libtuikit's tui_keyhelp_open(), with the quick
 * key of each menu entry added beside its direct key (":d"): the letters come
 * from the same assignment the menu draws, so the help cannot list a letter
 * the menu does not have (EMAIL-225).  The dialog's rows are the table's
 * entries of the view, in table order; the key column of a row with a quick
 * key is replaced by a string of ours. */
#define HELP_ROWS 96
#define HELP_KEYCOL 40

void ekeys_help(const char *view, const char *title) {
    if (!g_ready && ekeys_init(NULL, 0) != 0) return;
    tui_keyhelp h;
    if (tui_keyhelp_open(&h, &g_table, view, title) != 0) return;
    char letters[N_ENTRIES];
    static char keys[HELP_ROWS][HELP_KEYCOL];
    if (tui_keytable_assign_quick(&g_table, view, NULL, letters) >= 0) {
        const char **pairs = (const char **)h.dialog.items;
        int k = 0;
        for (int i = 0; i < N_ENTRIES && k < h.dialog.nitems && k < HELP_ROWS; i++) {
            const tui_keyentry *e = &g_entries[i];
            if (!e->action || !(e->view == NULL || (view && strcmp(e->view, view) == 0)))
                continue;
            if (letters[i]) {
                if (e->key == TUI_KC_NONE)
                    snprintf(keys[k], HELP_KEYCOL, ":%c", letters[i]);
                else {
                    char direct[24];
                    tui_keycode_name(e->key, direct, sizeof direct);
                    snprintf(keys[k], HELP_KEYCOL, "%s, :%c", direct, letters[i]);
                }
                pairs[2 * k] = keys[k];
            }
            k++;
        }
    }
    tui_frame_run_dialog(&h.dialog);
    tui_keyhelp_free(&h);
}

/* The quick key of an entry of @p view's menu, 0 when it has none. */
char ekeys_quick_key(const char *view, EmailAction a) {
    if (!g_ready && ekeys_init(NULL, 0) != 0) return 0;
    char letters[N_ENTRIES];
    if (tui_keytable_assign_quick(&g_table, view, NULL, letters) < 0) return 0;
    for (int i = 0; i < N_ENTRIES; i++)
        if ((int)g_entries[i].id == (int)a && g_entries[i].view &&
            strcmp(g_entries[i].view, view) == 0 && g_entries[i].cls != TUI_ACT_HARMLESS)
            return letters[i];
    return 0;
}

int ekeys_help_has(const char *view, tui_keycode key) {
    for (int i = 0; i < N_ENTRIES; i++)
        if (g_entries[i].key == key && g_entries[i].view &&
            strcmp(g_entries[i].view, view) == 0)
            return 1;
    return 0;
}

int ekeys_menu_rows(const char *view, EkeysMenuRow *rows, int max) {
    if (!g_ready && ekeys_init(NULL, 0) != 0) return -1;
    char letters[N_ENTRIES];
    if (tui_keytable_assign_quick(&g_table, view, NULL, letters) < 0) return -1;
    int n = 0;
    for (int i = 0; i < N_ENTRIES && n < max; i++) {
        const tui_keyentry *e = &g_entries[i];
        if (!e->action || !e->view || strcmp(e->view, view) != 0 ||
            e->cls == TUI_ACT_HARMLESS)
            continue;
        rows[n].letter = letters[i];
        rows[n].name = e->action;
        rows[n].cls = e->cls == TUI_ACT_SERIOUS ? "serious"
                    : e->cls == TUI_ACT_RARE ? "rare" : "routine";
        rows[n].direct = e->key != TUI_KC_NONE;
        n++;
    }
    return n;
}

int ekeys_menu_has(const char *view, EmailAction a) {
    for (int i = 0; i < N_ENTRIES; i++)
        if ((int)g_entries[i].id == (int)a && g_entries[i].view &&
            strcmp(g_entries[i].view, view) == 0)
            return 1;
    return 0;
}
