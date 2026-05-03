# TASK-008 — Implement US-71 (IMAP CONDSTORE) and add functional tests

**Type:** Feature + Test  
**Related US:** US-71  
**Status:** DONE

## Implementation (completed)

### 1. `sync_state` file I/O

Implemented in `libemail/src/infrastructure/local_store.c`:

```c
typedef struct { uint32_t uidvalidity; uint64_t highestmodseq; } FolderSyncState;

int  local_sync_state_load (const char *folder, FolderSyncState *state);
int  local_sync_state_save (const char *folder, const FolderSyncState *state);
void local_sync_state_clear(const char *folder);
```

File location: `~/.local/share/email-cli/accounts/<email>/sync_state/<folder>.tsv`  
Format: `<uidvalidity>\t<highestmodseq>\n`

### 2. `libemail/src/infrastructure/imap_client.c`

`imap_select_condstore()` parses `HIGHESTMODSEQ` and `UIDVALIDITY` from `SELECT`.  
`imap_uid_fetch_flags_changedsince()` issues `UID FETCH 1:* (UID FLAGS) (CHANGEDSINCE n)`.

### 3. `libemail/src/domain/email_service.c` — sync path

In `email_service_sync()`:
1. After `SELECT`, loads saved `sync_state` for the folder.
2. If `UIDVALIDITY` changed → clear state, do full sync (warns on stderr).
3. If `HIGHESTMODSEQ` unchanged → prints "up to date", skips fetch.
4. If `HIGHESTMODSEQ` advanced → `CHANGEDSINCE` fetch, applies flag updates.
5. After successful sync → saves new `sync_state`.
6. If server does NOT advertise `CONDSTORE` → full scan, no state saved.

### 4. Mock server extension (`tests/functional/mock_imap_server.c`)

Env vars:
- `MOCK_IMAP_CAPS=CONDSTORE` — advertises CONDSTORE capability
- `MOCK_IMAP_MODSEQ=<n>` — HIGHESTMODSEQ in SELECT response
- `MOCK_IMAP_UIDVAL=<n>` — UIDVALIDITY override
- `MOCK_IMAP_CHANGED_COUNT=<n>` — UIDs 1..N returned for CHANGEDSINCE

## Functional tests (Phase 33)

Originally planned as Phase 45; implemented as Phase 33 in
`tests/functional/run_functional.sh`:

| Check | Description |
|-------|-------------|
| 33.1  | First sync with CONDSTORE: message fetched |
| 33.2  | `sync_state/INBOX.tsv` contains saved uidvalidity |
| 33.2b | `sync_state/INBOX.tsv` contains saved modseq |
| 33.3  | Same modseq → "up to date", no server traffic |
| 33.4  | New modseq → not "up to date" |
| 33.5  | Incremental: `CHANGEDSINCE` command sent to server |
| 33.6  | `sync_state` updated to new modseq |
| 33.7  | UIDVALIDITY changed → resync warning on stderr |
| 33.8  | UIDVALIDITY changed → new uidval saved in sync_state |
| 33.9  | No CONDSTORE → regular sync succeeds |
| 33.10 | No CONDSTORE → no `sync_state` file created |

All 11 checks pass. No regressions.
