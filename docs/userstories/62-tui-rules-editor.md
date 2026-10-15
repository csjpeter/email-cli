# US-62 — TUI Rules Editor

**As a** user browsing the interactive message list,
**I want to** open a rules editor with a single key press,
**so that** I can view and manage sorting rules without leaving the TUI.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Pressing **l** in the interactive message list opens the rules editor. |
| 2 | The rules editor shows a numbered list of all rules for the current account. |
| 3 | Pressing **a** in the rules editor opens a form to add a new rule. |
| 4 | The add-rule form accepts: name, if-from, if-subject, if-to, if-label, add-label, remove-label, move-folder. |
| 5 | Pressing **d** on a selected rule removes it after confirmation. |
| 6 | Pressing **ESC** or **q** exits the rules editor and returns to the message list. |
| 7 | After adding or removing a rule, the list refreshes to show the updated rules. |
| 8 | The status bar shows available keys: `a=add  d=delete  ESC=back`. |

---

## Key bindings in message list

| Key | Action |
|-----|--------|
| **l** | Open rules editor |
| **U** | Refresh (re-list from local cache) |

---

## Help and input checks (EMAIL-95)

* `h` (or `?`) in the rules list, in a rule and in the condition list opens
  the same boxed key overlay as the other screens; any key closes it.  The
  key bars name it (`h=help`).
* A condition typed in the condition list (new or edited) is parsed at once;
  one that does not parse is refused with
  `Not a valid condition: <text>  (e.g. from:*@example.org)` on the last row
  and is not added.

---

## Related

* US-57: rules list
* US-61: rules add / remove
* US-59: rules apply
