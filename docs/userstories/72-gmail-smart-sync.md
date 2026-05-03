# US-72 — Gmail Smart Sync (Reconcile / Pending Fetch / Incremental Orchestration)

**As a** Gmail user,
**I want** `email-sync` to choose the cheapest sync strategy automatically,
**so that** the first run downloads everything, interrupted runs resume cleanly,
and subsequent runs only fetch what has changed.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | On **first sync** (no `historyId` saved), `gmail_sync_reconcile()` is called: all server message UIDs are listed, missing UIDs are added to `pending_fetch.tsv`, and a fresh `historyId` is saved from the `messages.list` response. |
| 2 | After the first sync, `pending_fetch.tsv` is empty (all messages were downloaded). |
| 3 | On **second sync** when `historyId` is valid and `pending_fetch.tsv` is empty, only the **incremental fast path** runs (`gmail_sync_incremental`); no reconcile (no "Listing messages" log line). |
| 4 | If `pending_fetch.tsv` is non-empty but `historyId` is valid (interrupted previous sync), the pending queue is **drained first**, then the incremental path runs — no full reconcile is triggered. |
| 5 | Forcing reconcile by deleting `historyId` causes the next sync to re-list the server ("Listing messages" appears in the log). |
| 6 | `pending_fetch.tsv` is located at `~/.local/share/email-cli/accounts/<email>/pending_fetch.tsv`. |
| 7 | Each entry in `pending_fetch.tsv` is a 16-character hex UID (one per line). |
| 8 | A UID is removed from `pending_fetch.tsv` immediately after its message is successfully downloaded. |

---

## Sync decision flow

```
load historyId
if no historyId:
    reconcile()          ← list all UIDs, populate pending_fetch.tsv, save historyId
drain pending_fetch()    ← download any queued UIDs (no-op if empty)
incremental()            ← History API from saved historyId (no-op if no new events)
```

---

## Local files

| File | Contents |
|------|----------|
| `pending_fetch.tsv` | Queue of UIDs to download (one per line, 16-char hex) |
| `gmail_history_id` | Single-line file containing the latest saved `historyId` |

---

## Related

* US-31: Gmail synchronization (full and incremental overview)
* US-73: Gmail History API incremental sync details
* Phase 30 functional tests
