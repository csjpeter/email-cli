# US-74 — Import Rules: Warn for Niche Thunderbird Elements

**As a** user importing Thunderbird filters,
**I want** to receive a clear warning for every Thunderbird filter element that the importer
cannot convert (beyond the already-covered body/age/negation/flag cases),
**so that** no rule element is silently dropped and I can complete the migration manually.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Condition field `cc` emits `[warn]` and is skipped. |
| 2 | Condition field `to or cc` emits `[warn]` and is skipped. |
| 3 | Condition field `all addresses` emits `[warn]` and is skipped. |
| 4 | Condition field `size` emits `[warn]` and is skipped. |
| 5 | Condition field `has attachment` emits `[warn]` and is skipped. |
| 6 | Condition field `date` emits `[warn]` and is skipped. |
| 7 | Condition field `junk status` emits `[warn]` and is skipped. |
| 8 | Condition field `status` emits `[warn]` and is skipped. |
| 9 | Condition field `priority` emits `[warn]` and is skipped. |
| 10 | Condition field `custom header` emits `[warn]` and is skipped. |
| 11 | Match type `is before` emits `[warn]` and is skipped. |
| 12 | Match type `is after` emits `[warn]` and is skipped. |
| 13 | Match type `is empty` emits `[warn]` and is skipped. |
| 14 | Match type `isn't` emits `[warn]` and is skipped (must NOT be silently converted to a positive match). |
| 15 | Action `Copy to folder` emits `[warn]` and is skipped. |
| 16 | Action `Add tag` emits `[warn]` and is skipped. |
| 17 | Action `Stop filter evaluation` emits `[warn]` and is skipped. |
| 18 | ~~Action `Forward to` emits `[warn]` and is skipped.~~ **Superseded by US-75**: `"Forward"` is now converted to `then-forward-to`. |
| 19 | Action `Reply with template` emits `[warn]` and is skipped. |
| 20 | Action `Change Priority` emits `[warn]` and is skipped. |
| 21 | All warnings follow the format already established in US-64. |

---

## Notes

These elements are either difficult to represent in the `rules.ini` format, or
have no counterpart in the email-cli rules engine.  They are intentionally out of
scope for automatic conversion but must never be silently dropped.

The `isn't` match type is a special case: because `strstr(f2, "is")` matches
both `"is"` and `"isn't"`, the current code converts `"isn't"` to a positive
exact-match pattern — this is a bug, not a feature, and is tracked in BUG-001.

---

## Related

* US-64: warn on unsupported elements (base infrastructure)
* BUG-001: `isn't` silently converts to positive match
* US-68: negation conditions (the proper fix for `isn't` and `doesn't contain`)
* US-65–US-70: implementing support for the higher-priority unsupported features
* US-75: additional action support (Forward, Mark read/unread, JunkScore) — supersedes AC18
