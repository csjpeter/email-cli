# US-71 — IMAP Incremental Sync (CONDSTORE / RFC 4551)

**As a** user with a CONDSTORE-capable IMAP server,
**I want** subsequent syncs to transfer only changed messages,
**so that** syncs after the first one are fast and produce minimal server traffic.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | On **first sync**, if the server advertises `CONDSTORE` capability, `HIGHESTMODSEQ` and `UIDVALIDITY` are saved to `sync_state/INBOX.tsv` after a successful sync. |
| 2 | On **second sync**, if `HIGHESTMODSEQ` is unchanged, the sync reports "up to date" and performs no `FETCH` or `SEARCH` commands. |
| 3 | When `HIGHESTMODSEQ` has advanced, an incremental sync uses `UID SEARCH CHANGEDSINCE <modseq>` instead of `UID SEARCH ALL` + `UID SEARCH UNSEEN`. |
| 4 | Messages reported by `CHANGEDSINCE` are fetched or have their flags updated as appropriate. |
| 5 | After a successful incremental sync, `sync_state` is updated with the new `HIGHESTMODSEQ`. |
| 6 | If `UIDVALIDITY` has changed (server was reset), the old sync state is discarded and a **full resync** is performed automatically. |
| 7 | If the server does NOT advertise `CONDSTORE`, the regular full-scan sync runs and no `sync_state` file is created. |
| 8 | The `sync_state` file format is `uid\thighestmodseq\tuidvalidity` (TSV). |

---

## sync_state file format

Location: `~/.local/share/email-cli/accounts/<email>/sync_state/<folder>.tsv`

```
INBOX	1234	42
```

Fields: `folder_uid_validity_latest_uid_known` is NOT stored here — only the
CONDSTORE modseq and uidvalidity that allow incremental start.

---

## Sync decision flow

```
load sync_state for folder
if UIDVALIDITY changed → discard sync_state, full resync
else if HIGHESTMODSEQ unchanged → "up to date", skip
else → incremental: UID SEARCH CHANGEDSINCE <saved_modseq>
```

---

## Related

* US-05: IMAP full sync (initial download)
* US-17: email-sync binary
* US-25: account filter
* Phase 33 functional tests
