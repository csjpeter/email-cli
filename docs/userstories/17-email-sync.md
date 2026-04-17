# User Story: email-sync — Standalone Sync Binary

## Summary
As a user or system administrator, I want a dedicated, single-purpose binary that
synchronises my email to the local store, so it can be reliably scheduled from cron
without depending on the interactive email-cli binary.

## Invocation
```
email-sync
email-sync --help
```

## Behaviour
1. Reads configuration from `~/.config/email-cli/config.ini`.
2. Lists all IMAP folders from the server.
3. For each folder, fetches all messages not yet in the local store.
4. Reports progress to stdout: `Syncing <folder> ...` per folder.
5. Prints a summary on completion: `Sync complete: N fetched, M already stored`.
6. Exits with code 0 on success, non-zero on error.

## Options
| Option | Description |
|--------|-------------|
| `--help`, `-h` | Print help and exit |

## No-Configuration Error
If no configuration file exists, prints:
```
Error: No configuration found.
Run 'email-cli' once to complete the setup wizard.
```
and exits with code 1.

## Cron Integration
`email-sync cron setup` installs `email-sync` (not `email-cli sync`) in the user's
crontab:
```
*/15 * * * * /path/to/email-sync >> ~/.cache/email-cli/sync.log 2>&1
```
`email-sync cron remove` and `email-sync cron status` detect both `email-sync` and
the legacy `email-cli sync` format.

## Differences from `email-cli sync`
- Single purpose: no subcommands, no interactive mode.
- Lighter startup: no TUI, no setup wizard.
- Designed for non-interactive/headless invocation (cron, systemd timer, CI).

## Exit Codes
- `0` sync successful (or all messages already stored)
- `1` configuration missing, connection failure, or partial error
