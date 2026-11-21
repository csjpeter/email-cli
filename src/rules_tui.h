/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-11-21 */
/* Interface of the rules screens of email-tui. */
#ifndef RULES_TUI_H
#define RULES_TUI_H

#include "config.h"

/**
 * @file rules_tui.h
 * @brief The rules overview, the rule form, the dry run and the undo of
 *        email-tui (EMAIL-312), built of libtuikit widgets.
 */

/**
 * @brief The rules overview of the account @p cfg names (key l of the message
 *        list): every rule in order with what it does, on or off, the cached
 *        messages it matches and its last change; reorder, switch, copy,
 *        delete, add and edit (the rule form), preview, apply and undo.
 *
 * Returns when the user goes back.  The terminal must not be in raw mode (the
 * screen enters it).
 */
void rules_tui_editor(const Config *cfg);

#endif /* RULES_TUI_H */
