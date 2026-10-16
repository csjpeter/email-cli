/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* Interface of the event wait that also reports a terminal resize. */
#ifndef CORE_KEY_EVENTS_H
#define CORE_KEY_EVENTS_H

#include <signal.h>
#include <tuikit/event.h>

/**
 * @file key_events.h
 * @brief One input event from a terminal descriptor, or a resize (EMAIL-97,
 *        EMAIL-109).
 *
 * The editors that own the input loop (body editor, single-line editor) wait
 * through key_events_next(): it returns a TUI_EV_RESIZE event when SIGWINCH
 * arrived, so the editor redraws at the new size at once.  The SIGWINCH
 * handler is installed for the time of the editor with key_events_begin() and
 * the program's own handler is put back by key_events_end().
 */

/** Saved state of the SIGWINCH handler that key_events_begin() replaced. */
typedef struct { struct sigaction old; } KeyEvents;

/** Install the resize handler; remember the previous one in @p ke. */
void key_events_begin(KeyEvents *ke);

/** Put the previous SIGWINCH handler back. */
void key_events_end(KeyEvents *ke);

/**
 * @brief Next event from @p fd through the persistent decoder @p d, so that a
 *        bracketed paste arrives whole.
 * @param idle_ms  < 0 blocks; otherwise 0 is returned when that long passes
 *                 with no input.
 * @return 1 event (TUI_EV_RESIZE when the terminal was resized), 0 idle,
 *         -1 end of input or error.
 */
int key_events_next(int fd, tui_decoder *d, tui_event *ev, int idle_ms);

#endif /* CORE_KEY_EVENTS_H */
