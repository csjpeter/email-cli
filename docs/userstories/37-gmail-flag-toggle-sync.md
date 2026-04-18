# US-37: Gmail Flag Toggle with Local + Server Sync

## User Story

As a Gmail user in the TUI,
I want to toggle read/unread and starred status on messages,
so that changes are reflected immediately in the label list counts
and synchronized with the Gmail server.

## Acceptance Criteria

1. **Toggle read status (`n` key):**
   - Pressing `n` on an unread message marks it as read
   - The UNREAD label index is updated immediately (UID removed from `.idx`)
   - The `.hdr` file flags field is updated to reflect the new state
   - When returning to the label list, the UNREAD count decreases by 1
   - Pressing `n` again marks it back as unread (UID re-added to UNREAD `.idx`)

2. **Toggle starred status (`f` key):**
   - Pressing `f` on an unstarred message stars it
   - The STARRED label index is updated immediately
   - The `.hdr` file flags field is updated
   - When returning to the label list, the STARRED count increases by 1

3. **Server synchronization (online):**
   - If a Gmail API connection is available, the label change is pushed
     to the server immediately (`UNREAD` label removed/added via Gmail API)
   - The connection is created lazily on first flag toggle (not at list load)

4. **Offline mode:**
   - If the Gmail API is unreachable, local state is still updated
   - The flag change is queued in the pending flag queue for next sync
   - Next `email-sync` run applies the queued changes to the server

5. **Consistency:**
   - The manifest, `.hdr` flags, and `.idx` files all reflect the same state
   - Re-entering a label view after toggling shows the correct messages

## Technical Notes

- `label_idx_remove("UNREAD", uid)` / `label_idx_add("UNREAD", uid)`
- `local_hdr_update_flags("", uid, new_flags)` updates the 5th tab field
- `mail_client_set_flag()` translates `\\Seen` to Gmail UNREAD label ops
- Lazy Gmail connection: `list_mc = make_mail(cfg)` on first toggle
