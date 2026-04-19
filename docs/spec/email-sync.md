# email-sync Command Specification

## Global syntax

```
email-sync [command] [options]
email-sync --help
email-sync -h
```

---

## `sync` (default)

```
email-sync
email-sync sync
```

Downloads all messages in every IMAP folder to the local store.

### Behaviour

1. Fetch the folder list via IMAP `LIST "" "*"`.
2. Sort folders lexicographically.
3. For each folder:
   a. Issue `UID SEARCH ALL` to obtain all UIDs.
   b. For each UID not already in the local store: fetch full message
      via `BODY.PEEK[]`, save to store, update from/date indexes.
4. Print progress per folder to stdout.

### Output

```
Syncing INBOX ...
  [42/52] UID 1234 fetched
  52 fetched, 0 already stored
Syncing munka/ai ...
  0 fetched, 18 already stored

Sync complete: 52 fetched, 18 already stored
```

### Exit codes

| Code | Condition |
|------|-----------|
| 0 | All folders synced without errors |
| 1 | Folder list fetch failed or at least one UID fetch failed |

---

## `--rebuild-index`

```
email-sync --rebuild-index [--account <email>]
```

Rebuilds Gmail label index files from locally cached `.hdr` files.
Does **not** contact the Gmail API — runs entirely offline.
IMAP accounts are silently skipped (they do not use label indexes).

### When to use

- After restoring a backup that is missing `.idx` files.
- After upgrading from a version that did not write `.idx` files.
- When label message counts appear wrong (e.g. all showing 0 after a
  sync where every message was already cached).
- To repair a corrupted or truncated index without re-downloading messages.

### Behaviour

1. Load all configured accounts (from `config.ini` files).
2. If `--account` is given, restrict to that account; return error if not found.
3. For each Gmail account:
   a. Scan every `.hdr` file in the local message store.
   b. Parse the labels field (4th tab-separated column).
   c. Build an in-memory `(label, uid)` pair list.
   d. Sort by label then UID.
   e. Write one `.idx` file per label (17-byte fixed-width records).
   f. Overwrite any existing `.idx` files atomically.
4. Print a summary per account.

### Output

```
Rebuilding label indexes for test@gmail.com ...
  Scanned 25251 messages, wrote 12 label index files.
Rebuilding label indexes for work@gmail.com ...
  Scanned 8120 messages, wrote 7 label index files.
```

### Options

| Option | Description |
|--------|-------------|
| `--account <email>` | Rebuild only the named account |

### Exit codes

| Code | Condition |
|------|-----------|
| 0 | Indexes rebuilt successfully for all processed accounts |
| 1 | Named account not found, or I/O error |

### Relationship to `email-sync` (normal sync)

Normal sync (`email-sync` without flags) automatically rebuilds label indexes
at the end of every **full sync** (first run or after historyId expiry).
Incremental sync (History API) updates only the affected index files.
`--rebuild-index` is an explicit repair tool for cases where the automatic
rebuild was skipped or the store was modified externally.

---

## `cron`

```
email-sync cron <setup|remove|status>
```

Manages a system cron job for periodic background sync.

### Subcommands

| Subcommand | Description |
|------------|-------------|
| `setup` | Install a cron entry for periodic sync |
| `remove` | Remove the installed cron entry |
| `status` | Show current cron configuration |

### Exit codes

| Code | Condition |
|------|-----------|
| 0 | Operation completed successfully |
| 1 | Operation failed |
