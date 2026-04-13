# User Story: email-cli-ro — Read-Only Batch CLI

## Summary
As an AI agent or automation script, I want a guaranteed read-only email CLI binary
that can never send email or modify any server state, so I can safely use it without
risk of accidental writes.

## Invocation
```
email-cli-ro <command> [options]
email-cli-ro help [command]
email-cli-ro --help
```

## Supported Commands
| Command | Description |
|---------|-------------|
| `list`    | List messages in the configured folder |
| `show`    | Display a message by UID |
| `folders` | List available IMAP folders |
| `sync`    | Download all messages to local store |
| `help`    | Show help |

## Differences from email-cli
- No interactive TUI: all output is batch/non-interactive.
- No `cron` command (no crontab writes).
- No setup wizard: configuration must already exist.
- No SMTP, no `\Seen` flag changes, no STORE commands.

## Preconditions
- Configuration must already exist at `~/.config/email-cli/config.ini`.
- If no configuration is found, an error message is printed and the process exits non-zero:
  ```
  Error: No configuration found.
  Run 'email-cli' once to complete the setup wizard.
  ```

## Options
All options from `email-cli list` and `email-cli show` are supported, except `--batch`
(which is a no-op accepted for backward compatibility).

## Security Guarantee
The `email-cli-ro` binary is linked only against `libemail` read/fetch paths.
The linker physically excludes any SMTP or STORE code.

## Examples
```
email-cli-ro list
email-cli-ro list --folder INBOX.Sent --limit 20
email-cli-ro show 42
email-cli-ro folders
email-cli-ro sync
```

## Exit Codes
- `0` success
- `1` argument error, configuration missing, or fetch error
