# US-41: Account Management

## Summary

As a user, I want to list, add, and remove accounts from the command line (batch mode), in addition to the existing interactive TUI.

## Commands

- `email-cli show-accounts` — list all configured accounts with type and server
- `email-cli add-account` — run the interactive setup wizard to add a new account
- `email-cli remove-account <email>` — remove an account by email address

## Acceptance Criteria

### show-accounts

1. Prints a table: Account (email), Type (Gmail/IMAP), Server.
2. Works even if no accounts are configured (prints "No accounts configured.").
3. Does NOT require a loaded account config (can run without any existing config).
4. Available in `email-cli-ro`.

### add-account

1. Runs `setup_wizard_run()` interactively.
2. Saves the new account via `config_save_account()`.
3. Prints "Account '<email>' added." on success.
4. Does NOT trigger the auto-wizard at startup (bypasses the "no config found → wizard" logic).
5. Blocked in `email-cli-ro`.

### remove-account

1. Removes the account configuration entry.
2. Local messages are NEVER deleted — they are PRESERVED on disk.
3. Prints the preservation path so the user knows where local data lives.
4. Prints instructions for manual deletion if desired.
5. Returns an error if the account is not found.
6. Does NOT require a loaded account config.
7. Blocked in `email-cli-ro`.

## TUI Accounts Screen

The `d` key (delete account) now shows a preservation notice in the info line after deletion, informing the user that local data was kept:

> "Account removed. Local messages preserved: <path>"

The status bar includes `(*keeps local data)` annotation next to `d=delete*`.

## Notes

- `show-accounts` and `remove-account` operate on `config_store` directly and do not require network connectivity.
- The `remove-account` path uses `platform_data_dir()` to compute the local data directory.
