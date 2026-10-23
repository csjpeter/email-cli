/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-22 */
/* Recording and reverting the mail operations (policies/undo-model.md). */
#ifndef HISTORY_SERVICE_H
#define HISTORY_SERVICE_H

#include <stddef.h>
#include "config.h"
#include "op_history.h"

/**
 * @file history_service.h
 * @brief The mail side of the operation history.
 *
 * The services that change a mail (flag, move, trash, archive, label, junk,
 * delete, folder, send) call a `history_note_*` function right after the
 * change succeeded; history_revert() performs the inverse of a recorded step
 * and records the revert as a step of its own.  The configuration, rule and
 * setting steps are recorded and reverted by the stores themselves
 * (config_store.c, mail_rules.c); history_revert() dispatches to them.
 *
 * Object names: `mail FOLDER/UID` (IMAP) or `mail UID` (Gmail); `folder NAME`;
 * `label NAME`; `rule NAME`; `account`; `settings`; `sent mail`.
 */

/**
 * The calls that reach the server when a step is reverted.  The defaults use
 * libemail-client; a test installs its own with history_set_backend().
 */
typedef struct {
    /** Moves the mail with @p msgid from folder @p from back to @p to
     *  (IMAP).  On a refusal writes the reason to @p refusal. */
    int (*move_back)(const Config *cfg, const char *msgid, const char *from,
                     const char *to, char *refusal, size_t size);
    /** Adds (@p add 1) or removes a Gmail label of the mail. */
    int (*label)(const Config *cfg, const char *uid, const char *label, int add);
    /** Creates (@p create 1) or deletes an IMAP folder, or a Gmail label
     *  (@p id is its id). */
    int (*folder)(const Config *cfg, const char *name, const char *id, int create);
} HistoryBackend;

/** @brief Installs @p b as the server backend; NULL restores the default. */
void history_set_backend(const HistoryBackend *b);

/** @brief The Message-ID of a locally stored mail; heap string, NULL if the
 *         local store has not the mail. */
char *history_message_id(const char *folder, const char *uid);

/** @brief Writes the object name of a mail into @p buf. */
void history_mail_object(const Config *cfg, const char *folder, const char *uid,
                         char *buf, size_t size);

/** @brief 1/0: whether flag @p bit is set on the locally stored mail; -1: the
 *         local store has not the mail. */
int history_flag_state(const Config *cfg, const char *folder, const char *uid,
                       int bit);

/** @brief Records a flag change (@p old_state, @p new_state are 0/1/-1:
 *         unknown counts as "the opposite").  No step when nothing changed. */
void history_note_flag(const Config *cfg, const char *folder, const char *uid,
                       int bit, int old_state, int new_state);

/** @brief Records the move of an IMAP mail; @p op is "move", "trash" or "archive". */
void history_note_move(const Config *cfg, const char *op, const char *folder,
                       const char *uid, const char *target, const char *msgid);

/** @brief Records the move of a Gmail mail from label @p src to @p target. */
void history_note_gmail_move(const Config *cfg, const char *uid, const char *src,
                             const char *target);

/** @brief Records the change of one Gmail label of a mail. */
void history_note_label(const Config *cfg, const char *uid, const char *label,
                        int add);

/** @brief Records that a Gmail mail went to the trash; @p labels is the CSV of
 *         its labels before. */
void history_note_gmail_trash(const Config *cfg, const char *uid,
                              const char *labels);

/** @brief Records the archiving of a Gmail mail: @p removed is the CSV of the
 *         labels taken off, @p was_unread whether it was unread. */
void history_note_gmail_archive(const Config *cfg, const char *uid,
                                const char *removed, int was_unread);

/** @brief Records the junk mark (@p junk 1) or the not-junk mark (0). */
void history_note_junk(const Config *cfg, const char *uid, int junk);

/** @brief Records a permanent delete (irreversible). */
void history_note_delete(const Config *cfg, const char *folder, const char *uid,
                         const char *msgid);

/** @brief Records the creation (@p create 1) or deletion of a folder, or of a
 *         Gmail label (@p id is its id then). */
void history_note_folder(const Config *cfg, const char *name, const char *id,
                         int create);

/**
 * @brief Reverts step @p id: checks who may, that the step was not overtaken,
 *        performs the inverse and records it as a step naming @p id.
 * @param refusal  Receives the reason when refused (the server's words where
 *                 the server refused).
 * @return 0 on success, -1 refused or failed.
 */
int history_revert(unsigned long id, char *refusal, size_t size);

/**
 * @brief Loads the steps of @p account (all when NULL), newest first; with
 *        @p object only that object's.  Keep the whole list when asking
 *        history_revertable(): a revert of a step is itself a step.
 */
int history_load(const char *account, const char *object, OpHistory *out);

/** @brief 1 when @p s belongs in the view: the history of one object shows
 *         every step; recent changes (@p recent 1) are everyone's for an
 *         administrator and the caller's own otherwise. */
int history_shown(const OpStep *s, int recent);

/** @brief One line describing what @p s did: "unread: no -> yes". */
void history_describe(const OpStep *s, char *buf, size_t size);

/** @brief 1 when @p s can be reverted now: revertable and not yet reverted. */
int history_revertable(const OpHistory *h, const OpStep *s);

#endif /* HISTORY_SERVICE_H */
