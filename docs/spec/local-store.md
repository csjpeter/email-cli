# Local Store Specification

`email-cli` stores messages and headers locally under
`~/.local/share/email-cli/accounts/imap.<host>/` using reverse digit bucketing.
Text-based indexes provide efficient lookup by sender and date.

---

## Account layout

Each IMAP server gets its own account directory, derived from the host URL:

```
~/.local/share/email-cli/
  accounts/
    imap.<hostname>/
      store/<folder>/<d1>/<d2>/<uid>.eml     ← full RFC 2822 messages
      headers/<folder>/<d1>/<d2>/<uid>.hdr   ← header-only copies
      index/from/<domain>/<localpart>        ← sender index
      index/date/<year>/<month>/<day>        ← date index
  ui.ini                                     ← UI preferences
```

`<hostname>` is the lowercased hostname extracted from the IMAP URL
(e.g. `imaps://mail.example.com:993` → `imap.mail.example.com`).

Logs remain at `~/.cache/email-cli/logs/` (transient data).

---

## Reverse digit bucketing

Messages and headers are distributed into subdirectories based on the
**last two decimal digits** of the UID:

```
d1 = uid % 10        (last digit)
d2 = (uid / 10) % 10 (second-to-last)
```

Examples:

| UID | d1 | d2 | Path |
|-----|----|----|------|
| 137 | 7 | 3 | `store/INBOX/7/3/137.eml` |
| 42 | 2 | 4 | `store/INBOX/2/4/42.eml` |
| 5 | 5 | 0 | `store/INBOX/5/0/5.eml` |
| 10002 | 2 | 0 | `store/INBOX/2/0/10002.eml` |

This yields 100 buckets (10 × 10), each holding at most ~100 files per
10,000 messages.

**Key advantage: scalability.** If a bucket grows too large, it can be
further split by a third digit (e.g. `7/3/` → `7/3/1/`, `7/3/2/`)
without reorganising the entire tree.  The total number of messages need
not be known in advance.

IMAP folder paths map naturally to filesystem directories:
`munka/ai` → `store/munka/ai/<d1>/<d2>/<uid>.eml`.

---

## Text indexes

Indexes are plain text files, one reference per line.  The reference
format for IMAP is `<folder>/<uid>` (e.g. `INBOX/42`).

### Sender index (`index/from/<domain>/<localpart>`)

Created when a message is first stored.  The `From:` header is parsed to
extract the email address; `@` becomes a directory boundary:

```
index/from/github.com/noreply     ← "INBOX/42\nINBOX/1042\n"
index/from/amazon.de/versand      ← "INBOX/891\n"
```

Domain and local part are lowercased.

### Date index (`index/date/<year>/<month>/<day>`)

Created from the `Date:` header, converted to local timezone:

```
index/date/2024/03/15     ← "INBOX/42\nmunka/ai/137\n"
index/date/2024/04/01     ← "INBOX/1042\n"
```

### Duplicate prevention

Before appending a reference, the index file is scanned for an existing
identical line.  Duplicates are silently skipped.

### Querying indexes

Indexes are designed for use with standard shell tools and AI agents:

```bash
cat index/from/github.com/noreply      # all emails from noreply@github.com
cat index/date/2024/03/*               # all emails from March 2024
grep "INBOX/" index/from/github.com/*  # github emails in INBOX
```

---

## Header store (`headers/<folder>/<d1>/<d2>/<uid>.hdr`)

Avoids fetching `BODY[HEADER]` on every `list` invocation.

### Write policy

Written immediately after a successful `BODY[HEADER]` fetch.

### Eviction

Stale entries are evicted when `list --all` is invoked.  All 100 bucket
directories are scanned; any `.hdr` file whose UID is absent from the
current ALL set is removed.

---

## Message store (`store/<folder>/<d1>/<d2>/<uid>.eml`)

Stores the complete RFC 2822 message including MIME parts (attachments
are stored as base64-encoded data within the message, not extracted).

### Write policy

Written on first access (cache miss in `show` or `sync`).  The index is
updated immediately after a successful save.

### Eviction

No automatic eviction.  Messages persist indefinitely.

---

## Initialisation

`local_store_init(host_url)` must be called once before any other
`local_*` function.  It extracts the hostname and sets the account base
path.  Called from `main.c` after configuration is loaded.

---

## Implementation

| Function | Location | Description |
|----------|----------|-------------|
| `local_store_init(host_url)` | `src/infrastructure/local_store.c` | Set account base from IMAP URL |
| `local_msg_exists(folder, uid)` | same | Check if `.eml` exists |
| `local_msg_save(folder, uid, data, len)` | same | Write `.eml` file |
| `local_msg_load(folder, uid)` | same | Read `.eml`; caller frees |
| `local_msg_delete(folder, uid)` | same | Remove `.eml` and `.hdr` |
| `local_hdr_exists(folder, uid)` | same | Check if `.hdr` exists |
| `local_hdr_save(folder, uid, data, len)` | same | Write `.hdr` file |
| `local_hdr_load(folder, uid)` | same | Read `.hdr`; caller frees |
| `local_hdr_evict_stale(folder, uids, n)` | same | Evict stale `.hdr` files |
| `local_index_update(folder, uid, raw)` | same | Update from/ and date/ indexes |
| `ui_pref_get_int(key, default)` | same | Read UI preference |
| `ui_pref_set_int(key, value)` | same | Write UI preference |

---

## File format

Both `.hdr` and `.eml` files store raw bytes as returned by the IMAP client,
NUL-terminated.  No additional framing or metadata.  Index files are
UTF-8 plain text with Unix line endings.

---

## Thread safety

The local store is not thread-safe.  `email-cli` is single-threaded.
