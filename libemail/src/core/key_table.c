/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-19 */
/* The key table of email-tui: one table for the key handler, the help and the ':' menu. */
#include "key_table.h"
#include "key_events.h"
#include "tui_frame.h"

#include <tuikit/actions.h>
#include <tuikit/event.h>
#include <tuikit/tty.h>
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

#define CONF_DELETE "%s?"
#define CONF_SYNC   "Start a background sync of %s?"

/* The keys the message list has in both kinds of account. */
#define LIST_COMMON(V)                                                              \
    H(V, 'j', "Down", "move the cursor down (also Down)"),                          \
    H(V, 'k', "Up", "move the cursor up (also Up)"),                                \
    H(V, TUI_KEY_PGDN, "Page down", NULL),                                          \
    H(V, TUI_KEY_PGUP, "Page up", NULL),                                            \
    H(V, TUI_KEY_HOME, "First message", NULL),                                      \
    H(V, TUI_KEY_END, "Last message", NULL),                                        \
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
    H(V, 'q', "Back", NULL),                                                        \
    H(V, TUI_KEY_ESC, "Back", "first clears marks and filter"),                     \
    H(V, TUI_KC_QUIT, "Quit", NULL)

#define READER_COMMON(V)                                                            \
    H(V, 'j', "Down", "scroll a line (also Down)"),                                 \
    H(V, 'k', "Up", "scroll a line (also Up)"),                                     \
    H(V, TUI_KEY_PGDN, "Page down", NULL),                                          \
    H(V, TUI_KEY_PGUP, "Page up", NULL),                                            \
    H(V, TUI_KEY_HOME, "Top", NULL),                                                \
    H(V, TUI_KEY_END, "Bottom", NULL),                                              \
    H(V, 'r', "Reply", NULL),                                                       \
    H(V, 'A', "Reply-all", NULL),                                                   \
    H(V, 'F', "Forward", NULL),                                                     \
    H(V, 'v', "Source", "toggle rendered / raw source"),                            \
    H(V, '/', "Search", "in the message body"),                                     \
    H(V, 'N', "Previous match", "n: next match"),                                   \
    H(V, 'a', "Save attachment", NULL),                                             \
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
    H(V, 'q', "Back", "to the list"),                                               \
    H(V, TUI_KEY_ESC, "Back", "to the list"),                                       \
    H(V, TUI_KC_QUIT, "Quit", NULL)

