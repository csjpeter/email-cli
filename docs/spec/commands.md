# Command Specification

## Global syntax

```
email-cli [--batch] <command> [command-options]
```

`--batch` is a **global flag** and may appear anywhere in the argument list, before
or after the command name.  It is detected by a pre-scan of all arguments before
dispatching to the command handler.

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

Supported topics: `list`, `show`, `folders`.

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
   (see [caching.md](caching.md)), parse `From`, `Subject`, `Date`; render table row.
7. When `--all` and the full UID set was fetched, call `hcache_evict_stale` to
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

1. Check the **full-message cache** (`~/.cache/email-cli/messages/<folder>/<uid>.eml`).
2. **Cache hit**: load from disk.
3. **Cache miss**:
   a. Check whether the message currently has `\Seen` (via `UID FETCH uid (FLAGS)`).
   b. Fetch full message via IMAP URL (`/<folder>/;UID=<uid>`); libcurl issues
      `UID FETCH uid BODY[]` which sets `\Seen` on the server.
   c. If the message was **not** `\Seen` before step (b): immediately issue
      `UID STORE uid -FLAGS (\Seen)` to restore the unread state.
   d. Save fetched content to the full-message cache.
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
