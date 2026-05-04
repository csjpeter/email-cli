# US-82 — Rules: Per-Action Condition Groups

**As a** user who wants different actions to fire under different conditions,
**I want** a single rule to contain multiple independent action groups,
each with its own boolean `when` expression,
**so that** I do not have to duplicate shared logic across multiple rules
and I can express e.g. "always label, but only move if not already labelled."

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | A rule can contain one or more **action groups**, each identified by `action N` (N = 1, 2, …). |
| 2 | Each action group has its own `when` expression (US-81) and a set of `then-*` directives. |
| 3 | Action groups within the same rule are evaluated independently; each fires or does not fire based on its own `when`. |
| 4 | The name of the rule (the string in `[rule "..."]`) is shared across all its action groups. |
| 5 | In the ini file, action groups are written as separate sections: `[rule "name" action N]`. |
| 6 | Action group numbering starts at 1 and must be contiguous; a gap (1, 3) is treated as an error and a `[warn]` is emitted. |
| 7 | If a rule has only one action group, the `action 1` suffix may be omitted (shorthand for the common case). |

---

## File format

```ini
; Action group 1 — always label the message
[rule "misina" action 1]
when = from:*snecica70@gmail.com* or to:*snecica70@gmail.com*
then-add-label = UNREAD

; Action group 2 — only move+forward if it wasn't already in UNREAD
[rule "misina" action 2]
when = (from:*snecica70@gmail.com* or to:*snecica70@gmail.com*) and !label:UNREAD
then-move-folder = gyerekek
then-forward-to = Brigitta Mucsi <brigitta.mucsi@gmail.com>
```

Shorthand (single action group, `action 1` suffix omitted):
```ini
[rule "simple"]
when = from:*@spam.com
then-add-label = _junk
```

---

## MailRule struct change

```c
typedef struct {
    char  *name;
    int    action_index;    /* 1-based; 1 = first (and usually only) group */
    char  *when;            /* boolean expression string */
    /* void *condition_ast; */ /* parsed AST — populated by mail_rules_load */
    char  *then_add_label[MAIL_RULE_MAX_LABELS];
    int    then_add_count;
    char  *then_rm_label[MAIL_RULE_MAX_LABELS];
    int    then_rm_count;
    char  *then_move_folder;
    char  *then_forward_to;
} MailRule;
```

Rules with the same `name` but different `action_index` values are distinct
`MailRule` entries in the `MailRules` array. The engine applies all of them.

---

## Related

* US-81: boolean condition expression language (the `when` syntax)
* US-83: TUI form for editing multi-action rules
* US-84: backwards compatibility
