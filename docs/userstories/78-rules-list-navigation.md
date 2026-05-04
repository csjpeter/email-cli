# US-78 — Rules Editor: Cursor-Based List Navigation

**As a** user in the email-tui rules editor,
**I want** to navigate the list of rules with the j/k keys or arrow keys
and open a rule by pressing Enter,
**so that** I can browse many rules without entering a number and see the list
without all field details cluttering the screen.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The rules list shows only rule names, one per line, with a highlighted cursor row. |
| 2 | `j` or ↓ moves the cursor one rule down; `k` or ↑ moves it one rule up. |
| 3 | The list scrolls when the number of rules exceeds the visible terminal height. |
| 4 | Pressing Enter on a rule opens the rule detail view (US-79). |
| 5 | The footer shows: `j/k=navigate  Enter=open  a=add  d=delete  ESC/q=back`. |
| 6 | `d` deletes the rule at the current cursor position (with y/N confirmation) without requiring a number. |
| 7 | After a delete the cursor stays at the same position or moves to the last rule if the deleted rule was last. |

---

## Notes

The previous list view showed full rule details for every rule inline, which
made the screen unreadable with even a handful of rules. The new design mirrors
the message-list view: names only in the list, details on demand.

---

## Related

* US-62: rules editor (original implementation)
* US-79: rule detail view
* US-80: rule edit form with inline editing
