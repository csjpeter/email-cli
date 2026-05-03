# TASK-011 — Implement US-75 (import-rules additional action support)

**Type:** Feature + Test
**Related US:** US-75
**Status:** DONE

## Changes

### `libemail/src/infrastructure/mail_rules.h`
- Added `char *then_forward_to` field to `MailRule`.
- Updated comment to document the new field.

### `libemail/src/infrastructure/mail_rules.c`
- `mail_rules_load`: parse `then-forward-to = ...`
- `mail_rules_save`: serialize `then-forward-to = ...`
- `mail_rules_free`: `free(r->then_forward_to)`

### `src/main_import_rules.c`
- Added `cur_pending_forward` state variable (reset at blank lines, `name=`, and each new `action=`).
- `"Mark read"` → alias for `"Mark as read"`: `then-remove-label = UNREAD`
- `"Mark unread"` / `"Mark as unread"` → `then-add-label = UNREAD`
- `"JunkScore"` (TB internal) → `then-add-label = _junk`
- `"Forward"` → `cur_pending_forward = 1`; `actionValue` resolves to `then_forward_to`
- Print / `--output` serialisation includes `then-forward-to`.
- **Dangling pointer fix**: `account_auto = strdup(accounts[0].name)` before `config_free_account_list`.

## Functional tests (Phase 45)

Added to `tests/functional/run_functional.sh`:

| Check | Description |
|-------|-------------|
| 45.1  | `"Mark read"` → `then-remove-label = UNREAD` |
| 45.1b | No `[warn]` for `"Mark read"` |
| 45.2  | `"Mark unread"` → `then-add-label = UNREAD` |
| 45.2b | No `[warn]` for `"Mark unread"` |
| 45.3  | `"JunkScore"` → `then-add-label = _junk` |
| 45.3b | No `[warn]` for `"JunkScore"` |
| 45.4  | `"Forward"` → `then-forward-to = assistant@example.com` |
| 45.4b | No `[warn]` for `"Forward"` |
| 45.4c | Forward rule: `then-move-folder` also preserved |
| 45.5  | Auto-detected account name correct in output |
| 45.5b | Save message contains account name (not glob pattern) |

All 463 functional checks pass. No regressions.
