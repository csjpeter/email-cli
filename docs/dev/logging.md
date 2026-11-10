# Developer Guide: Logging

## Overview

There is **no session log** (EMAIL-284). What a failure has to say is in the
error message on stderr (`../policies/logging-model.md`); no INFO, WARN or
milestone line is written anywhere.

The one file is the **traffic log**, `~/.cache/email-cli/logs/session.log`,
made only when `--debug` is on the command line (the option is the one way;
there is no environment variable). Without `--debug` no file is created and no
log directory is made. `logger_debug_requested()` decides and removes `--debug`
from argv.

## What the traffic log holds

| Prefix | Where | Content |
|--------|-------|---------|
| `IMAP [OUT]` / `IMAP [ IN]` | `imap_client.c` | commands and response lines |
| `SMTP [OUT]` / `SMTP [ IN]` | `smtp_adapter.c` | commands and reply lines |
| `HTTP [OUT]` / `HTTP [ IN]` | `http_client.c` | Gmail REST request line, headers, status line |

## Credentials never reach the file

`logger_traffic()` runs every line through `logger_mask_secret()` before it is
written: `LOGIN user pass`, `AUTHENTICATE <mech> <data>`, `AUTH <mech> <data>`,
the `Authorization:` header value, and the values of `access_token`,
`refresh_token`, `client_secret` and `password` become `xxxxx`. The data lines
of an SMTP `AUTH LOGIN` exchange are masked by `smtp_adapter.c` (the step is
known there). Message bodies are not logged. `test_logger_masks_credentials`
feeds each kind and searches the file.

## Files

```
~/.cache/email-cli/logs/        (0700)
  session.log        ← current run, 0600
  session.log.1 … .5 ← older, five kept
```

Rotation happens when `session.log` exceeds **5 MB** at the next `--debug` run:
the oldest file is deleted and each existing file shifts up by one.

## Purging

```bash
email-cli --clean-logs
```

Deletes all `session.log*` files (`logger_clean_logs()` in `core/logger.c`).

## In code

```c
#include "logger.h"

logger_traffic("IMAP [OUT] %s %s", tag, cmd);   // --debug only, masked
logger_error("TLS handshake failed: %s", why);  // stderr, "ERROR: " prefix
```

Do not add `logger_traffic` calls outside a protocol send/receive function.
`logger_error` is the stderr output of the library modules; a new error path
builds its message with what failed, where, what the other side said and what
to try (see the project CLAUDE.md, "Error messages instead of a log file").
