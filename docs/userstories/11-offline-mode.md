# User Story: Offline / Cron Mode

## Summary
As a user who has set up automatic background sync, I want the interactive TUI and
list commands to work instantly from the local cache without connecting to the server.

## Trigger
`sync_interval > 0` in config (set via `email-sync cron setup`).

## Behaviour
- `email-cli list` and the interactive TUI serve message lists entirely from the
  local manifest cache — no IMAP connection is made.
- Message counts and unread status come from the manifest files
  (`<account_base>/manifests/<folder>.tsv`).
- Opening a message (`show` or Enter in TUI) that is not yet in the local cache
  shows an error; the user must run `sync` first or wait for cron.
- If the manifest for a folder is empty (not yet synced), the TUI shows a waiting
  screen: "No data yet. Press Backspace/ESC."

## Manual sync
The user can always run `email-sync` at any time to fetch new messages on demand.

## Folder list
The folder list is served from `folders.cache` (no server call) when in cron mode.
