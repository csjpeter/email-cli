# TASK-009 — PTY tests for US-62 (TUI rules editor, R key)

**Type:** Test  
**Related US:** US-62  

## Context

US-62 specifies that pressing `R` in the TUI opens an inline rules editor.
No PTY functional test exists for this feature.

## Work items

Add to `tests/pty/test_pty_views.c` (or a new file `test_pty_rules.c`):

1. **R key opens rules panel**  
   Launch `email-tui` with a seeded account; press `R`; verify a "Rules" or
   "rules" heading appears in the terminal output.

2. **Rules panel displays existing rule**  
   Pre-populate `rules.ini` with one rule; press `R`; verify the rule name
   is displayed.

3. **ESC closes rules panel**  
   Press `R` then `ESC`; verify the panel is dismissed (message list
   reappears).

4. **'a' key in rules panel triggers add-rule prompt** (if implemented)

## Prerequisite

US-62 (TUI rules editor) must be implemented before these tests can be written.
If the R-key handler is not yet present, this task's first sub-item is to
implement the feature stub and then layer the tests on top.

## Definition of done

PTY tests pass in CI via `./manage.sh test`.
