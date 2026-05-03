# US-75 — Import Rules: Additional Action Support

**As a** user importing Thunderbird filters,
**I want** `email-import-rules` to convert the remaining common Thunderbird actions
(mark-read shorthand, mark-unread, junk scoring, forward) without warnings,
**so that** my real-world filter rules are imported completely and the rules file
faithfully captures the original intent.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `action="Mark read"` (Thunderbird shorthand without "as") converts to `then-remove-label = UNREAD` with no `[warn]`. |
| 2 | `action="Mark unread"` (and `"Mark as unread"`) converts to `then-add-label = UNREAD` with no `[warn]`. |
| 3 | `action="JunkScore"` (Thunderbird internal junk scoring) converts to `then-add-label = _junk` — identical outcome to `"Mark as junk"` — with no `[warn]`. |
| 4 | `action="Forward"` with `actionValue="<address>"` converts to `then-forward-to = <address>` with no `[warn]`. |
| 5 | When a rule has both `Forward` and `Move to folder` actions, both are preserved in the output rule. |
| 6 | `then-forward-to` is stored in `rules.ini` and round-trips through `mail_rules_save` / `mail_rules_load`. |
| 7 | The auto-detected account name in the "Rules saved to …" message is always the real account name, never a glob pattern from rule data (fix for dangling-pointer bug). |

---

## Notes

AC 1–3 extend US-66 (flag actions) with aliases and the internal JunkScore action.

AC 4 replaces US-74 AC18 ("Forward to emits `[warn]`"): `"Forward"` is now fully
converted; actual forwarding execution at sync time is future work.

AC 7 fixes a memory bug: `config_list_accounts()` returns heap-allocated names that
are freed by `config_free_account_list()`.  The account pointer was previously set
to `accounts[0].name` before the free, leaving a dangling pointer.

---

## Related

* US-64: warn infrastructure
* US-66: flag action conversion (base)
* US-74: niche Thunderbird elements (Forward moved here from warn list)
