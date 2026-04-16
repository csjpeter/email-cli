# US-25 — email-sync Single-Account Sync Filter

**As a** user with multiple configured email accounts,
**I want to** run `email-sync --account <email>` to sync only one specific account,
**so that** I can avoid waiting for all accounts to sync when I only care about one.

---

## Acceptance Criteria

| # | Criterion |
|---|-----------|
| 1 | `email-sync --account <email>` syncs only the account whose profile directory name matches `<email>` exactly. |
| 2 | All IMAP folders of the matched account are fetched into the local store. |
| 3 | Other configured accounts are left untouched. |
| 4 | If no account with the given name is found, the command exits with status 1 and prints `Account '<email>' not found.` to stderr. |
| 5 | `--account` requires a value; `email-sync --account` (without value) exits with status 1 and prints an error. |
| 6 | `--account <email>` is documented in `email-sync --help`. |

---

## Invocation

```
email-sync --account alice@example.com
```

---

## Behaviour

- Without `--account`, every configured account is synced in alphabetical order.
- With `--account`, only the account whose directory name equals the given value is synced.
- If the value matches no account, exit 1 with `Account '<email>' not found.` on stderr.

---

## Implementation Notes

- Matching uses `strcmp(accounts[i].name, only_account)` — exact, case-sensitive.
- The account name is the profile directory name under `~/.config/email-cli/accounts/` (typically the full email address).
- Only named accounts are eligible; there is no "default" fallback.
- `email_service_sync_all(only_account)` in `libemail/src/domain/email_service.c` handles the filtering.

---

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Sync completed successfully |
| `1` | Account not found, or option error, or sync failure |
