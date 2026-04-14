# User Story: Background Sync from TUI List View

## Summary
As a user viewing the message list in `email-tui` or `email-cli`, I want to trigger a
background sync without leaving the list view, so that new messages are fetched while I
continue reading.

## Invocation
Press `s` while the interactive message list is active.

## Preconditions
- Configuration exists and the message list is displayed.
- `email-sync` binary is available in the same directory as the running binary (or on
  `PATH`).

## Flow

1. User presses `s` in the message list.
2. The application forks and execs `email-sync` as a child process.
3. The count line immediately changes to `⟳ syncing...` to indicate sync is in progress.
4. The screen is not redrawn on `SIGCHLD`; the notification is shown on the next keypress
   after the child has exited.
5. On the next keypress after the child exits (success or failure), the count line changes
   to `✉ New mail may have arrived! R=refresh`.
6. User presses `R` to reload the message list from local cache.
7. The list refreshes in place; the notification clears and the normal count line returns.

## Key Bindings

| Key | Action |
|-----|--------|
| `s` | Start background sync |
| `R` | Refresh list from local cache |

## Status Line States

| State | Count line text |
|-------|----------------|
| Idle | `1-20 of 42 unread message(s) in INBOX.` |
| Syncing | `⟳ syncing...` |
| Sync done (pending refresh) | `✉ New mail may have arrived! R=refresh` |

## Error Handling
- If `email-sync` exits with a non-zero code, the notification still reads
  `✉ New mail may have arrived! R=refresh` (the user can refresh to see whether new
  messages appeared before the error).
- If a sync is already in progress and the user presses `s` again, the key is silently
  ignored (no second child is spawned).

## Constraints
- The list view remains fully interactive during sync (keypresses are processed normally).
- `SIGCHLD` does not trigger a screen redraw; only the next regular keypress checks
  whether the child has exited.
- Applies to both `email-tui` and `email-cli`.
