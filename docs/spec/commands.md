# Command Specification

## Global syntax

```
email-cli [--batch] <command> [command-options]
```

`--batch` is a **global flag** and may appear anywhere in the argument list, before
or after the command name.  It is detected by a pre-scan of all arguments before
dispatching to the command handler.

### Global option: `--help`

`--help` anywhere in the argument list shows the relevant help page and exits:

```
email-cli --help            → general help
email-cli list --help       → same as: email-cli help list
email-cli show --help       → same as: email-cli help show
email-cli folders --help    → same as: email-cli help folders
email-cli send --help       → same as: email-cli help send
```

`--help` is processed before any other dispatch logic.

### Global flag: `--batch`

| Condition | Effect |
|-----------|--------|
| `--batch` present | Disables interactive pager; uses fixed default page size (100) |
| stdout is not a terminal (`isatty(STDOUT_FILENO) == 0`) | Same as `--batch` |
| neither | Terminal height is detected via `ioctl TIOCGWINSZ`; interactive pager enabled |

---

## `help`

```
email-cli help [<command>]
email-cli           # equivalent to `help` when no command is given
```

Prints usage text to **stdout** and exits with code **0**.

If `<command>` is given and unknown, prints an error to **stderr** and exits **1**.

Supported topics: `list`, `show`, `folders`, `attachments`, `save-attachment`, `send`, `config`.

---

## `list`

```
email-cli [--batch] list [--all] [--folder <name>] [--limit <n>] [--offset <n>]
```

Lists messages in the configured (or overridden) IMAP folder.

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--all` | off | Show all messages; without it only UNSEEN messages are listed |
| `--folder <name>` | `cfg.folder` | Override the IMAP folder for this invocation only |
| `--limit <n>` | terminal height − 6, or 100 in batch mode | Maximum rows per page; must be a positive integer |
| `--offset <n>` | 1 | 1-based index of the first message to display |

### Behaviour

1. Issue `UID SEARCH UNSEEN` to obtain the set of unread UIDs.
2. If `--all`: additionally issue `UID SEARCH ALL` to obtain all UIDs.
3. Build a combined entry list tagged with read/unread status.
4. Sort: **unread messages first**, then by **descending UID** within each group
   (higher UID = more recent message).
5. Apply `--offset` (1-based; convert to 0-based cursor position) and `--limit`.
6. For each message in the visible window: fetch headers from cache or server
   (see [local-store.md](local-store.md)), parse `From`, `Subject`, `Date`; render table row.
7. When `--all` and the full UID set was fetched, call `local_hdr_evict_stale` to
   remove cached headers for UIDs no longer present on the server.
8. Paginate / interact according to [pagination.md](pagination.md).

### Interactive cursor mode (default, no `--batch`)

In interactive mode `list` operates as a full-screen TUI with a persistent
cursor:

- A **highlighted row** (reverse video) marks the currently selected entry.
- The visible window scrolls automatically to keep the cursor on screen.
- The cursor starts at the position implied by `--offset` (default: first row).
- **The loop never exits automatically** when the cursor reaches the first or
  last row; the user must press `q` or `ESC` to quit.
- Pressing **Enter** opens the selected message with `show_uid_interactive()`.
  - If the user presses `q` inside the message view, the list also exits.
  - If the user presses `ESC` inside the message view, control returns to the
    list at the same cursor position.
- A **status bar** is printed below the table on every redraw with minimal
  key binding hints and the current position (`[cursor+1 / total]`).

### Batch mode

Prints the current window and exits immediately.  If more messages remain
beyond the window a hint line is appended:

```
  -- <remaining> more message(s) --  use --offset <next> for next page
