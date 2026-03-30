# Output Format Specification

All normal output goes to **stdout**.  Error messages and the interactive pager
prompt go to **stderr**.  The trailing success/failure line is written by `main`
after the command returns.

---

## `list` — unread-only mode (default)

```
--- Fetching emails: <user> @ <host>/<folder> ---
<first>-<last> of <total> unread message(s) in <folder>.

  <UID-col>  <From-col>                      <Subject-col>                   <Date-col>
  ═════  ══════════════════════════════  ══════════════════════════════  ═══════════════════════════
  <uid>  <from>                          <subject>                       <date>   ← normal row
▶ <uid>  <from>                          <subject>                       <date>   ← selected row (reverse video + \033[K\033[0m)
  ...

  ↑↓=step  PgDn/PgUp=page  Enter=open  q=quit  [<cursor+1>/<total>]   ← status bar (dim)

Success: Fetch complete.
```

The selected row is printed with `\033[7m` (reverse video) before the row
content and `\033[K\033[0m` after; this highlights the entire line to the
terminal edge.  The status bar is printed in dim style (`\033[2m … \033[0m`).

When the mailbox has no unread messages:
```
No unread messages in <folder>.
```

### Column widths

Column widths adapt to the current terminal width (measured via `ioctl TIOCGWINSZ`,
falling back to 80).  Fixed overhead per row is subtracted first; the remaining
space is split evenly between **From** and **Subject**.

| Column | Width | Notes |
|--------|-------|-------|
| UID | 5 (right-aligned) | fixed |
| From | `avail / 2` terminal cols | truncated at character boundary; min 20 |
| Subject | `avail - from_w` terminal cols | truncated at character boundary; min 20 |
| Date | 16 (fixed, `YYYY-MM-DD HH:MM`) | truncated at 16 bytes |

**Overhead** (subtracted from terminal width before splitting):

| Mode | Overhead |
|------|----------|
| unread-only | 29 cols (`"  UID  "` + `"  "` + `"  DATE"`) |
| `--all` | 32 cols (`"  S  UID  "` + `"  "` + `"  DATE"`) |

Column widths are recomputed on each screen redraw so the layout adjusts
automatically after terminal resize.

### Date format

Dates are parsed from RFC 2822 format and displayed as **`YYYY-MM-DD HH:MM`**
in the **local system timezone**.  Examples:

| Raw server value | Displayed (UTC+2 local) |
|------------------|------------------------|
| `Tue, 10 Mar 2026 15:07:40 +0000 (UTC)` | `2026-03-10 17:07` |
| `Thu, 26 Mar 2026 12:00:00 +0000` | `2026-03-26 14:00` |

If the date cannot be parsed, the raw string is shown as-is.

---

## `list --all`

```
--- Fetching emails: <user> @ <host>/<folder> ---
<first>-<last> of <total> message(s) in <folder> (<unread> unread).

  S  <UID-col>  <From-col>                      <Subject-col>                   <Date-col>
  ═  ═════  ══════════════════════════════  ══════════════════════════════  ═══════════════════════════
  N  <uid>  <from>                          <subject>                       <date>
     <uid>  <from>                          <subject>                       <date>
  ...

  ↑↓=step  PgDn/PgUp=page  Enter=open  q=quit  [<cursor+1>/<total>]
```

The `S` (status) column contains `N` for unread messages and a space for read ones.
Unread messages always appear before read ones regardless of UID order.
In interactive mode the currently selected row is highlighted (see `list` above).

---

## `show`

```
From:    <from>
Subject: <subject>
Date:    <date>
─────────────────────────────────────────────────────────────────
<body text>

Success: Fetch complete.
```

