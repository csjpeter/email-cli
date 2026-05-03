# US-57 — Mail Rules: List Rules

**As a** user who has configured mail sorting rules,
**I want to** see all sorting rules for my account with a single command,
**so that** I can verify what rules are active and review their conditions and actions.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `email-cli rules list` prints all rules from `~/.config/email-cli/accounts/<account>/rules.ini`. |
| 2 | Each rule shows its name in a `[rule "Name"]` header line. |
| 3 | All configured conditions (`if-from`, `if-subject`, `if-to`, `if-label`) are shown. |
| 4 | All configured actions (`then-add-label`, `then-remove-label`, `then-move-folder`) are shown. |
| 5 | When no rules file exists or the file is empty, a message is printed: `No rules configured for <account>.` |
| 6 | `email-cli rules list --help` / `email-cli help rules list` show a usage page. |
| 7 | With multiple accounts configured, `--account <email>` selects which account's rules to list. |

---

## Example output

```
=== Rules: user@example.com ===

[rule "GitHub notifications"]
  if-from           = *@github.com*
  then-add-label    = GitHub
  then-remove-label = INBOX

[rule "Newsletters"]
  if-subject        = *newsletter*
  then-add-label    = Newsletter

2 rule(s) configured.
```

---

## Related

* US-58: sync --verbose rules log
* US-59: rules apply
* US-61: rules add / remove
