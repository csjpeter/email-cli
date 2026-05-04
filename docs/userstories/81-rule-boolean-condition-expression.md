# US-81 — Rule Condition: Boolean Expression Language

**As a** user configuring email rules,
**I want** to write the condition for a rule as a single boolean expression
using `and`, `or`, `!`, and parentheses,
**so that** I can express any combination of field tests without being restricted
to the implicit AND of separate flat fields.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | A rule's condition is stored as a single `when` string (replaces the old flat `if-*` fields). |
| 2 | The expression language supports the following atoms: |
|   | `from:pattern` — From header matches glob |
|   | `to:pattern` — To header matches glob |
|   | `subject:pattern` — Subject matches glob |
|   | `label:name` — Message currently carries label `name` |
|   | `body:pattern` — Plain-text body matches glob |
|   | `age-gt:N` — Message is older than N days |
|   | `age-lt:N` — Message is younger than N days |
| 3 | Operators: `and` (conjunction), `or` (disjunction), `!` (prefix negation), `(` `)` (grouping). |
| 4 | Precedence: `!` binds tightest, then `and`, then `or`; parentheses override. |
| 5 | An empty `when` (or absent) is treated as always-true (matches every message). |
| 6 | A syntactically invalid `when` causes the rule to be skipped and a `[warn]` is emitted. |
| 7 | The evaluator produces the same result as the current flat-field AND chain for any expression that only uses `from:`, `to:`, `subject:`, `label:`, `body:`, `age-gt:`, `age-lt:` ANDed together. |

---

## Expression examples

```
from:*@spam.com

from:*@gmail.com or to:*@gmail.com

(from:*alice* or from:*bob*) and !label:UNREAD

from:*boss@corp.com and subject:*urgent* and age-lt:2
```

---

## File format

```ini
[rule "no-unread-from-alice" action 1]
when = from:*alice@example.com* and !label:UNREAD
then-add-label = IMPORTANT
```

Old flat-field format (for backwards compatibility, see US-84):
```ini
[rule "OldStyle"]
if-from = *alice@example.com*
then-add-label = IMPORTANT
```

---

## Implementation notes

- `mail_rule_condition_parse(expr, &ast)` → allocates an AST; returns 0 on success, -1 on error
- `mail_rule_condition_eval(ast, msg_ctx)` → returns 1 if condition matches, 0 otherwise
- `mail_rule_condition_free(ast)` → frees the AST
- The AST is stored in `MailRule.condition_ast`; the original string is `MailRule.when`
- The `msg_ctx` provides: from, to, subject, labels_csv, body, message_date, current_labels

---

## Related

* US-82: per-action condition groups (the new multi-action rule format)
* US-83: TUI edit form for boolean conditions
* US-84: backwards compatibility with old flat-field format
