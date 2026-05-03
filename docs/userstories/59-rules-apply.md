# US-59 — Mail Rules: Apply Rules Retroactively

**As a** user who has defined or updated sorting rules,
**I want to** apply those rules to all locally cached messages,
**so that** previously received messages are also organized without re-downloading them.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `email-cli rules apply` processes all locally cached messages for the account. |
| 2 | For each matching message, the rule's label add/remove actions are applied to the local store. |
| 3 | The command prints a summary: `Rules applied: N message(s) modified.` |
| 4 | Running the command a second time on already-processed messages applies no changes (idempotent). |
| 5 | `--verbose` / `-v` flag prints a `[rule]` line per matched message (same format as US-58). |
| 6 | With multiple accounts, `--account <email>` limits processing to one account. |
| 7 | `email-cli rules apply --help` shows a usage page. |
| 8 | The command does not contact the IMAP server; operates only on the local cache. |

---

## Example output

```
$ email-cli rules apply --verbose
=== Applying rules: user@example.com ===
  [rule] "GitHub notifications" → uid:1001  +GitHub  -INBOX
  [rule] "GitHub notifications" → uid:1003  +GitHub  -INBOX
  Rules applied: 2 message(s) modified.
```

---

## Related

* US-57: rules list
* US-60: rules apply --dry-run
* US-58: sync --verbose
