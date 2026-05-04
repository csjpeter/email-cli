# US-79 — Rules Editor: Rule Detail View

**As a** user in the email-tui rules editor,
**I want** to open a dedicated detail screen for any rule I select,
**so that** I can read all its fields clearly without noise from other rules,
and take focused actions (edit, delete) on that one rule.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Pressing Enter in the rules list opens a detail screen for the selected rule. |
| 2 | The detail screen shows the rule name and account at the top. |
| 3 | All non-empty condition fields are displayed with their labels. |
| 4 | Conditions without a value show `(any)` for the four primary fields (if-from, if-subject, if-to, if-label). |
| 5 | All non-empty action fields are displayed with their labels. |
| 6 | The footer shows `e=edit  d=delete  ESC/q=back`. |
| 7 | Pressing `e` opens the edit form (US-80) pre-filled with the rule's current values. |
| 8 | Pressing `d` shows `Delete "<name>"? (y/N)` at the footer; `y` deletes the rule and returns to the list; any other key cancels and also returns to the list. |
| 9 | Pressing ESC or `q` returns to the rules list. |
| 10 | After an edit the detail view reloads and shows the updated field values. |

---

## Notes

The detail view is a read-only display that also acts as the gateway to editing
and deleting. It makes it easy to verify a rule before modifying it.

---

## Related

* US-78: rules list navigation (the calling screen)
* US-80: rule edit form with inline editing
* US-62: rules editor (original implementation)
