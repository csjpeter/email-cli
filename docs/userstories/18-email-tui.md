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
1. When run interactively with no command, `email-tui` **always** opens the accounts screen first — even on subsequent launches.
2. The account cursor is restored to the position of the last-used account from the previous session.
3. Pressing Enter opens the folder browser for the selected account.
4. The folder browser cursor is restored to the last-used folder for that account (per-account preference, persisted across sessions).
5. Pressing `e` opens the SMTP setup wizard for the selected account.
6. Pressing `i` opens the IMAP setup wizard for the selected account.
7. Pressing `n` adds a new account via the setup wizard.
8. Pressing `d` deletes the selected account.
9. Pressing ESC or `q` quits the application.

### Folder Browser
10. On entry, the cursor is positioned on the last folder visited for this account.
    Falls back to the configured default folder (`EMAIL_FOLDER`) or `INBOX`.
11. Pressing Enter opens the message list for the selected folder.
12. Pressing Backspace at the root level returns to the accounts screen.

### Message List (reached from folder browser)
13. The message list behaves identically to `email-cli`, plus additional write-capable keys:
    - `c` — compose a new message (opens `$EDITOR`; see [US-20](20-compose-reply.md)).
    - `r` — reply to the selected message (opens `$EDITOR` with quoted body).
    - `s` — start background sync (see US-19).
    - `R` — refresh the list from local cache.
    - `n` / `f` / `d` — set IMAP keyword flags on the selected message.
14. In the message reader, `r` also opens a reply (see [US-20](20-compose-reply.md)).
15. Pressing Backspace returns to the folder browser.

### Cursor Persistence
The following cursor positions are persisted to `~/.local/share/email-cli/ui.ini`:

| Key | Value |
|-----|-------|
| `last_account` | username of the last-opened account |
| `folder_cursor_<user>` | last folder visited in that account's folder browser |

### General
16. Supports all read commands: `list`, `show`, `folders`, `sync`.
17. Supports cron management: `cron setup`, `cron remove`, `cron status`.
18. Runs the IMAP setup wizard on first launch (no existing configuration).
19. In non-TTY mode (or with `--batch`), outputs in batch/scriptable format.
20. All help pages use `email-tui` in usage lines.

## Navigation Hierarchy
```
Accounts screen  (Enter=folder browser, n=add, d=delete, i=IMAP, e=SMTP, ESC=quit)
  └─ Folder browser  (Enter=message list, Backspace=accounts, ESC=quit)
       └─ Message list  (Backspace=folder browser, c=compose, r=reply, s=sync, ESC=quit)
            └─ Message reader  (r=reply, Backspace=list, ESC=quit)
```

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

## Trust Boundary (ADR-0004)
Write modules (`smtp_adapter`, `imap_write`) are linked exclusively to `email-tui` —
never to `email-cli-ro` or `email-sync`. This enforces write capability at the binary
level.

## Exit Codes
- `0` success
- `1` error (connection failure, missing configuration, etc.)
