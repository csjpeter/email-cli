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
