# US-62 — TUI Rules Editor

**As a** user browsing the interactive message list,
**I want to** open a rules editor with a single key press,
**so that** I can view and manage sorting rules without leaving the TUI.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Pressing **R** in the interactive message list opens the rules editor. |
| 2 | The rules editor shows a numbered list of all rules for the current account. |
| 3 | Pressing **a** in the rules editor opens a form to add a new rule. |
| 4 | The add-rule form accepts: name, if-from, if-subject, if-to, if-label, add-label, remove-label, move-folder. |
| 5 | Pressing **d** on a selected rule removes it after confirmation. |
| 6 | Pressing **ESC** or **q** exits the rules editor and returns to the message list. |
| 7 | After adding or removing a rule, the list refreshes to show the updated rules. |
| 8 | The status bar shows available keys: `a=add  d=delete  ESC=back`. |
| 9 | The **R** key replaces the previous **U** (refresh) binding; refresh is moved to **U**. |

---

## Key bindings in message list

| Key | Action |
|-----|--------|
| **R** | Open rules editor |
| **U** | Refresh (re-sync) |

---

## Related

* US-57: rules list
* US-61: rules add / remove
* US-59: rules apply