- The separator line is exactly 65 `─` (U+2500) characters.
- `Date` is shown in `YYYY-MM-DD HH:MM` format (local timezone), same as `list`.
- If a header field is absent or unparseable, `(none)` is printed.
- The body is selected and rendered in the following priority order (see
  [html-rendering.md](html-rendering.md) for full details):
  1. **`text/html`** part — rendered to terminal text via `html_render()`,
     with ANSI colour/style escapes when the pager is active.
  2. **`text/plain`** part — extracted as-is and word-wrapped.
  3. **Neither present** — `(no readable text body)` is printed.
- Long lines are **soft-wrapped** at word boundaries so that no output line
  exceeds `min(terminal_width, 80)` terminal columns.  Words that individually
  exceed the limit are emitted on their own line without breaking.
- When the interactive pager is active, each page begins with a full-screen
  clear (`\033[H\033[2J`) followed by the headers + separator, then the body
  page.  A pager status bar is printed to **stderr** (reverse video):

  ```
  -- [<cur>/<total>] PgDn/↓=scroll  PgUp/↑=back  ESC=list  q=quit --
  ```

  After a key is pressed, the status bar is erased with `\r\033[K`.

### `show` when opened from `list` (interactive context)

When `show_uid_interactive()` is called from the `list` TUI (Enter key), the
behaviour is identical to standalone `show` with the following differences:

| Key | Effect |
|-----|--------|
| ESC | Return to `list` at the same cursor position (return value 0) |
| Ctrl-C | Exit the entire program (return value 1) |
| PgDn / Down / Enter | Scroll forward; stays at last page when the end is reached |

---

## `folders` — flat mode

One folder name per line, lexicographically sorted, IMAP Modified UTF-7 decoded:

```
INBOX
INBOX.Sent
INBOX.Trash
```

---

## `folders --tree`

Unicode box-drawing tree, showing only the last path component at each node.
The hierarchy separator is taken from the IMAP LIST response.

```
└── INBOX
    ├── Sent
    └── Trash
```

Box-drawing characters used:

| Purpose | Character | Unicode |
|---------|-----------|---------|
| Vertical continuation | `│` | U+2502 |
| Non-last child | `├──` | U+251C U+2500 U+2500 |
| Last child | `└──` | U+2514 U+2500 U+2500 |

---

## Pager prompt (stderr) — standalone `show`

```
-- [<current>/<total>] PgDn/↓=scroll  PgUp/↑=back  q=quit --
```

Displayed in **reverse video** (`\033[7m` … `\033[0m`).  Erased with `\r\033[K`
after a key is pressed.

## Status bar — interactive `list`

Printed to **stdout** below the table (dim style `\033[2m … \033[0m`):

```
  ↑↓=step  PgDn/PgUp=page  Enter=open  ESC=quit  [<cursor+1>/<total>]
```

## Status bar — interactive `show` (opened from list)

Printed to **stderr** in reverse video:

```
-- [<cur>/<total>] PgDn/↓=scroll  PgUp/↑=back  ESC=list --
```

### Key bindings — all interactive views

| Key | `list` | `show` (standalone) | `show` (from list) |
|-----|--------|---------------------|--------------------|
| Space / PgDn | Next page | Next page | Next page |
| PgUp | Prev page | Prev page | Prev page |
| ↑ (Up-arrow) | Cursor up one row | Scroll up one line | Scroll up one line |
| ↓ (Down-arrow) | Cursor down one row | Scroll down one line | Scroll down one line |
| Enter | Open message | Next page | Next page (stays at last page) |
| ESC | Quit list | Quit | Return to list |
| Ctrl-C | Quit entirely | Quit | Quit entirely |
| Left / Right arrows | No-op (ignored) | No-op | No-op |

Multi-byte escape sequences (PgDn = `ESC[6~`, PgUp = `ESC[5~`, arrows = `ESC[A/B`)
are fully consumed before returning so no stray bytes remain in the input buffer.

---

## Trailing status lines (stdout)

Written by `main` after every command:

| Outcome | Output |
|---------|--------|
| Success | `\nSuccess: Fetch complete.\n` |
| Failure | `\nFailed. Check logs in <log-path>\n` (to stderr) |
