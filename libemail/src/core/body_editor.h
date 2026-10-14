/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* The full-screen message body editor: libtuikit's tui_textedit with autosave. */
#ifndef CORE_BODY_EDITOR_H
#define CORE_BODY_EDITOR_H

/** @brief Called with the whole text whenever the editor saves. */
typedef void (*BodyEditorSave)(const char *text, void *ctx);

/** @brief Seconds of idle typing after which a changed text is autosaved. */
#define BODY_EDITOR_AUTOSAVE_MS 3000

/**
 * @brief Edits @p *body (heap, may be NULL) on the whole terminal.
 *
 * There is no save key to forget: Esc leaves and saves; Ctrl-S saves and
 * stays; a changed text is also saved after BODY_EDITOR_AUTOSAVE_MS of
 * silence.  @p save is called for each save.  *body is replaced by the new
 * text (heap).
 * @param header  The text of the title bar.
 * @return 0 when left normally, -1 when the terminal could not be taken.
 */
int body_editor_run(char **body, const char *header, BodyEditorSave save, void *ctx);

#endif /* CORE_BODY_EDITOR_H */
