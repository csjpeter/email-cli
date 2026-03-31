# Manifest Cache Specification

## Problem

Entering a folder for the first time requires one IMAP `FETCH BODY.PEEK[HEADER]`
per message in the visible window. With 10+ messages this takes 3+ seconds.
Even with cached `.hdr` files, the individual file reads and header parsing
add ~500ms per page.

## Solution: per-folder manifest

A **manifest** is a pre-parsed summary of all messages in a folder, stored as
a single TSV file. It contains the display-ready From, Subject, and Date for
each UID, eliminating per-message file I/O and header parsing during rendering.

## Storage

```
accounts/imap.<host>/manifests/<folder-path>.tsv
```

Example: `manifests/INBOX.tsv`, `manifests/munka/ai.tsv`

## File format

Tab-separated values, one line per message, sorted by UID descending:

```
<uid>\t<from>\t<subject>\t<date>\n
```

Fields are already MIME-decoded and date-formatted (display-ready).
Embedded tabs in values are replaced with spaces on write.
The file is UTF-8 encoded with Unix line endings.

## Flow: entering a folder

```
1. SEARCH UNSEEN         → unseen_uids[]     (1 IMAP request)
2. SEARCH ALL            → all_uids[]        (1 IMAP request)
3. Load manifest         → manifest_uids[]
4. new_uids   = all_uids − manifest_uids     (set difference)
5. stale_uids = manifest_uids − all_uids     (deleted on server)
6. For each new UID:
     FETCH BODY.PEEK[HEADER]                  (1 IMAP request per new message)
     Parse From, Subject, Date
     Add to manifest
7. Remove stale_uids from manifest
8. Save manifest
9. Render from manifest (no file I/O, no parsing)
```

**Cold start:** all UIDs are new → same cost as before, but manifest is saved.
**Warm start:** only genuinely new messages are fetched → near-instant.

## Unseen flag

The manifest does NOT cache unseen status because it changes frequently
(user reads messages on other clients). The unseen flag is always obtained
fresh from `SEARCH UNSEEN` and merged at render time.

## Eviction

When a UID disappears from `SEARCH ALL`, it is removed from the manifest
and its `.hdr` file is evicted (existing `local_hdr_evict_stale` logic).

## API

```c
/* Load manifest for a folder. Returns NULL if not found. Caller must free. */
Manifest *manifest_load(const char *folder);

/* Save manifest for a folder. Returns 0 on success. */
int manifest_save(const char *folder, const Manifest *m);

/* Free a manifest and all its entries. */
void manifest_free(Manifest *m);

/* Find entry by UID. Returns pointer into manifest or NULL. */
ManifestEntry *manifest_find(const Manifest *m, int uid);

/* Add or update an entry. */
void manifest_upsert(Manifest *m, int uid, const char *from,
                     const char *subject, const char *date);

/* Remove entries whose UID is not in keep_uids. */
void manifest_retain(Manifest *m, const int *keep_uids, int keep_count);
```

## Data structures

```c
typedef struct {
    int   uid;
    char *from;      /* MIME-decoded, display-ready */
    char *subject;   /* MIME-decoded, display-ready */
    char *date;      /* formatted as "YYYY-MM-DD HH:MM" */
} ManifestEntry;

typedef struct {
    ManifestEntry *entries;
    int count;
    int capacity;
} Manifest;
```

## Implementation location

`src/infrastructure/local_store.c` — alongside existing store functions.
