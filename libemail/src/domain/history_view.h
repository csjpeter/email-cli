/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-22 */
/* The history screen of the TUI (libtuikit's tui_histview). */
#ifndef HISTORY_VIEW_H
#define HISTORY_VIEW_H

#include "config.h"

/**
 * @file history_view.h
 * @brief The recorded operations on screen, with Revert (policies/undo-model.md).
 *
 * Runs libtuikit's tui_histview full screen over the steps of
 * history_service.h: the history of one object, or the recent changes.  The
 * Revert entry (':' menu, or Enter) calls history_revert(); a refusal is
 * shown in its words at the bottom of the screen.  The screen returns on Esc,
 * q or Backspace; the caller redraws its own view.
 */

/**
 * @brief Shows the history screen and runs it until the user leaves it.
 * @param cfg     The account in use (its steps and the global ones are shown).
 * @param object  The object whose history is shown ("mail INBOX/00..2a"), or
 *                NULL for the recent changes.
 * @return 0 to carry on, -1 when ctrl-c asked the program to quit.
 */
int history_view_run(const Config *cfg, const char *object);

#endif /* HISTORY_VIEW_H */
