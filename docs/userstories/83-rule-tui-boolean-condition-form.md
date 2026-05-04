# US-83 — Rules TUI: Edit Form for Boolean Conditions and Action Groups

**As a** user editing rules in email-tui,
**I want** the edit form to show a single `when` expression field (instead of
11 separate flat condition fields) and allow me to manage multiple action groups
within one rule,
**so that** the form reflects the new boolean-expression model and stays usable.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The rule edit form shows a single `When:` field (input_line_run, full cursor editing) instead of the 11 flat `if-*` fields. |
| 2 | The form shows the list of action groups for the rule, with their `when` (if different from the rule-level when) and their actions. |
| 3 | Navigating to an action group and pressing `e` opens an action-group edit sub-form. |
| 4 | The action-group sub-form has: a `When (override):` field and the action fields (`add-label[1–3]`, `rm-label[1–3]`, `then-move-folder`, `then-forward-to`). |
| 5 | Pressing `+` in the rule edit form adds a new action group. |
| 6 | Pressing `d` on a highlighted action group removes it (with y/N confirmation). |
| 7 | Saving the form writes all action groups as `[rule "name" action N]` sections. |
| 8 | The detail view (US-79) shows each action group's `when` and actions separately. |
| 9 | Rules created before US-82 (single-group, flat-field format) continue to display and edit correctly (see US-84). |

---

## Form layout sketch

```
Edit rule for peter@csaszar.email
Rule name: misina

Action groups (2):
  [1] when: from:*snecica70* or to:*snecica70*
      then-add-label: UNREAD
  [2] when: (from:*snecica70* or to:*snecica70*) and !label:UNREAD
      then-move-folder: gyerekek
      then-forward-to: brigitta.mucsi@gmail.com

  j/k=select  e=edit group  +=add group  d=delete  s=save  ESC=cancel
```

Action group sub-form (opened with `e`):
```
Action group 1 of rule "misina"
When: from:*snecica70@gmail.com* or to:*snecica70@gmail.com*

add-label [1]: UNREAD
add-label [2]:
add-label [3]:
rm-label  [1]:
rm-label  [2]:
rm-label  [3]:
then-move-folder:
then-forward-to:

Enter=next field  ESC=cancel  y=save group
```

---

## Related

* US-81: boolean condition expression language
* US-82: per-action condition groups (data model)
* US-84: backwards compatibility
* US-79: rule detail view (must also be updated)
* US-80: current edit form (superseded by this story for condition fields)