static const tui_keyentry g_entries[] = {
    /* The message list of an IMAP account. */
    LIST_COMMON(EKV_LIST_IMAP),
    M(EKV_LIST_IMAP, "Toggle Done", "the $Done flag", EA_DONE),

    /* The message list of a Gmail account. */
    LIST_COMMON(EKV_LIST_GMAIL),
    R(EKV_LIST_GMAIL, 't', TUI_VERB_MOVE, "Labels", "pick the labels of the message",
      EA_LABELS),
    M(EKV_LIST_GMAIL, "Archive", "remove every label", EA_ARCHIVE),
    M(EKV_LIST_GMAIL, "Restore from Trash", "back to the inbox", EA_RESTORE),
    M(EKV_LIST_GMAIL, "Remove this label", "from the message", EA_RM_LABEL),
    M(EKV_LIST_GMAIL, "Add a label...", "to the messages, asked for", EA_ADD_LABEL),
    M(EKV_LIST_GMAIL, "Remove a label...", "from the messages, asked for",
      EA_REMOVE_LABEL),

    /* The message reader. */
    READER_COMMON(EKV_READ_IMAP),
    M(EKV_READ_IMAP, "Toggle Done", "the $Done flag", EA_DONE),
    READER_COMMON(EKV_READ_GMAIL),
    R(EKV_READ_GMAIL, 't', TUI_VERB_MOVE, "Labels", "pick the labels of the message",
      EA_LABELS),
    M(EKV_READ_GMAIL, "Archive", "remove every label", EA_ARCHIVE),
    M(EKV_READ_GMAIL, "Remove this label", "from the message", EA_RM_LABEL),

    /* The Gmail label browser. */
    H(EKV_LABELS, 'j', "Down", "move down (also Down)"),
    H(EKV_LABELS, 'k', "Up", "move up (also Up)"),
    H(EKV_LABELS, TUI_KEY_PGDN, "Page down", NULL),
    H(EKV_LABELS, TUI_KEY_PGUP, "Page up", NULL),
    H(EKV_LABELS, TUI_KEY_HOME, "First label", NULL),
    H(EKV_LABELS, TUI_KEY_END, "Last label", NULL),
    H(EKV_LABELS, TUI_KEY_ENTER, "Open", "the messages of the label"),
    H(EKV_LABELS, '/', "Search", "cross-folder content search"),
    H(EKV_LABELS, '?', "Help", NULL),
    H(EKV_LABELS, ':', "Actions", "the menu of everything that changes state"),
    H(EKV_LABELS, 'q', "Back", "to the accounts"),
    H(EKV_LABELS, TUI_KEY_ESC, "Back", "to the accounts"),
    M(EKV_LABELS, "Create a label...", "on the server, name asked for", EA_LABEL_CREATE),
    S(EKV_LABELS, TUI_KC_NONE, TUI_VERB_NONE, "Delete the label", "on the server",
      "Delete the label %s on the server? Its messages keep their other labels.",
      EA_LABEL_DELETE),

    /* The IMAP folder browser (EMAIL-129): creating and deleting a folder are
     * rare, so they are in the menu only; deleting names the folder and asks.
     * 't' (tree / flat) is a registry key (move): the table refuses it as a
     * harmless key, so the browser handles it outside the table. */
    H(EKV_FOLDERS, 'j', "Down", "move down (also Down)"),
    H(EKV_FOLDERS, 'k', "Up", "move up (also Up)"),
    H(EKV_FOLDERS, TUI_KEY_PGDN, "Page down", NULL),
    H(EKV_FOLDERS, TUI_KEY_PGUP, "Page up", NULL),
    H(EKV_FOLDERS, TUI_KEY_HOME, "First folder", NULL),
    H(EKV_FOLDERS, TUI_KEY_END, "Last folder", NULL),
    H(EKV_FOLDERS, TUI_KEY_ENTER, "Open", "the folder, or step into its subfolders; t: tree / flat view"),
    H(EKV_FOLDERS, '/', "Search", "cross-folder content search"),
    H(EKV_FOLDERS, 'c', "Compose", "a new message"),
    H(EKV_FOLDERS, '?', "Help", NULL),
    H(EKV_FOLDERS, ':', "Actions", "the menu of everything that changes state"),
    H(EKV_FOLDERS, 'q', "Back", "up one level; the accounts at the top"),
    H(EKV_FOLDERS, TUI_KEY_ESC, "Back", "up one level; the accounts at the top"),
    M(EKV_FOLDERS, "Create a folder...", "on the server, name asked for", EA_FOLDER_CREATE),
    S(EKV_FOLDERS, TUI_KC_NONE, TUI_VERB_NONE, "Delete the folder", "on the server",
      "Delete the folder %s on the server? Its messages are lost there.",
      EA_FOLDER_DELETE),

    /* The accounts screen, a Gmail account under the cursor (EMAIL-130).  The
     * other keys of that screen (n, d, i, e) are the screen's own. */
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
    M(EKV_ACCOUNTS_GMAIL, "Renew the authorization...", "Gmail access, in the browser or by paste",
      EA_REAUTH),

    /* The attachment picker of the reader (EMAIL-147): no key changes state. */
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
    M(EKV_REVIEW, "Remove an attachment", NULL, EA_UNATTACH),
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
static void run_actionmenu(tui_actionmenu *m, const char *const targets[EA_COUNT]) {
    tui_decoder dec;
    tui_decoder_init(&dec);
    KeyEvents winch;
    key_events_begin(&winch);
    while (m->state == TUI_ACTION_OPEN) {
        int rows = terminal_rows(), cols = terminal_cols();
        if (rows <= 0) rows = 24;
        if (cols <= 0) cols = 80;
        tui_frame_overlay(draw_menu, m, rows, cols);
        tui_event ev;
        int r = key_events_next(STDIN_FILENO, &dec, &ev, -1);
        if (r <= 0 || ev.type == TUI_EV_HANGUP) {
            m->state = TUI_ACTION_CANCELLED;
            break;
        }
        if (ev.type == TUI_EV_RESIZE) continue;
        if (targets && !m->confirming && m->idx && m->dialog.list.selected >= 0) {
            int id = g_entries[m->idx[m->dialog.list.selected]].id;
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
    if (tui_actionmenu_open(&m, &g_table, view, "") != 0) return EA_NONE;
    run_actionmenu(&m, targets);
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
    run_actionmenu(&m, NULL);
    int yes = m.state == TUI_ACTION_CHOSEN;
    tui_actionmenu_free(&m);
    return yes;
}

void ekeys_help(const char *view, const char *title) {
    if (!g_ready && ekeys_init(NULL, 0) != 0) return;
    tui_keyhelp h;
    if (tui_keyhelp_open(&h, &g_table, view, title) != 0) return;
    tui_frame_run_dialog(&h.dialog);
    tui_keyhelp_free(&h);
}

int ekeys_help_has(const char *view, tui_keycode key) {
    for (int i = 0; i < N_ENTRIES; i++)
        if (g_entries[i].key == key && g_entries[i].view &&
            strcmp(g_entries[i].view, view) == 0)
            return 1;
    return 0;
}
