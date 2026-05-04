# BUG-002 — IMAP Modified UTF-7 Folder Names Displayed Raw in Rules Detail View

## Summary

Folder names that contain non-ASCII characters (e.g. `és pénzügy`) are stored in
`rules.ini` using IMAP modified UTF-7 encoding (RFC 3501, e.g. `&AOk-s p&AOk-nz&APw-gy`)
because the rule was created while the raw IMAP folder name was in scope. The rules detail
view and edit form display the encoded form instead of the decoded UTF-8 string.

## Reproduction

1. Open a rule whose `then-move-folder` targets a folder with accented characters.
2. Navigate to that rule in the TUI rules editor (Rules → select rule → Enter).
3. Observe `then-move-folder` shows e.g. `hivatalos &AOk-s p&AOk-nz&APw-gy` instead
   of `hivatalos és pénzügy`.

### Observed

```
  then-move-folder: hivatalos &AOk-s p&AOk-nz&APw-gy
```

### Expected

```
  then-move-folder: hivatalos és pénzügy
```

## Root Cause

The folder name is received from the IMAP server in modified UTF-7 encoding and stored
verbatim in `rules.ini` without prior decoding. Neither `mail_rules_load` nor the TUI
display functions decode IMAP modified UTF-7 before presenting the value to the user.

## Fix

Decode IMAP modified UTF-7 → UTF-8 at the point where folder names enter the system:

- **`mail_rules_load`**: when reading `then-move-folder`, call
  `imap_utf7_decode(raw, decoded_buf, sizeof(decoded_buf))` (already available in
  `libemail/src/core/imap_util.c`) and store the decoded UTF-8 string.
- **`mail_rules_save`**: when writing `then-move-folder`, the value is already UTF-8 so
  write it directly (do **not** re-encode to modified UTF-7; the ini file stores
  human-readable UTF-8).
- **Rule creation / import**: any code path that sets `then-move-folder` from an IMAP
  folder name must decode before storing.

## Affected Files

- `libemail/src/infrastructure/config_store.c` — `mail_rules_load` / `mail_rules_save`
- `src/main_tui.c` — `tui_rules_detail`, `tui_rules_add_form` (display only; fix is in loader)
- Any other path that copies an IMAP folder name into a `MailRule`

## Acceptance Criteria

| # | Criterion |
|---|-----------|
| 1 | `mail_rules_load` decodes `then-move-folder` values from IMAP modified UTF-7 to UTF-8. |
| 2 | `mail_rules_save` writes the UTF-8 decoded folder name (no re-encoding). |
| 3 | The rules detail view shows `és pénzügy`, not `&AOk-s p&AOk-nz&APw-gy`. |
| 4 | The rules edit form pre-fills the `then-move-folder` field with the decoded UTF-8 name. |
| 5 | A unit test covers `mail_rules_load` with a `then-move-folder` containing modified UTF-7. |
