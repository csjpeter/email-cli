# IMAP Protocol Behaviour

## Library

All IMAP communication is handled via the **imap_client** infrastructure layer,
which uses direct TCP sockets with **OpenSSL** for TLS.  The `imap_client` module
manages connection lifecycle, TLS negotiation, and IMAP command/response parsing.

## TLS policy

| Setting | Value |
|---------|-------|
| Protocol | TLS 1.2+ via OpenSSL |
| Certificate verification | Enabled in production; disabled when `ssl_no_verify=true` in config |

`imaps://` scheme forces TLS unconditionally.  `imap://` connects without TLS (for local testing only).

---

## IMAP operations issued

### `UID SEARCH`

Used by `list` to enumerate messages.

```
Command: UID SEARCH UNSEEN   (or UID SEARCH ALL)
```

Response: `* SEARCH <uid1> <uid2> ...`  Parsed by `parse_uid_list()`.

### `UID FETCH … (FLAGS)`

Used by `show` (cache-miss path) to check the current `\Seen` state before
fetching the full message.

```
Command: UID FETCH <uid> (FLAGS)
```

Response: `* <seq> FETCH (FLAGS (\Seen …))` or `(FLAGS ())`.
The presence of `\Seen` in the response string determines `was_seen`.

### `UID FETCH` full message (via URL)

Used by `show` to retrieve the complete RFC 2822 message.

```
Command: UID FETCH <uid> BODY[]
```

The imap_client sends `UID FETCH <uid> BODY[]` and reads the full literal payload.
**This implicitly sets `\Seen`** (RFC 3501 §6.4.5).
The fetch-and-restore pattern is used to preserve the unread state (see below).

### `UID FETCH … BODY[HEADER]` (via URL)

Used by `list` to retrieve message headers.

```
Command: UID FETCH <uid> BODY[HEADER]
```

Per RFC 3501, fetching only the `HEADER` section does **not** set `\Seen`.

### `UID STORE … -FLAGS (\Seen)`

Used by `show` to restore the unread state after a full-message fetch, when the
message was not already read.

```
Command: UID STORE <uid> -FLAGS (\Seen)
```

This is a no-op if `was_seen` was true (i.e. the message was already read before
the fetch).

### `LIST "" "*"`

Used by `folders` to enumerate all folders.

```
Command: LIST "" "*"
```

Response: one `* LIST (flags) sep mailbox` line per folder.

---

## `\Seen` flag invariant

> **Viewing a message with `show` must never permanently change its `\Seen` flag.**

Implementation:

```
1. was_seen = (UID FETCH uid (FLAGS) response contains "\Seen")
2. raw      = URL-fetch uid          ← sets \Seen on server
3. if not was_seen:
       UID STORE uid -FLAGS (\Seen)  ← restore unread state
```

If the fetch succeeds but the STORE fails (network error), a warning is logged but
the error is not propagated — the message content is still returned to the caller.

---

## IMAP Modified UTF-7 decoding

IMAP folder names may contain non-ASCII characters encoded as IMAP Modified UTF-7
(RFC 3501 §5.1.3).  This encoding differs from standard UTF-7 (RFC 2152) in two
ways:

1. The shift sequence uses `&` as the start delimiter instead of `+`.
2. The base64 alphabet uses `,` in place of `/`.

### Encoding scheme

- `&-` → literal `&`
- `&<modified-base64>-` → UTF-16BE code units, decoded to Unicode code points,
  then re-encoded as UTF-8.

### Implementation

`imap_utf7_decode(const char *s)` in `libemail/src/core/imap_util.c`:

- Iterates the input string.
- Outside a shift sequence: copies bytes verbatim, except `&-` → `&`.
- Inside a shift sequence (between `&` and `-`): accumulates modified-base64
  digits, decodes 16-bit pairs to Unicode code points, handles surrogate pairs
  (U+D800–U+DFFF for code points > U+FFFF), encodes each code point as UTF-8.
- Returns a heap-allocated UTF-8 string; caller must `free()`.

### Test vector

| Encoded | Decoded (UTF-8) |
|---------|-----------------|
| `INBOX` | `INBOX` |
| `&-` | `&` |
| `&AOk-` | `é` (U+00E9) |
| `&AOE-rv&AO0-zt&AXE-r&AVE-t&APw-k&APY-rf&APo-r&APM-g&AOk-p` | `árvíztűrőtükörfúrógép` |
| `INBOX.&AOk-p&AO0-tkez&AOk-s` | `INBOX.éptkezés` (example) |

---

## Debug logging

All IMAP traffic is forwarded to the logger at `LOG_DEBUG` level.
Sent commands and received responses are logged as `[OUT]` / `[ IN]` lines.

Log destination: `~/.cache/email-cli/logs/session.log`.
