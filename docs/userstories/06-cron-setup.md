# User Story: Cron Setup

## Summary
As a user, I want to install an automatic background sync that runs periodically via
my user crontab, without needing sudo or system access.

## Invocation
```
email-cli cron setup
```

## Behaviour
1. Reads `sync_interval` from config (minutes between syncs).
2. If `sync_interval` is not configured (≤ 0), defaults to 5 minutes, saves the
   default to config, and informs the user.
3. Checks the current user crontab for an existing `email-cli sync` entry.
4. If an entry already exists, prints a message and exits without modifying the crontab.
5. Otherwise, adds a new crontab entry: `*/<interval> * * * * /path/to/email-cli sync`
   where `/path/to/email-cli` is resolved from `/proc/self/exe` (Linux) or `argv[0]`.

## Example Output (new installation)
```
sync_interval not configured; using default of 5 minutes.
Cron job installed: */5 * * * * /usr/local/bin/email-cli sync
```

## Example Output (already installed)
```
Cron job already installed. Run 'email-cli cron remove' first to change the interval.
```

## Exit Codes
- `0` success or already installed
- `1` error (e.g., `crontab` command not found)
