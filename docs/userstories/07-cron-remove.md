# User Story: Cron Remove

## Summary
As a user, I want to remove the automatic background sync crontab entry.

## Invocation
```
email-cli cron remove
```

## Behaviour
1. Reads the current user crontab.
2. Removes any line containing `email-sync` (or legacy `email-cli sync`).
3. Installs the filtered crontab.
4. If no such entry was found, prints a message and exits normally.

## Example Output (entry removed)
```
Cron job removed.
```

## Example Output (not found)
```
No email-sync cron entry found.
```

## Note
This command does NOT require configuration to be loaded. It can run even before
`email-cli` has been configured.

## Exit Codes
- `0` success or not found
- `1` error (e.g., `crontab` command not found)
