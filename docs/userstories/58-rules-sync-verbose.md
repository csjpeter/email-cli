# US-58 — Mail Rules: Verbose Sync Log

**As a** user who has configured mail sorting rules,
**I want to** see which rules fired for which messages during sync,
**so that** I can verify that my rules are working correctly.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `email-sync --verbose` prints a `[rule]` line for each message that matches at least one rule. |
| 2 | Each `[rule]` line shows the rule name and the message UID. |
| 3 | Each `[rule]` line shows the label additions (`+Label`) and removals (`-Label`) triggered. |
| 4 | Without `--verbose`, no `[rule]` lines are printed. |
| 5 | `email-sync --apply-rules --verbose` applies rules retroactively and prints `[rule]` lines. |
| 6 | `email-sync --verbose` short alias: `-v`. |

---

## Example output

```
$ email-sync --verbose
Syncing account: user@example.com ...
  [1/5] UID 1001...
  [rule] "GitHub notifications" → uid:1001  +GitHub  -INBOX
  [2/5] UID 1002...
  [3/5] UID 1003...
  [rule] "GitHub notifications" → uid:1003  +GitHub  -INBOX
Sync complete: 5 fetched, 2 rules fired.
```

---

## Related

* US-57: rules list
* US-59: rules apply
