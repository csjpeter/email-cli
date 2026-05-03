# TASK-012 — Implement US-76 (import-rules prefs.js account mapping)

**Type:** Feature + Test
**Related US:** US-76
**Status:** DONE

## Problem

Without reading Thunderbird's `prefs.js`, the importer had no reliable way to
determine which ImapMail subdirectory belongs to which account.  Two Gmail
accounts share the same hostname (`imap.gmail.com`), so hostname-based matching
assigned every account all rules from all subdirectories.

## Changes

### `src/main_import_rules.c`

- Added `TBAccountEntry` struct: `{ char hostname[256]; char username[256]; char dir[256]; }`.
- Added `extract_hostname(url, buf, buflen)`: strips scheme and port from an IMAP URL.
- Added `parse_tb_prefs(profile_path, entries, max)`: reads `prefs.js` line by line,
  groups `mail.server.serverN.{hostname,userName,directory-rel,type}` entries by N,
  strips `[ProfD]ImapMail/` prefix from `directory-rel`, returns entry count.
- Added `find_tb_dir_for_account(entries, count, email, host, dir_out, dir_out_size)`:
  matches by hostname (case-insensitive) AND userName (full email or local-part);
  returns 1 and fills `dir_out` on success.
- `scan_tb_named_dir(parent, dir_name, out)`: exact subdirectory lookup (no globbing).
- `process_account(account, tb_dir_name, tb_path, dry_run, output)`: accepts exact dir name.
- `main()` rewritten:
  - Parses prefs.js once; both single-account and multi-account modes use
    `find_tb_dir_for_account` for exact per-account directory resolution.
  - Single-account: no match → error + `EXIT_FAILURE`.
  - Multi-account: no match → "skipping" message, continues with next account.

## Functional tests (Phase 46)

Added to `tests/functional/run_functional.sh`:

| Check | Description |
|-------|-------------|
| 46.1  | `ta46` rules file contains `AlphaRule` (its own rule) |
| 46.2  | `ta46` rules file does NOT contain `BetaRule` (isolation) |
| 46.3  | `tb46` rules file contains `BetaRule` (its own rule) |
| 46.4  | `tb46` rules file does NOT contain `AlphaRule` (isolation) |
| 46.5  | `tc46` (no matching prefs.js entry) reported as skipped |
| 46.6  | `tc46/rules.ini` not created |

Also added `make_tb_prefs` helper to `run_functional.sh` and called it in each
import-rules phase (38–45) to provide the required prefs.js in every test profile.

All 469 functional checks pass. No regressions.
