# IMAP Protocol Behaviour

## Library

All IMAP communication is handled via **libcurl** (minimum version tested: 8.5.0)
with TLS support via OpenSSL.  The `curl_adapter` infrastructure layer wraps
libcurl handle lifecycle and TLS configuration.

## TLS policy

| Setting | Value |
|---------|-------|
| `CURLOPT_USE_SSL` | `CURLUSESSL_TRY` — upgrade to TLS if server supports it |
| `CURLOPT_SSLVERSION` | `CURL_SSLVERSION_TLSv1_2` — minimum TLS 1.2 |
| `CURLOPT_SSL_VERIFYPEER` / `VERIFYHOST` | `1` in production; `0` when `ssl_no_verify=true` in config |

`imaps://` scheme forces TLS unconditionally.  `imap://` uses STARTTLS if available.

---

## IMAP operations issued

### `UID SEARCH`

Used by `list` to enumerate messages.

```
CURLOPT_CUSTOMREQUEST = "UID SEARCH UNSEEN"   # or "UID SEARCH ALL"
URL                   = imap://<host>/<folder>
```

Response: `* SEARCH <uid1> <uid2> ...`  Parsed by `parse_uid_list()`.

### `UID FETCH … (FLAGS)`

Used by `show` (cache-miss path) to check the current `\Seen` state before
fetching the full message.

```
CURLOPT_CUSTOMREQUEST = "UID FETCH <uid> (FLAGS)"
URL                   = imap://<host>/<folder>
```

Response: `* <seq> FETCH (FLAGS (\Seen …))` or `(FLAGS ())`.
The presence of `\Seen` in the response string determines `was_seen`.

### `UID FETCH` full message (via URL)

Used by `show` to retrieve the complete RFC 2822 message.

```
URL = imap://<host>/<folder>/;UID=<uid>    # no CURLOPT_CUSTOMREQUEST
```

libcurl internally sends `UID FETCH <uid> BODY[]`.  The full literal payload is
delivered to the write callback.  **This implicitly sets `\Seen`** (RFC 3501 §6.4.5).

> **Important libcurl limitation**: when `CURLOPT_CUSTOMREQUEST` is set on a
> UID URL (`/;UID=N`), libcurl only delivers the IMAP envelope line (e.g.
> `* 1 FETCH (BODY[] {123}\r\n`) to the write callback; the literal payload is
> processed internally as `CURLINFO_HEADER_IN` and never reaches the callback.
> Therefore BODY.PEEK[] **cannot** be used via CUSTOMREQUEST to avoid setting
> `\Seen`.  The fetch-and-restore pattern is used instead (see below).

### `UID FETCH … BODY[HEADER]` (via URL)

Used by `list` to retrieve message headers.

```
URL = imap://<host>/<folder>/;UID=<uid>/;SECTION=HEADER
```

libcurl internally sends `UID FETCH <uid> BODY[HEADER]`.  Per RFC 3501, fetching
only the `HEADER` section does **not** set `\Seen`.

### `UID STORE … -FLAGS (\Seen)`

Used by `show` to restore the unread state after a full-message fetch, when the
message was not already read.

```
CURLOPT_CUSTOMREQUEST = "UID STORE <uid> -FLAGS (\Seen)"
URL                   = imap://<host>/<folder>
```

This is a no-op if `was_seen` was true (i.e. the message was already read before
the fetch).

### `LIST "" "*"`

Used by `folders` to enumerate all folders.

```
CURLOPT_CUSTOMREQUEST = "LIST \"\" \"*\""
URL                   = imap://<host>/
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

`imap_utf7_decode(const char *s)` in `src/core/imap_util.c`:

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

All IMAP traffic is forwarded to the logger at `LOG_DEBUG` level via
`CURLOPT_DEBUGFUNCTION`.  The callback logs:
- `CURLINFO_HEADER_OUT` / `CURLINFO_DATA_OUT` → `[OUT] <trimmed line>`
- `CURLINFO_HEADER_IN` / `CURLINFO_DATA_IN` → `[ IN] <trimmed line>`

Log destination: `~/.cache/email-cli/logs/session.log`.
