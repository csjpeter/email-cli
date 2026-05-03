# US-68 — Mail Rules: Negation Conditions ("doesn't contain", "isn't")

**As a** user who wants exclusion-based sorting rules,
**I want to** define rules that match when a field does NOT contain a pattern,
**so that** I can write rules like "if sender is not from my company, label as external".

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `if-not-from`, `if-not-subject`, `if-not-to` are new valid fields in `rules.ini`. |
| 2 | A message matches the condition when the field does NOT match the glob pattern. |
| 3 | `mail_rules_apply()` evaluates negated conditions correctly (negated glob). |
| 4 | Negated and positive conditions on the same field are allowed and ANDed. |
| 5 | `email-import-rules` converts `from,doesn't contain,X` → `if-not-from = *X*`. |
| 6 | `email-import-rules` converts `subject,isn't,X` → `if-not-subject = X`. |
| 7 | No `[warn]` is emitted for `doesn't contain` or `isn't` once this story is implemented. |
| 8 | `email-cli rules add` accepts `--if-not-from`, `--if-not-subject`, `--if-not-to` options. |
| 9 | `email-cli rules list` displays `if-not-*` conditions. |

---

## rules.ini syntax

```ini
[rule "External sender"]
if-not-from    = *@mycompany.com
then-add-label = External

[rule "Not newsletter"]
if-not-subject = *newsletter*
if-from        = *@example.com*
then-add-label = Regular
```

---

## Conversion table

| Thunderbird | → | rules.ini |
|---|---|---|
| `from,doesn't contain,@company.com` | | `if-not-from = *@company.com*` |
| `subject,isn't,Newsletter` | | `if-not-subject = Newsletter` |
| `to,doesn't contain,me@example.com` | | `if-not-to = *me@example.com*` |

---

## Implementation notes

**`mail_rules.h`**: add `if_not_from`, `if_not_subject`, `if_not_to` to `MailRule`.

**`mail_rules.c`**: in `mail_rules_apply()` / `mail_rule_matches()`, for each
`if_not_*` field: return 0 (no match) if `glob_match(pattern, value) == 1`.

**`mail_rules_load()` / `mail_rules_save()`**: parse and emit `if-not-from` etc.

---

## Related

* US-64: warn on unsupported elements
* US-65: match type conversion
* US-61: rules add / remove CLI
