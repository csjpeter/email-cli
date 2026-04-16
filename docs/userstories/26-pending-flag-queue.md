# US-26 — Offline Pending Flag Queue

**As a** user working in cron/offline mode,
**I want** flag changes (read/starred/done) I make in the TUI to be remembered and
applied to the IMAP server on the next sync,
**so that** I can manage message flags even when the server is temporarily unreachable.

---

## Acceptance Criteria

| # | Criterion |
|---|-----------|
| 1 | Pressing `n` / `f` / `d` in the interactive message list immediately updates the local manifest and re-renders the status column, regardless of connectivity. |
| 2 | When an IMAP connection is active (online mode), the flag change is pushed to the server immediately via `UID STORE`. |
| 3 | The flag change is **always** appended to a pending queue file (`pending_flags/<folder>.tsv`) under the account's local store directory, regardless of online/offline state. |
| 4 | On the next `email-sync` run, all pending flag changes are applied to the IMAP server (via `UID STORE`) before fetching new messages, and the queue file is deleted after successful upload. |
| 5 | If the pending queue file does not exist (no pending changes), sync proceeds normally without error. |
| 6 | Queue entries use the tab-separated format: `<uid>\t<flag_name>\t<add>`, where `add` is `1` (set flag) or `0` (remove flag). |

---

## Key Bindings

| Key | Effect | IMAP flag name |
|-----|--------|----------------|
| `n` | Toggle New (unread/seen) | `\Seen` |
| `f` | Toggle Flagged (starred) | `\Flagged` |
| `d` | Toggle Done | `$Done` |

---

## Status Column Format

The status column in the message list shows a 4-character field:

```
N*DA
│││└── A = attachment present
││└─── D = $Done flag set
│└──── * = \Flagged set
└───── N = \Seen not set (unread)
```

Flag changes are reflected immediately on re-render via optimistic local-manifest update.

---

## Storage

Pending flag file path:
```
~/.local/share/email-cli/accounts/<user>/pending_flags/<folder>.tsv
```

Example content (UID 42 marked Seen, UID 17 un-flagged):
```
42	\Seen	1
17	\Flagged	0
```

---

## Offline Operation

In cron/offline mode (`SYNC_INTERVAL > 0` in config), `list_imap` is `NULL` — flag
changes are **only queued**, not pushed to the server immediately.  The manifest is
updated optimistically so the UI reflects the change without a network round-trip.

On the next successful `email-sync`, the pending queue is flushed before new messages
are fetched, ensuring server state stays consistent with the user's local view.
