# US-80 — Rules Editor: Edit Form with Inline Cursor Editing

**As a** user adding or editing a rule in email-tui,
**I want** each form field to support proper cursor movement and UTF-8 input
with existing values pre-filled,
**so that** I can edit fields without arrow keys writing `^[[D` sequences,
accented characters are handled correctly, and previously saved values appear
in the fields ready to be modified.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The edit form is opened in raw terminal mode; each field uses `input_line_run` for cursor-aware, UTF-8-correct line editing. |
| 2 | Left/right arrow keys move the cursor within the field; Home/End jump to start/end; Backspace deletes the character before the cursor. |
| 3 | When editing an existing rule, all fields are pre-filled with the rule's saved values. |
| 4 | The pre-fill does not exhibit garbage values (dangling pointer bug fixed: values are copied into local buffers before any source MailRules is freed). |
| 5 | The form includes all rule fields: Name, if-from, if-subject, if-to, if-label, if-not-from, if-not-subject, if-not-to, if-body, if-age-gt, if-age-lt, add-label[1–3], rm-label[1–3], then-move-folder, then-forward-to (19 fields total). |
| 6 | Pressing Enter advances to the next field; pressing ESC on any field cancels the entire form without saving. |
| 7 | After the last field a `Save? (y/N)` prompt is shown; `y` saves and returns to the caller; any other key cancels. |
| 8 | If the Name field is left empty, a "Rule name is required" message is shown; the form exits without saving. |
| 9 | A saved rule is immediately visible in the rules list and detail view without restarting email-tui. |

---

## Notes

The old form used `fgets` in cooked terminal mode, which caused:
* Arrow keys to emit raw escape sequences like `^[[D` into the field value.
* Multi-byte UTF-8 characters (accented letters) to be mishandled.
* A dangling pointer: `MailRule copy = rules->rules[idx]; mail_rules_free(rules)` left
  all `copy.*` pointers pointing to freed memory, so prefill values appeared as garbage.

The new implementation:
* Uses `input_line_run()` (the same widget used in compose and folder-select forms).
* Copies all prefill strings into local `char[]` buffers before the source `MailRules`
  is freed, eliminating the dangling pointer.
* Supports all 11 condition fields and 8 action fields (with 3 visible add/rm-label slots).

---

## Related

* US-79: rule detail view (the caller of the edit form)
* US-78: rules list navigation
* US-62: rules editor (original implementation, superseded for these aspects)
