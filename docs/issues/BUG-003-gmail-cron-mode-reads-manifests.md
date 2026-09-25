# BUG-003 — Gmail account with a cron sync cannot read its own cache

**Status:** FIXED
Fixed in `libemail/src/domain/email_service.c` and
`libemail/src/infrastructure/local_store.c`.  Covered by functional phase 87
and unit test `test_local_search_gmail`.

**Severity:** High — the account appears empty even though the store is full
**Component:** `libemail/src/domain/email_service.c`,
`libemail/src/infrastructure/local_store.c`
**Reported:** user report, Gmail account with `SYNC_INTERVAL=5`

---

## Description

A Gmail account with any `SYNC_INTERVAL > 0` reported an empty mailbox:

```
$ email-cli-ro list --folder INBOX
No cached data for INBOX. Run 'email-cli sync' first.
```

while `labels/INBOX.idx` held 3122 freshly synced UIDs.  `show` refused to open
messages that were present on disk, and `__search__` found nothing at all.

### Root cause

`email_service_list()` chose its data source in this order:

```c
if      (is_virtual_flags)        /* manifest aggregate */
else if (is_virtual_search)       /* manifest-based search */
else if (cfg->sync_interval > 0)  /* manifests/<folder>.tsv   ← taken */
else if (cfg->gmail_mode)         /* labels/<label>.idx + .hdr ← unreachable */
else                              /* IMAP online */
```

The `sync_interval` test came first, so every Gmail account with a cron sync
was routed into the manifest branch — and `gmail_sync.c` never writes a
manifest.  A grep over it finds zero `manifest_save`/`manifest_upsert`/
`manifest_load` calls against eighteen `label_idx_*` calls.  The listing was
reading a file nothing ever fills.

The deeper mistake is that one condition decided two unrelated things.  The
**storage format** follows the account type (Gmail = flat store keyed by UID +
label indexes; IMAP = per-folder store + manifests), while only the **refresh
policy** follows `sync_interval`.  Ordering the branches this way tied them
together.

Manifests did sometimes exist for Gmail accounts, which is why the symptom was
confusing: the interactive listing path persists a dirty manifest
(`email_service.c`, `manifest_save` after rendering), so a few stale rows with
wrong flag bytes accumulated as a side effect of earlier online listings.  That
produced the reported "1199 unread but no N flag anywhere, and only two pages":
the count came from `label_idx_count("UNREAD")` — the real index — while the
rows came from a 42-row stale manifest.

Two further defects shared the same root:

- **`show`** (`load_message`) looked for the `.eml` under the label name.  Gmail
  stores one copy per message under the **empty** folder, so in cron mode the
  lookup missed and the command refused to connect:
  `Could not load message UID … in folder 'INBOX'`.  The interactive TUI reader
  already handled this; the CLI path did not.
- **`local_search()`** walked `manifests/*.tsv` exclusively, so cross-folder
  search never worked on Gmail in either mode — not a regression, a capability
  that was never reachable.

### Why `SYNC_INTERVAL=0` appeared to fix it

Setting it to 0 skips the manifest branch and lands on the Gmail branch, which
is **purely local** (`.idx` + `.hdr`, no network).  It is therefore a
diagnostic probe, not a fix: it leaves the config claiming there is no cron
sync while one is running, and it changes `show` behaviour, which does go
online when `sync_interval == 0`.

---

## Fix

1. `email_service.c` — the manifest cache branch is now explicitly the IMAP
   one (`cfg->sync_interval > 0 && !cfg->gmail_mode`), so the account type
   decides the storage format and `sync_interval` only decides the refresh
   policy.
2. `email_service.c` — the empty-cache screen (batch message, JSON document +
   stderr advice, TUI panel) was extracted into `list_empty_cache_view()` and
   is now shared: a Gmail label with no index gets the same "run email-sync"
   guidance instead of a bare "no messages" implying an empty mailbox.
3. `email_service.c` — `load_message()` resolves Gmail messages in the flat
   store, since a Gmail folder is a label rather than a location; diagnostics
   still name the label the user typed, not the empty storage folder.
4. `local_store.c` — `local_search()` takes a `gmail_mode` argument and walks
   the `.hdr` records plus the flat `.eml` store for Gmail accounts, making
   cross-folder search work there for the first time.  Results carry the
   message's first Gmail label so listings can name a location per row.
5. `email_service.c` — an IMAP-only virtual view (`__unread__` and friends)
   asked for on Gmail now says so and points at the real label.

## Why it was not caught

The functional suite had ten Gmail configurations and four cron-mode
(`SYNC_INTERVAL=5`) configurations, but no configuration that was both.  Phase
87 now syncs a mock Gmail account with `SYNC_INTERVAL=5` and exercises list,
show and all four search scopes against it.  Verified to fail without the fix:
twelve of its checks fail, while the four that record the premise (sync
succeeds, label index written, no manifest written) still pass.
