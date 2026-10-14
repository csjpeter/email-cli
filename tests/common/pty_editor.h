/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* PTY test helper: drive the compose body editor (tui_textedit, EMAIL-87). */
#ifndef TESTS_COMMON_PTY_EDITOR_H
#define TESTS_COMMON_PTY_EDITOR_H

#include "ptytest.h"

#include <stdio.h>
#include <unistd.h>

/** Marker of the editor's bottom line, printed by body_editor.c. */
#define PTY_EDITOR_MARK "Esc=save and continue"

/**
 * Waits for the body editor, types @p text (may be NULL) and leaves it with
 * Esc, which saves.  Returns 0, or -1 when the editor did not appear.
 */
static inline int pty_editor_type_and_leave(PtySession *s, const char *text) {
    if (pty_wait_for(s, PTY_EDITOR_MARK, 6000) != 0) return -1;
    pty_settle(s, 150);
    if (text && text[0]) {
        pty_send_str(s, text);
        pty_settle(s, 150);
    }
    pty_send_key(s, PTY_KEY_ESC);
    return 0;
}

/** Removes the autosaved compose draft of the test home @p home, so that a
 *  test is not offered the draft a killed earlier session left. */
static inline void pty_editor_forget_draft(const char *home) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.local/share/email-cli/compose-draft", home);
    unlink(path);
}

#endif /* TESTS_COMMON_PTY_EDITOR_H */
