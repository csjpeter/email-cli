# User Story: email-tui — Interactive TUI Binary

## Summary
As a user, I want a dedicated interactive TUI binary (`email-tui`) that provides the
full-featured email experience, and will eventually support write operations (compose,
reply, send, flag) that are intentionally absent from `email-cli-ro`.

## Invocation
```
email-tui [--batch] [command] [options]
```

## Behaviour
`email-tui` is functionally equivalent to `email-cli` at v0.1.4:
1. When run interactively with no command, launches the full-screen TUI.
2. Supports all read commands: `list`, `show`, `folders`, `sync`.
3. Supports cron management: `cron setup`, `cron remove`, `cron status`.
4. Runs the setup wizard on first launch (no existing configuration).
5. In non-TTY mode (or with `--batch`), outputs in batch/scriptable format.
6. All help pages use `email-tui` in usage lines.

## Commands

| Command              | Description                                    |
|----------------------|------------------------------------------------|
| `list`               | List messages in the configured mailbox        |
| `show <uid>`         | Display a message by UID                       |
| `folders`            | List available IMAP folders                    |
| `sync`               | Download all messages to local store           |
| `cron setup/remove/status` | Manage background sync crontab entry    |
| `help [command]`     | Show help                                      |

## Trust Boundary (ADR-0004)
`email-tui` links `libemail` only at v0.1.4. Future write modules (`smtp_adapter`,
`imap_write`) will be linked exclusively to `email-tui` — never to `email-cli-ro`
or `email-sync`. This enforces write capability at the binary level.

## Exit Codes
- `0` success
- `1` error (connection failure, missing configuration, etc.)
