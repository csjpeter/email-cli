# User Story: List Command

## Summary
As a user, I want to list email messages in a mailbox folder from the command line,
with control over which messages are shown and how many per page.

## Invocation
```
email-cli list [options]
email-cli list --help
```

## Options
| Option | Description |
|--------|-------------|
| `--all` | Show all messages (unread first, read after); no-op for backward compat |
| `--folder <name>` | Use a specific folder instead of the configured default |
| `--limit <n>` | Max messages per page (default: terminal height or 100 in batch) |
| `--offset <n>` | Start from message number n (1-based) |
| `--batch` | Disable interactive pager; use fixed limit of 100 |

## Behaviour
- Messages are always shown: unread ones first (marked `N`), then read ones.
- In interactive mode (TTY), the pager is active: arrow keys / PgDn scroll; ESC quits.
- In batch mode or when output is piped, all messages up to `--limit` are printed.
- Pressing Backspace in interactive mode returns control to the caller (folder browser).

## Examples
```
email-cli list
email-cli list --all
email-cli list --all --offset 21
email-cli list --folder INBOX.Sent --limit 50
email-cli list --all --batch
```

## Exit Codes
- `0` success (or user quit)
- `1` argument error
