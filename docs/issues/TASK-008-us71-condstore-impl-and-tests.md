# TASK-008 — Implement US-71 (IMAP CONDSTORE) and add functional tests

**Type:** Feature + Test  
**Related US:** US-71  

## Implementation

### 1. `sync_state` file I/O helper

New file: `libemail/src/infrastructure/imap_sync_state.c` (+ `.h`)

```c
typedef struct {
    char    folder[256];
    uint64_t highestmodseq;
    uint32_t uidvalidity;
} ImapSyncState;

int imap_sync_state_load(const char *account, const char *folder, ImapSyncState *out);
int imap_sync_state_save(const char *account, const ImapSyncState *state);
void imap_sync_state_clear(const char *account, const char *folder);
```

File location: `~/.local/share/email-cli/accounts/<email>/sync_state/<folder>.tsv`  
Format: `<folder>\t<highestmodseq>\t<uidvalidity>`

### 2. `libemail/src/infrastructure/imap_client.c`

Extend `imap_select_folder()` response parser to capture `HIGHESTMODSEQ` and
`UIDVALIDITY` from the `SELECT` response.

Add `imap_search_changedsince(uint64_t modseq, uint32_t **uids, int *count)`.

### 3. `libemail/src/infrastructure/imap_client.h`

```c
int imap_get_capabilities(ImapClient *ic, char ***caps, int *count);
int imap_search_changedsince(ImapClient *ic, uint64_t modseq,
                              uint32_t **uids, int *count);
```

### 4. `libemail/src/domain/email_service.c` / sync path

In `email_service_sync()`:
1. After `SELECT`, check if server advertised `CONDSTORE`.
2. Load `sync_state` for the folder.
3. If `UIDVALIDITY` changed → clear state, do full sync.
4. If `HIGHESTMODSEQ` unchanged → log "up to date", skip fetch.
5. Else → `UID SEARCH CHANGEDSINCE <saved_modseq>`, fetch only those UIDs.
6. After successful sync → save new `sync_state`.
7. If server does NOT advertise CONDSTORE → full scan, no state saved.

## Mock server extension

`tests/functional/mock_imap_server.c` must support:
- Advertising `CONDSTORE` in `CAPABILITY` response (via env flag `MOCK_CONDSTORE=1`)
- Returning `HIGHESTMODSEQ` in `SELECT` responses
- Handling `UID SEARCH CHANGEDSINCE <modseq>`

## Functional tests (Phase 45)

- `45.1` First sync saves `sync_state/<folder>.tsv`
- `45.2` Second sync (modseq unchanged) reports "up to date", no FETCH
- `45.3` Second sync (modseq advanced) uses `CHANGEDSINCE`, fetches new UIDs
- `45.4` UIDVALIDITY change → full resync, old state discarded
- `45.5` Server without CONDSTORE → full scan, no `sync_state` file created

## Definition of done

All Phase 45 checks pass, no regression.
