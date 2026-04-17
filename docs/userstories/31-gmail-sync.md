# US-31 — Gmail Synchronization

**As a** Gmail user,
**I want** email-cli to synchronize my messages and labels efficiently,
**so that** I can read my mail offline and subsequent syncs are fast.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | **Full sync** (first run): all messages are fetched via paginated `messages.list`, raw `.eml` and `.hdr` files are saved, and `.idx` label files are populated. |
| 2 | **Incremental sync** (subsequent runs): only changes since `historyId` are fetched via the Gmail History API, processing `messagesAdded`, `messagesDeleted`, `labelsAdded`, `labelsRemoved` deltas. |
| 3 | `historyId` is saved to `gmail_history_id` after each successful sync. |
| 4 | If incremental sync fails with `historyId` not found (HTTP 404), a warning is logged and a full resync is triggered automatically. |
| 5 | `.idx` files are re-sorted after any modification. |
| 6 | `_nolabel.idx` (Archive) is maintained: UIDs with no labels are added; UIDs gaining a label are removed. |
| 7 | Deleted messages (server-side) have their `.eml` and `.hdr` files removed and UIDs purged from all `.idx` files. |
| 8 | Pressing **s** in the TUI triggers an incremental sync in the foreground. |
| 9 | The `email-sync` binary supports Gmail accounts via `--account` filter. |

---

## Full sync flow

1. `GET /gmail/v1/users/me/messages?maxResults=500` (paginated)
   → collect all message IDs.
2. For each message ID:
   - `GET messages/{id}?format=raw` → base64url decode → save as `.eml`
   - `GET messages/{id}?format=metadata` → extract headers + `labelIds` → save as `.hdr`
3. Populate `.idx` files: for each label on each message, append UID to that
   label's `.idx`.
4. Sort all `.idx` files.
5. Build `_nolabel.idx` from messages with no labels.
6. Save `historyId` from the most recent message.

---

## Incremental sync flow

1. `GET /gmail/v1/users/me/history?startHistoryId=<saved>&historyTypes=messageAdded,messageDeleted,labelAdded,labelRemoved`

2. Process delta records:

   | Delta type | Local action |
   |-----------|-------------|
   | `messagesAdded` | Download `.eml` + `.hdr`; insert UID into relevant `.idx` files |
   | `messagesDeleted` | Delete `.eml` + `.hdr`; remove UID from all `.idx` files |
   | `labelsAdded` | Insert UID into the label's `.idx`; remove from `_nolabel.idx` if present |
   | `labelsRemoved` | Remove UID from the label's `.idx`; if no labels remain → add to `_nolabel.idx` |

3. Re-sort any modified `.idx` files.
4. Save new `historyId`.

---

## Error recovery

| Scenario | Behaviour |
|----------|-----------|
| `historyId` not found (HTTP 404 on `/history`) | Log warning, delete all `.idx` files, perform full resync |
| HTTP 429 (rate limit) | Exponential backoff: 1s → 2s → 4s → 8s → 16s → 30s cap; retry up to 6 times |
| HTTP 5xx (server error) | Retry 3 times with exponential backoff |
| Network timeout | Display error in status bar; user can retry with `s` |

---

## Local store layout (Gmail)

```
~/.local/share/email-cli/accounts/<email>/
  store/<d1>/<d2>/<uid>.eml
  headers/<d1>/<d2>/<uid>.hdr
  labels/
    INBOX.idx
    SENT.idx
    STARRED.idx
    UNREAD.idx
    DRAFTS.idx
    <UserLabel>.idx
    _nolabel.idx
    _spam.idx
    _trash.idx
  gmail_history_id
```

---

## Implementation notes

* `gmail_sync.c` implements `gmail_full_sync()` and `gmail_incremental_sync()`.
* All messages use the unified 16-character hex UID (native Gmail message ID).
* `.idx` files use fixed 17-byte records (16-char UID + newline) for O(log N)
  binary search.
* `gmail_history_id` is a single integer file (the most recent `historyId`).

---

## Related

* Spec: `docs/spec/gmail-api.md` sections 7, 8
* GML milestones: GML-07, GML-08
