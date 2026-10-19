/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-19 */
/* The key table of email-tui: one table for the key handler, the help and the ':' menu. */
#ifndef CORE_KEY_TABLE_H
#define CORE_KEY_TABLE_H

#include "platform/terminal.h"

#include <stddef.h>

/**
 * @file key_table.h
 * @brief email-tui's keyboard policy (EMAIL-175).
 *
 * policies/tui-interface-model.md, "Actions that change state: the action
 * menu".  Every key of the message list, the message reader, the label browser
 * and the review screen is a row of one libtuikit key table.  The key
 * handlers ask the table what a key means (ekeys_find()), the help overlay is
 * made from the same table (ekeys_help()), and ':' opens the action menu of
 * the view (ekeys_menu()): every state-changing action there, with the
 * confirmation of a serious one naming its target and No as the default.
 */

/** The program's numbers for the actions of the table (0: a harmless key). */
typedef enum {
    EA_NONE = 0,
    EA_STAR,          /**< '*'  toggle the star (flag)                      */
    EA_READ,          /**< '+'  mark read                                   */
    EA_UNREAD,        /**< '-'  mark unread                                 */
    EA_JUNK,          /**< '!'  toggle junk                                 */
    EA_DELETE,        /**< 'd', Delete: move to Trash / delete for good     */
    EA_LABELS,        /**< 't'  Gmail label picker                          */
    EA_SYNC,          /**< menu: start a background sync                    */
    EA_ARCHIVE,       /**< menu: Gmail archive                              */
    EA_RESTORE,       /**< menu: Gmail restore from Trash                   */
    EA_DONE,          /**< menu: IMAP toggle $Done                          */
    EA_RM_LABEL,      /**< menu: Gmail remove the current label             */
    EA_ADD_LABEL,     /**< menu: Gmail add a named label                    */
    EA_REMOVE_LABEL,  /**< menu: Gmail remove a named label                */
    EA_MOVE,          /**< menu: move to a folder / label                   */
    EA_SAVE_ALL,      /**< menu: save all attachments                       */
    EA_LABEL_CREATE,  /**< menu: create a label                             */
    EA_LABEL_DELETE,  /**< menu: delete a label                             */
    EA_SEND,          /**< menu: send the message                           */
    EA_UNATTACH,      /**< menu: remove the last attachment of the draft    */
    EA_EDIT_HEADERS,  /**< 'e'  edit the headers of the draft               */
    EA_FOLDER_CREATE, /**< menu: create an IMAP folder (EMAIL-129)          */
    EA_FOLDER_DELETE, /**< menu: delete an IMAP folder (EMAIL-129)          */
    EA_COUNT
} EmailAction;

/** The views of the table. */
#define EKV_LIST_IMAP   "list-imap"
#define EKV_LIST_GMAIL  "list-gmail"
#define EKV_READ_IMAP   "reader-imap"
#define EKV_READ_GMAIL  "reader-gmail"
#define EKV_LABELS      "labels"
#define EKV_FOLDERS     "folders"
#define EKV_REVIEW      "review"

/**
 * @brief Validates the table (once).  A program calls it at start and exits
 *        on -1: a table that breaks the policy is a defect, not a setting.
 * @param why  Receives the reason on failure; may be NULL.
 */
int ekeys_init(char *why, size_t size);

/**
 * @brief The action the key means in @p view, or EA_NONE when it is a
 *        harmless key or not in the table.  @p ch is
 *        terminal_last_printable() of a TERM_KEY_IGNORE key.
 */
EmailAction ekeys_find(const char *view, TermKey key, int ch);

/**
 * @brief Opens the ':' menu of @p view over the screen and runs it.
 * @param targets  Per action number, the text a confirmation names (the
 *                 message, the account, the label), or NULL for none.
 * @return The chosen action (confirmed when it needed it), or EA_NONE.
 */
EmailAction ekeys_menu(const char *view, const char *const targets[EA_COUNT]);

/**
 * @brief Asks the confirmation of action @p a in @p view naming @p target
 *        (default answer No).  An action that needs none is chosen at once.
 * @return 1 when the action is to be performed, 0 when it was refused.
 */
int ekeys_confirm(const char *view, EmailAction a, const char *target);

/** @brief The help overlay of @p view, made from the table; returns when closed. */
void ekeys_help(const char *view, const char *title);

#endif /* CORE_KEY_TABLE_H */
