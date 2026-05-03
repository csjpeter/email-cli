# TASK-009 — PTY tests for US-62 (TUI rules editor, l key)

**Type:** Test
**Related US:** US-62
**Status:** DONE

## Context

US-62 specifies that pressing `l` in the TUI opens an inline rules editor.
PTY functional tests were added to `tests/pty/test_pty_views.c`.

## Work items (completed)

1. **l key opens rules panel** — `test_tui_rules_editor_opens`
2. **Empty rules list shows hint** — `test_tui_rules_editor_empty_message`
3. **Pre-populated rules appear** — `test_tui_rules_editor_lists_rules`
4. **'a' key adds a rule** — `test_tui_rules_editor_add_rule`
5. **Empty name cancels** — `test_tui_rules_editor_cancel_add`
6. **'d' key deletes a rule** — `test_tui_rules_editor_delete_rule`
7. **'q' also closes the editor** — `test_tui_rules_editor_q_closes`

## Note

The key was changed from `R` to `l` to resolve a conflict with the `U` (refresh)
binding used by US-19 and several Gmail TUI tests.