```

### Header cache eviction

Eviction is triggered only when `--all` is used and at least one UID was returned
by the server.  It removes `.hdr` files whose numeric basename (UID) is absent from
the current ALL set.

### Exit codes

| Code | Condition |
|------|-----------|
| 0 | Command completed (even if mailbox is empty) |
| −1 | IMAP SEARCH failed |

---

## `show`

```
email-cli [--batch] show <uid>
```

Displays the full content of one message identified by its IMAP UID.

### Arguments

| Argument | Description |
|----------|-------------|
| `<uid>` | Positive integer IMAP UID (as shown in `list` output) |

### Behaviour

1. Check the **local message store** (`~/.local/share/email-cli/accounts/imap.<host>/store/<folder>/<uid>.eml`).
2. **Local store hit**: load from disk.
3. **Miss**:
   a. Check whether the message currently has `\Seen` (via `UID FETCH uid (FLAGS)`).
   b. Fetch full message via `UID FETCH uid BODY[]` which sets `\Seen` on the server.
   c. If the message was **not** `\Seen` before step (b): immediately issue
      `UID STORE uid -FLAGS (\Seen)` to restore the unread state.
   d. Save fetched content to the local message store.
4. Parse MIME headers (`From`, `Subject`, `Date`) and extract plain-text body.
5. Print headers + separator + body.
6. Paginate the body according to [pagination.md](pagination.md).

### Read-status policy

**Viewing a message with `show` must never permanently alter its `\Seen` flag.**
The user's explicit action (e.g. a future `mark-read` command) is the only
legitimate way to change read status.  The fetch-and-restore in step 3 implements
this invariant.  If the message was already `\Seen` before the fetch, no STORE is
issued (no-op).

### Exit codes

| Code | Condition |
|------|-----------|
| 0 | Message displayed successfully |
| −1 | Fetch or cache load failed |

---

## `folders`

```
email-cli [--batch] folders [--tree]
```

Lists all IMAP folders available on the server.

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--tree` | off | Render the folder hierarchy as a Unicode box-drawing tree |

### Behaviour

1. Issue IMAP `LIST "" "*"` via `CURLOPT_CUSTOMREQUEST` against the root mailbox URL.
2. Parse each `* LIST (flags) sep mailbox` response line.
3. Decode IMAP Modified UTF-7 folder names (see [imap-protocol.md](imap-protocol.md)).
4. Sort folder names lexicographically.
5. Print flat list (one per line) or tree (see [output-formats.md](output-formats.md)).

### Exit codes

| Code | Condition |
|------|-----------|
| 0 | Folders listed (even if list is empty) |
| −1 | IMAP LIST failed |

---

## `attachments`

```
email-cli attachments <uid>
```

Lists all MIME attachments in the message identified by its UID.

### Arguments

| Argument | Description |
|----------|-------------|
| `<uid>` | Positive integer IMAP UID (as shown in `list` output) |

### Behaviour

1. Fetch or load the message from the local store (same resolution as `show`).
2. Parse the MIME structure to enumerate attachment parts
   (content-disposition `attachment` or non-text content types).
3. Print one line per attachment: filename and decoded size.

### Exit codes

| Code | Condition |
|------|-----------|
| 0 | Attachments listed (even if none found) |
| −1 | Fetch or parse failed |

---

## `save-attachment`

```
email-cli save-attachment <uid> <filename> [dir]
```

Saves a single attachment from a message to disk.

### Arguments

| Argument | Description |
|----------|-------------|
| `<uid>` | Positive integer IMAP UID |
| `<filename>` | Exact attachment filename (as shown by `attachments`) |
| `[dir]` | Destination directory (default: `~/Downloads` if it exists, otherwise `~`) |

### Behaviour

1. Fetch or load the message from the local store.
2. Locate the MIME part whose filename matches `<filename>` exactly.
3. Decode the attachment body (base64 or quoted-printable).
4. Write to `<dir>/<filename>`.  If the file already exists, it is overwritten.

### Exit codes

| Code | Condition |
|------|-----------|
| 0 | Attachment saved successfully |
| −1 | Message not found, no matching attachment, or write failed |

---

## `send`

```
email-cli send --to <addr> --subject <text> --body <text>
```

Sends a message non-interactively (scriptable / batch mode).

### Options

