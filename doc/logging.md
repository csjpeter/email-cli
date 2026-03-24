# Logging Strategy

## Overview
The `email-cli` uses a rotating logging system to record application behavior and IMAP traffic. This is critical for debugging real-world server interactions and reproducing bugs.

## Log Location
Logs are stored in `~/.cache/email-cli/logs/`.
- `session.log`: The current active log.
- `session.log.1` to `session.log.5`: Rotated history files.

## Log Levels
- **DEBUG**: Full traffic dump (all IMAP requests/responses).
- **INFO**: Application milestones (startup, connection success, shutdown).
- **WARN**: Non-fatal issues (missing optional config, permission hiccups).
- **ERROR**: Fatal errors (connection failure, write errors).

## Traffic Capture
By setting the level to `DEBUG`, the application logs every byte sent to and received from the IMAP server via the `CURL` adapter. This allows developers to:
1. Identify protocol mismatches.
2. Capture raw server responses for use in functional test cases.
3. Verify that sensitive data (like passwords) is handled correctly.

## Self-Cleaning
Users can purge all log data using the CLI command:
```bash
email-cli --clean-logs
```
This is implemented in `fs_util.c` and `logger.c`.
