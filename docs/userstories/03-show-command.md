# User Story: Show Command

## Summary
As a user, I want to read the full content of a specific email message identified by
its IMAP UID.

## Invocation
```
email-cli show <uid>
email-cli show --help
```

## Arguments
| Argument | Description |
|----------|-------------|
| `<uid>` | Numeric IMAP UID shown by `email-cli list` |

## Behaviour
1. Checks the local message cache first (`~/.local/share/email-cli/accounts/<host>/`).
2. On a cache miss, fetches the full message from the IMAP server using `BODY.PEEK[]`
   (does NOT set the `\Seen` flag — read status is managed by sync).
3. Parses the RFC 2822 / MIME message:
   - Prefers HTML body (rendered as plain text).
   - Falls back to plain-text body.
4. Prints headers: From, Subject, Date.
5. In TTY mode, paginates the body with an interactive pager (PgDn/PgUp/ESC).
6. In batch/piped mode, prints the body to stdout all at once.

## Caching
- After first fetch, the raw `.eml` is saved locally.
- Subsequent `show` calls for the same UID are served from the local cache.

## Examples
```
email-cli show 42
email-cli show 42 --batch
```

## Exit Codes
- `0` success
- `1` UID not found or fetch error
