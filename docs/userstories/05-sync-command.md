# User Story: Sync Command

## Summary
As a user, I want to download all messages from all IMAP folders to local storage so
that I can browse email offline and so the TUI is fast.

## Invocation
```
email-cli sync
```

## Behaviour
1. Checks for a running sync via PID file (`~/.cache/email-cli/sync.pid`).
   If another `email-cli sync` is already running, exits immediately.
2. Fetches the full folder list from the IMAP server (`LIST "" "*"`).
3. Saves the folder list to the local cache (`folders.cache`) for instant future use.
4. For each folder:
   a. Issues `UID SEARCH ALL` to get the current UID list.
   b. Issues `UID SEARCH UNSEEN` to determine unread status.
   c. Evicts manifest entries for deleted messages.
   d. For each UID not yet in the local message store:
      - Fetches the full message with `BODY.PEEK[]` (does NOT mark as read).
      - Saves the raw `.eml` file and updates the full-text index.
   e. Updates the manifest (uid, from, subject, date, unseen flag).
   f. Prints per-folder progress: `N fetched, M already stored`.
5. Prints a final summary.
6. Removes the PID file on exit.

## Concurrency
- Only one `email-cli sync` runs at a time (enforced by PID file + process name check).
- A stale PID file from a crashed run is detected via `kill(pid, 0)` + `/proc/<pid>/comm`.

## Progress Output
```
Syncing INBOX ...
  [1/10] UID 42
  ...
  3 fetched, 7 already stored
Syncing INBOX.Sent ...
  (empty)

Sync complete: 3 fetched, 7 already stored
```

## Exit Codes
- `0` success (even if some messages skipped as already stored)
- `1` errors during sync (partial completion possible)
