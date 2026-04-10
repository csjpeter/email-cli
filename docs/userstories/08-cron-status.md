# User Story: Cron Status

## Summary
As a user, I want to check whether an automatic background sync is currently
scheduled in my crontab.

## Invocation
```
email-cli cron status
```

## Behaviour
1. Reads the current user crontab.
2. Searches for any line containing `email-cli sync`.
3. If found, prints the matching crontab entry.
4. If not found, prints a message indicating no entry is installed.

## Example Output (entry installed)
```
Cron entry found:
*/5 * * * * /usr/local/bin/email-cli sync
```

## Example Output (not installed)
```
No email-cli sync cron entry found.
```

## Note
This command does NOT require configuration to be loaded. It can run even before
`email-cli` has been configured.

## Exit Codes
- `0` always (informational command)
