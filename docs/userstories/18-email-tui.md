# User Story: email-tui — Interactive TUI Binary

## Summary
As a user, I want a dedicated interactive TUI binary (`email-tui`) that provides the
full-featured email experience including write operations (compose, reply, send, flag)
that are intentionally absent from `email-cli-ro` and `email-cli`.

## Invocation
```
email-tui [--batch] [command] [options]
```

## Behaviour

### Accounts Screen
1. When run interactively with no command, `email-tui` opens the accounts screen first.
2. The accounts screen shows the configured IMAP account (address and host).
3. Pressing Enter opens the message list for the default folder directly (folder browser
   is skipped on this path).
4. Pressing `e` opens the SMTP setup wizard (`setup_wizard_smtp`).
5. Pressing ESC quits the application.

### Message List (reached from accounts screen)
6. The message list behaves identically to `email-cli`, plus additional write-capable keys:
   - `c` — compose a new message (opens `$EDITOR`; see [US-20](20-compose-reply.md)).
   - `r` — reply to the selected message (opens `$EDITOR` with quoted body).
   - `s` — start background sync (see US-19).
   - `R` — refresh the list from local cache.
   - `n` / `f` / `d` — set IMAP keyword flags on the selected message.
7. In the message reader, `r` also opens a reply (see [US-20](20-compose-reply.md)).
8. Status bar in list view (100-column terminal):
   ```
     ↑↓=step  PgDn/PgUp=page  Enter=open  Backspace=folders  ESC=quit
     c=compose  r=reply  n=new  f=flag  d=done  s=sync  R=refresh  [n/N]
   ```

### General
9. Supports all read commands: `list`, `show`, `folders`, `sync`.
10. Supports cron management: `cron setup`, `cron remove`, `cron status`.
11. Runs the IMAP setup wizard on first launch (no existing configuration).
12. In non-TTY mode (or with `--batch`), outputs in batch/scriptable format.
13. All help pages use `email-tui` in usage lines.

## Commands

| Command                      | Description                                    |
|------------------------------|------------------------------------------------|
| `list`                       | List messages in the configured mailbox        |
| `show <uid>`                 | Display a message by UID                       |
| `folders`                    | List available IMAP folders                    |
| `sync`                       | Download all messages to local store           |
| `config smtp`                | Set or update SMTP credentials                 |
| `cron setup/remove/status`   | Manage background sync crontab entry           |
| `help [command]`             | Show help                                      |

## Navigation Hierarchy
```
Accounts screen  (Enter=open, e=SMTP settings, ESC=quit)
  └─ Message list  (Backspace=folders, s=sync, R=refresh, c=compose, r=reply, ESC=quit)
       └─ Folder browser  (Backspace=back/up, ESC=quit)
```

## Trust Boundary (ADR-0004)
Write modules (`smtp_adapter`, `imap_write`) are linked exclusively to `email-tui` —
never to `email-cli-ro` or `email-sync`. This enforces write capability at the binary
level.

## Exit Codes
- `0` success
- `1` error (connection failure, missing configuration, etc.)
