# US-77 — Mail Rules: Apply Rules Pushes Changes to IMAP Server

**As a** user who runs `email-sync --apply-rules` to retroactively sort cached messages,
**I want** the resulting flag and folder-move changes to be pushed to the IMAP server immediately,
**so that** my mailbox on the server stays in sync with the locally applied rules without a separate sync step.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | After `email-sync --apply-rules` applies rules locally, it automatically pushes all pending flag changes to the IMAP server. |
| 2 | After `email-sync --apply-rules` applies rules locally, it automatically pushes all pending folder-move operations to the IMAP server (UID COPY + STORE `\Deleted` + EXPUNGE). |
| 3 | The output includes a `Syncing rule changes to server` line before the server push begins. |
| 4 | The output includes a `Rules applied:` summary line. |
| 5 | No `Error:` is printed when the server accepts the operations. |
| 6 | Pending move operations are flushed before pending flag changes. |
| 7 | After a successful push, the `pending_moves/` queue files are deleted. |
| 8 | For Gmail accounts, folder-move is silently skipped (label-based model; use label operations instead). |
| 9 | `--dry-run` mode applies rules locally and prints what would be pushed, but does not contact the server. |

---

## Example output

```
$ email-sync --apply-rules --verbose
=== Applying rules: user@example.com ===
  [rule] "Archive test messages" → uid:1001  +Archived  →Archive
  Rules applied: 1 message(s) modified.
Syncing rule changes to server...
  Moving uid:1001 → Archive
Sync complete.
```

---

## Implementation notes

- **Pending move queue:** `local_pending_move_add/load/clear()` — stored in
  `pending_moves/<folder>.tsv` (same pattern as `pending_flags`).
- **IMAP move:** `imap_uid_move()` = UID COPY to target + STORE `\Deleted` + EXPUNGE on source.
- **Dispatch:** `mail_client_move_to_folder()` routes to `imap_uid_move()` for IMAP accounts
  and is a no-op for Gmail accounts.
- **Ordering:** `email_service_sync_all()` flushes `pending_moves` before `pending_flags`.
- **Entry point:** `main_sync.c` calls `sync_all()` immediately after `apply_rules()`.

---

## Related

* US-59: rules apply (local-only, no server push)
* US-60: rules apply --dry-run
* US-58: sync --verbose (rule firing log)
* US-57: rules list