| Option | Description |
|--------|-------------|
| `--to <addr>` | Recipient email address (required) |
| `--subject <text>` | Subject line (required) |
| `--body <text>` | Message body text (required) |

All three options are mandatory.

### Behaviour

1. Validate that all three required options are present.
2. Determine the sender address: `cfg.smtp_user` if set, otherwise `cfg.user`.
3. Build a complete RFC 2822 message via `compose_build_message()` (adds Date,
   Message-ID, MIME-Version, Content-Type headers; converts LF to CRLF).
4. Send via `smtp_send()` using the account's SMTP configuration.
5. On success, save the sent message to the local Sent folder via
   `email_service_save_sent()`.

### Prerequisites

SMTP settings must be configured before using this command.  Run
`email-cli config smtp` to set up outgoing mail.

### Gmail accounts

For Gmail accounts (`GMAIL_MODE=1`), the send dispatch uses the Gmail REST
API (`gmail_send_message()`) instead of SMTP.  No SMTP configuration is
needed.

### Exit codes

| Code | Condition |
|------|-----------|
| 0 | Message sent (and optionally saved to Sent folder) |
| −1 | Missing options, build failure, or SMTP/API error |

---

## `config`

```
email-cli [--account <email>] config <subcommand>
```

View or update configuration settings.

### Global options

| Option | Description |
|--------|-------------|
| `--account <email>` | Select a specific account by email address.  Required when multiple accounts are configured. |

### Subcommands

#### `config show`

Prints the current configuration to stdout.  Passwords are masked as `****`.

Output format:

```
email-cli configuration (user@example.com):

  IMAP:
    Host:     imaps://imap.example.com
    User:     user@example.com
    Password: ****
    Folder:   INBOX

  SMTP:
    Host:     smtps://smtp.example.com
    Port:     587
    User:     user@example.com
    Password: ****
```

If SMTP is not configured:

```
  SMTP:
    (not configured — will be derived from IMAP host)
```

#### `config imap`

Runs the interactive IMAP setup wizard for the selected account.  On
success, the updated configuration is saved to
`~/.config/email-cli/accounts/<email>/config.ini`.

#### `config smtp`

Runs the interactive SMTP setup wizard for the selected account.  On
success, the SMTP fields are appended/updated in the account's config file.

### Multi-account dispatch

When multiple accounts are configured and `--account` is omitted, the
command lists the available accounts on stderr and exits with code 1:

```
Multiple accounts configured. Re-run with --account <email>:
  alice@example.com
  bob@work.example
```

### Exit codes

| Code | Condition |
|------|-----------|
| 0 | Subcommand completed successfully |
| 1 | Unknown subcommand, missing account, or wizard aborted |

---

> **Note:** The `sync` command and cron management have moved to the separate
> `email-sync` binary. See [email-sync.md](email-sync.md) for the specification.

---

## Binary-specific defaults

| Binary | `list` default | Interactive pager | Write operations |
|--------|---------------|-------------------|-----------------|
| `email-cli` | UNSEEN only | No (batch) | send, config imap/smtp |
| `email-cli-ro` | UNSEEN only | No (batch) | None |
| `email-tui` | UNSEEN only | Yes (TUI) | All (compose, reply, send, flag) |

---

## First-run setup wizard

When no configuration file exists at `~/.config/email-cli/config.ini`, the
**setup wizard** runs interactively before any command is dispatched.  It prompts
for IMAP host, username, password, and folder, then writes the config file with
mode `0600`.  If the user aborts (EOF / empty host), the program exits with code 1.

---

## Exit codes summary

| Code | Meaning |
|------|---------|
| 0 (`EXIT_SUCCESS`) | Command completed successfully |
| 1 (`EXIT_FAILURE`) | Fatal error, bad argument, or command failed |

The `main` function maps the internal `0` / `−1` return to `EXIT_SUCCESS` /
`EXIT_FAILURE` and prints a trailing `"\nSuccess: Fetch complete.\n"` on success
or `"\nFailed. Check logs in <path>\n"` on failure.
