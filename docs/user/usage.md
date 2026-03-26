# Usage Reference

## Synopsis

```
email-cli [options]
```

## Commands

Running without arguments fetches the most recent emails from the configured mailbox.

| Command | Description |
|---------|-------------|
| `email-cli` | Fetch and display up to 10 most recent emails |
| `email-cli --clean-logs` | Delete all log files in `~/.cache/email-cli/logs/` |
| `email-cli --help` | Show help message |

## Fetch Behaviour

1. Connects to the IMAP server specified in config.
2. Selects the configured folder (default: `INBOX`).
3. Issues `UID SEARCH ALL` to list all message UIDs.
4. Fetches the **10 most recent** messages (highest UIDs) and prints them to stdout.
5. Exits with code `0` on success, `1` on any fetch error.

## Output Format

```
--- Fetching recent emails from imaps://imap.example.com/INBOX ---
Showing 3 most recent of 47 message(s).

══════════════════════════════════════════
 Message 1/3  (UID 147)
══════════════════════════════════════════
<raw message content>

══════════════════════════════════════════
 Message 2/3  (UID 146)
══════════════════════════════════════════
...

Fetch complete. Success.

Success: Fetch complete.
```

Message content is printed as-is from the server (raw IMAP body, including headers).

## Logs

Diagnostic logs are written to `~/.cache/email-cli/logs/session.log`.
All IMAP traffic is logged at DEBUG level. Check the log file if a connection fails:

```bash
cat ~/.cache/email-cli/logs/session.log
```

To purge all log files:

```bash
email-cli --clean-logs
```

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | All messages fetched successfully |
| `1` | One or more messages failed to fetch, or fatal error |
