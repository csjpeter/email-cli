# Developer Guide: Logging

## Overview

The logger writes to a rotating file at `~/.cache/email-cli/logs/session.log`.
All IMAP traffic is captured at DEBUG level via a libcurl debug callback.

## Log Levels

| Level | Meaning |
|-------|---------|
| `LOG_DEBUG` | Full IMAP traffic dump (every byte in/out via CURL) |
| `LOG_INFO` | Application milestones: startup, config loaded, session end |
| `LOG_WARN` | Non-fatal issues: incomplete config, permission warnings |
| `LOG_ERROR` | Fatal errors: connection failure, config write error (also to stderr) |

The default runtime level is `LOG_DEBUG` — everything is logged.

## Log Files

```
~/.cache/email-cli/logs/
  session.log        ← current session
  session.log.1      ← previous session
  ...
  session.log.5      ← oldest kept
```

Rotation triggers when `session.log` exceeds **5 MB**. The oldest file
(`session.log.5`) is deleted; each existing file shifts up by one.

## IMAP Traffic Capture

libcurl's debug callback (`curl_debug_cb` in `curl_adapter.c`) is always
registered. It forwards `CURLINFO_HEADER_IN`, `CURLINFO_HEADER_OUT`,
`CURLINFO_DATA_IN`, and `CURLINFO_DATA_OUT` to `logger_log(LOG_DEBUG, ...)`.
At runtime LOG_INFO level the logger filters these out silently.

To capture a full traffic dump, set `logger_init(path, LOG_DEBUG)` (already the
default in `main.c`) and read `session.log` after a run.

## Purging Logs

```bash
email-cli --clean-logs
```

Deletes all `session.log*` files in the cache directory.
Implemented in `logger_clean_logs()` (`src/core/logger.c`).

## Using the Logger in Code

```c
#include "logger.h"

logger_log(LOG_INFO,  "Connected to %s", host);
logger_log(LOG_WARN,  "Config missing optional field: %s", key);
logger_log(LOG_ERROR, "Failed to open file: %s", path);  // also prints to stderr
```

The logger is a global singleton. Call `logger_init()` once in `main.c` before
any other module uses it. Call `logger_close()` at exit.
