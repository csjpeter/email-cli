# US-73 — Gmail History API Incremental Sync

**As a** Gmail user,
**I want** incremental syncs to use the Gmail History API correctly,
**so that** only changed messages are downloaded and the local store stays consistent.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The `historyId` is captured from the `messages.list` response body (field `"historyId"` at the top level), **not** from `/gmail/v1/users/me/profile`. |
| 2 | After a successful full sync, the saved `historyId` equals the value returned by `messages.list`. |
| 3 | Incremental sync calls `GET /gmail/v1/users/me/history?startHistoryId=<saved>&historyTypes=messageAdded,messageDeleted,labelAdded,labelRemoved`. |
| 4 | `messagesAdded` events: the new message is downloaded (`.eml` + `.hdr`) and added to the relevant `.idx` files. |
| 5 | `messagesDeleted` events: `.eml` and `.hdr` are deleted; the UID is removed from all `.idx` files. |
| 6 | `labelsAdded` events: the UID is inserted into the affected label's `.idx` and removed from `_nolabel.idx` if present. |
| 7 | `labelsRemoved` events: the UID is removed from the affected label's `.idx`; if no labels remain it is added to `_nolabel.idx`. |
| 8 | After a successful incremental sync, `historyId` is updated to the latest value from the history response. |
| 9 | If the server returns **HTTP 404** for the `/history` endpoint (expired `historyId`), a warning is logged, the `historyId` file is deleted, and a full reconcile is triggered automatically. |
| 10 | New messages downloaded via `messagesAdded` are immediately accessible through `email-cli list`. |

---

## historyId capture flow

```
messages.list response body:
{
  "messages": [...],
  "historyId": "7777",   ← save this
  "resultSizeEstimate": 5
}
```

If `messages.list` does not include `historyId`, fall back to
`GET /gmail/v1/users/me/profile` → `"historyId"`.

---

## Incremental sync flow

```
GET /gmail/v1/users/me/history
  ?startHistoryId=<saved>
  &historyTypes=messageAdded,messageDeleted,labelAdded,labelRemoved
  &maxResults=500
```

Page through results until no `nextPageToken`.  
For each history record, process events in order.  
Save the `historyId` from the last page's response.

---

## Error recovery

| Scenario | Behaviour |
|----------|-----------|
| HTTP 404 (expired `historyId`) | Warn, delete `gmail_history_id`, run full reconcile |
| HTTP 429 (rate limit) | Exponential back-off up to 30 s, retry up to 6 times |
| HTTP 5xx | Retry 3× with exponential back-off |

---

## Related

* US-31: Gmail synchronization overview
* US-72: Gmail smart sync orchestration (reconcile / pending_fetch)
* Phase 32 functional tests
