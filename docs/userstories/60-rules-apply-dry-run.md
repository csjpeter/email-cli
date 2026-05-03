# US-60 — Mail Rules: Apply --dry-run Preview

**As a** user who wants to test new or modified sorting rules,
**I want to** preview which messages would be affected without making any changes,
**so that** I can verify the rule logic before committing to it.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `email-cli rules apply --dry-run` evaluates all rules against cached messages but writes nothing. |
| 2 | For each matching message a `[dry-run]` line is printed (same format as `[rule]` in US-59). |
| 3 | The summary reads: `Rules dry-run: N message(s) would be modified.` |
| 4 | Running `rules apply` after `rules apply --dry-run` on the same data applies all the previewed changes. |
| 5 | `--dry-run` can be combined with `--verbose` (both flags active). |
| 6 | No files in the local store are modified when `--dry-run` is active. |

---

## Example output

```
$ email-cli rules apply --dry-run
=== Dry-run rules: user@example.com ===
  [dry-run] "GitHub notifications" → uid:1001  +GitHub  -INBOX
  [dry-run] "GitHub notifications" → uid:1003  +GitHub  -INBOX
  Rules dry-run: 2 message(s) would be modified.
```

---

## Related

* US-59: rules apply (actual execution)
* US-57: rules list
