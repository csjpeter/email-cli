# Caching Specification

`email-cli` maintains two local caches under `~/.cache/email-cli/` to reduce
latency and IMAP round-trips.

---

## Directory layout

```
~/.cache/email-cli/
├── headers/
│   └── <folder>/
│       └── <uid>.hdr       ← cached BODY[HEADER] response
├── messages/
│   └── <folder>/
│       └── <uid>.eml       ← cached full RFC 2822 message
└── logs/
    └── session.log
```

`<folder>` is the literal IMAP folder name as returned by the server (e.g. `INBOX`,
`INBOX.Sent`).  Directories are created on first use with mode `0700`.

---

## Header cache (`headers/<folder>/<uid>.hdr`)

### Purpose

Avoids fetching `BODY[HEADER]` from the server on every `list` invocation.
Fetching headers for 845 messages on every listing would be unacceptably slow;
the cache reduces it to a single IMAP round-trip per new/uncached message.

### Write policy

A `.hdr` file is written immediately after a successful `BODY[HEADER]` fetch.
Content is the raw IMAP section body (the bytes returned by libcurl's write
callback for the `/;SECTION=HEADER` URL).

### Read policy

Before issuing any IMAP FETCH for headers, `hcache_exists(folder, uid)` is checked.
If the file exists, `hcache_load(folder, uid)` returns its content and no network
request is made.

### Eviction

Stale entries (headers for messages deleted from the server) are evicted when
`list --all` is invoked and the server returns at least one UID.

Algorithm (`hcache_evict_stale`):

1. Sort the current ALL UID array.
2. Open `~/.cache/email-cli/headers/<folder>/`.
3. For each `*.hdr` file whose numeric basename is **not** present in the sorted
   UID array (binary search), call `remove()` on the file.

Eviction is skipped in `list` (unread-only) mode because the ALL set is not
fetched and therefore cannot be used as the authoritative presence list.

---

## Message cache (`messages/<folder>/<uid>.eml`)

### Purpose

Avoids re-fetching a full message from the server on subsequent `show` invocations.

### Write policy

Written immediately after a successful full-message IMAP fetch (cache-miss path
in `email_service_read`).

### Read policy

`cache_exists(folder, uid)` is checked first.  On a hit, `cache_load(folder, uid)`
returns the full message without any network request — including without the
`\Seen`-check-and-restore logic (which is only needed on a cache miss).

### Eviction

No automatic eviction.  Messages remain cached indefinitely.  A future `purge`
command or manual deletion of `~/.cache/email-cli/messages/` is the expected
maintenance path.

---

## File format

Both `.hdr` and `.eml` files store the **raw bytes** as returned by libcurl's
write callback, NUL-terminated.  No additional framing or metadata is added.
The header cache files contain the IMAP `BODY[HEADER]` section content, which
is a subset of RFC 2822 (header fields only, ending with `\r\n\r\n`).
The message cache files contain the full RFC 2822 message including body.

---

## Thread safety

The cache is not thread-safe.  `email-cli` is a single-threaded CLI tool; no
concurrent access is expected.

---

## Implementation

| Function | Location | Description |
|----------|----------|-------------|
| `cache_exists(folder, uid)` | `src/infrastructure/cache_store.c` | Check if `.eml` file exists |
| `cache_save(folder, uid, data, len)` | same | Write `.eml` file |
| `cache_load(folder, uid)` | same | Read `.eml` file; caller frees |
| `hcache_exists(folder, uid)` | same | Check if `.hdr` file exists |
| `hcache_save(folder, uid, data, len)` | same | Write `.hdr` file |
| `hcache_load(folder, uid)` | same | Read `.hdr` file; caller frees |
| `hcache_evict_stale(folder, uids, count)` | same | Evict stale `.hdr` files |
