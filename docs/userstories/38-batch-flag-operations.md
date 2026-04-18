# US-38: Batch Flag Operations

## Summary

As a user, I want to mark messages as read/unread or starred/unstarred from the command line (batch mode), so that I can script flag changes without the interactive TUI.

## Commands

- `email-cli mark-read <uid> [--folder <name>]` — remove UNSEEN flag (mark as read)
- `email-cli mark-unread <uid> [--folder <name>]` — add UNSEEN flag (mark as unread)
- `email-cli mark-starred <uid> [--folder <name>]` — add FLAGGED flag (star)
- `email-cli remove-starred <uid> [--folder <name>]` — remove FLAGGED flag (unstar)

## Acceptance Criteria

1. Each command updates the local manifest (flag bitmask) immediately.
2. For Gmail accounts, the corresponding Gmail label index is updated:
   - mark-read: removes UID from UNREAD index
   - mark-unread: adds UID to UNREAD index
   - mark-starred: adds UID to STARRED index
   - remove-starred: removes UID from STARRED index
3. The change is added to the pending flag queue for next sync.
4. A synchronous server push is attempted immediately; if it fails, a warning is shown and the pending queue ensures retry on next sync.
5. `--folder` is optional; defaults to the configured folder.
6. UIDs are numeric strings matching the format from `email-cli list`.
7. The commands are blocked in `email-cli-ro` with a clear error message.

## Implementation Notes

- Implemented via `email_service_set_flag()` in `email_service.c`.
- `MSG_FLAG_UNSEEN` semantics: `imap_add = !flag_add` (add UNSEEN = remove \\Seen on IMAP).
- `MSG_FLAG_FLAGGED`: `imap_add = flag_add` (direct mapping to \\Flagged).
