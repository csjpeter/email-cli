# Usage Reference

For the authoritative behavioural specification see [`docs/spec/`](../spec/README.md).
This page is a quick-reference summary for end users.

## Synopsis

```
email-cli [--batch] <command> [options]
```

`--batch` disables the interactive pager and uses a fixed page size (100).
It is implied when stdout is redirected to a pipe or file.

## Commands

| Command | Description |
|---------|-------------|
| `email-cli list` | List unread messages in the configured folder |
| `email-cli list --all` | List all messages (unread marked with `N`) |
| `email-cli list --folder <name>` | Use a different folder |
| `email-cli list --limit <n> --offset <n>` | Manual pagination |
| `email-cli show <uid>` | Display a message by its UID |
| `email-cli folders` | List all IMAP folders |
| `email-cli folders --tree` | Folder hierarchy as a tree |
| `email-cli help [command]` | Show help |

## Output

- `list` prints a table: Status, UID, From, Subject, Date.
- Dates are shown as `YYYY-MM-DD HH:MM` in the local timezone.
- `show` prints From / Subject / Date headers, a separator, then the plain-text body.
- `folders --tree` uses Unicode box-drawing characters.

## Read-status policy

`show` **never permanently marks a message as read**.  If a message was unread
before viewing, it remains unread afterward.

## Caching

- Headers are cached at `~/.cache/email-cli/headers/<folder>/<uid>.hdr`.
- Full messages are cached at `~/.cache/email-cli/messages/<folder>/<uid>.eml`.
- Stale header cache entries are evicted automatically when `list --all` is run.

## Logs

```bash
cat ~/.cache/email-cli/logs/session.log
```

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Error (see stderr and log file) |
