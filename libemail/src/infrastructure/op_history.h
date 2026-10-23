/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-22 */
/* Interface of the operation history (policies/undo-model.md). */
#ifndef OP_HISTORY_H
#define OP_HISTORY_H

#include <stddef.h>

/**
 * @file op_history.h
 * @brief The program's own record of every state-changing operation.
 *
 * A mail server keeps no history, so the history lives in the program's local
 * store: one append-only file, `<data dir>/email-cli/history.tsv`, one line per
 * step (who, when, account, object, operation, thing, old value, new value,
 * the step it reverts, whether it can be reverted).  A step is written right
 * after the change it records succeeded, by the same function that made the
 * change (`docs/spec/operation-history.md`).  History is never rewritten: a
 * revert is a new step that names the step it reverts.
 *
 * Secrets are never stored: a secret field is recorded as "changed".
 */

/** How an operation can be taken back (undo-model rule 7). */
typedef enum {
    OPH_REVERTABLE = 0,    /**< the inverse change exists and is performed by a revert */
    OPH_COMPENSABLE,       /**< a counter-step exists, but it is not the exact inverse */
    OPH_IRREVERSIBLE       /**< cannot be taken back: recorded, shown as "not revertable" */
} OpKind;

/** One recorded step.  All strings are heap copies owned by the OpStep. */
typedef struct {
    unsigned long id;        /**< the step number, 1, 2, ... */
    char *time;              /**< UTC, ISO 8601: 2026-10-22T20:47:16Z */
    char *who;               /**< the operating system user */
    char *account;           /**< the account the step belongs to; "" for global ones */
    char *op;                /**< operation name: flag, move, label, rule, account, ... */
    char *object;            /**< what changed: "mail INBOX/000000000000002a", "rule Foo" */
    char *thing;             /**< the field or part that changed */
    char *old_value;         /**< value before; "changed" for a secret */
    char *new_value;         /**< value after; "changed" for a secret */
    char *ref;               /**< operation specific data the revert needs (may be "") */
    unsigned long reverts;   /**< the step this one reverts; 0: none */
    OpKind kind;
} OpStep;

/** The steps read from the file, oldest first. */
typedef struct {
    OpStep *steps;
    int     count;
} OpHistory;

/** What op_history_record() takes.  NULL strings are "". */
typedef struct {
    const char *account;
    const char *op;
    const char *object;
    const char *thing;
    const char *old_value;
    const char *new_value;
    const char *ref;
    OpKind      kind;
    int         secret;   /**< 1: stored as "changed"; the step cannot be reverted by value */
} OpRecord;

/**
 * @brief Records one step.
 *
 * An operation that changes nothing (old equals new, and the step is not an
 * irreversible event such as a sent mail: those pass an empty old value and
 * are always recorded) records nothing.  When a revert is running
 * (op_history_revert_begin()) the step carries its `reverts` number.
 *
 * @return the new step's id; 0 when nothing was recorded (no change) ; -1 on error.
 */
long op_history_record(const OpRecord *rec);

/**
 * @brief Reads the history.
 * @param account  Only steps of this account (global steps, with an empty
 *                 account, are included); NULL: all.
 * @param object   Only steps whose object equals this; NULL: all.
 * @param out      Filled on success; release with op_history_free().
 * @return 0, or -1 on a read error (a missing file is an empty history).
 */
int op_history_load(const char *account, const char *object, OpHistory *out);

/** @brief Releases an OpHistory. */
void op_history_free(OpHistory *h);

/** @brief Releases the strings of one step (not the step itself). */
void op_history_step_free(OpStep *s);

/** @brief The step @p id in @p h, or NULL. */
const OpStep *op_history_step(const OpHistory *h, unsigned long id);

/** @brief The id of the step that reverts @p id and has not itself been
 *         reverted; 0: none. */
unsigned long op_history_reverted_by(const OpHistory *h, unsigned long id);

/** @brief "revertable", "compensable" or "not revertable". */
const char *op_history_kind_name(OpKind kind);

/**
 * @brief Whether the current user may revert @p step.
 *
 * A user reverts their own steps; an administrator (root) anyone's.  A step
 * that is irreversible, or was already reverted, is refused too.
 * @param h        The whole history (to find a revert of the step).
 * @param refusal  Receives the reason when refused.
 * @return 0 when allowed, -1 when refused.
 */
int op_history_may_revert(const OpHistory *h, const OpStep *step,
                          char *refusal, size_t size);

/** @brief The user steps are recorded as.  Default: the effective user's name. */
const char *op_history_actor(void);

/** @brief Overrides the actor and the administrator flag (tests, and nothing else). */
void op_history_set_actor(const char *who, int admin);

/** @brief 1 when the current user acts as an administrator. */
int op_history_is_admin(void);

/** @brief Steps recorded until op_history_revert_end() are reverts of step @p id. */
void op_history_revert_begin(unsigned long id);
/** @brief Ends the revert started by op_history_revert_begin(). */
void op_history_revert_end(void);

/** @brief Records a sent mail (irreversible); the subject is read from @p wire. */
void op_history_record_send(const char *account, const char *to, const char *wire);

/** @brief Escapes tab, newline and backslash (for the file); heap string. */
char *op_history_escape(const char *s);

#endif /* OP_HISTORY_H */
