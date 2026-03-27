# Output Format Specification

All normal output goes to **stdout**.  Error messages and the interactive pager
prompt go to **stderr**.  The trailing success/failure line is written by `main`
after the command returns.

---

## `list` — unread-only mode (default)

```
--- Fetching emails: <user> @ <host>/<folder> ---
<count> unread message(s) in <folder>.

  <UID-col>  <From-col>                      <Subject-col>                   <Date-col>
  ═════  ══════════════════════════════  ══════════════════════════════  ═══════════════════════════
  <uid>  <from>                          <subject>                       <date>
  ...

Success: Fetch complete.
```

When `--offset` or `--limit` restricts the view to a subset:
```
<first>-<last> of <total> unread message(s) in <folder>.
```

When the mailbox has no unread messages:
```
No unread messages in <folder>.
```

### Column widths

| Column | Width | Truncation |
|--------|-------|------------|
| UID | 5 (right-aligned) | — |
| From | 30 (left-aligned) | truncated at 30 chars |
| Subject | 30 (left-aligned) | truncated at 30 chars |
| Date | variable (no truncation) | — |

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
<count> message(s) in <folder> (<unread> unread).

  S  <UID-col>  <From-col>                      <Subject-col>                   <Date-col>
  ═  ═════  ══════════════════════════════  ══════════════════════════════  ═══════════════════════════
  N  <uid>  <from>                          <subject>                       <date>
     <uid>  <from>                          <subject>                       <date>
  ...
```

The `S` (status) column contains `N` for unread messages and a space for read ones.
Unread messages always appear before read ones regardless of UID order.

---

## `show`

```
From:    <from>
Subject: <subject>
Date:    <date>
────────────────────────────────────────────────────────────────
<body text>

Success: Fetch complete.
```

- The separator line is exactly 65 `─` (U+2500) characters.
- `Date` is shown in `YYYY-MM-DD HH:MM` format (local timezone), same as `list`.
- If a header field is absent or unparseable, `(none)` is printed.
- The body is the plain-text part extracted from the MIME structure.  If no
  plain-text part exists, `(no readable text body)` is printed.
- When the interactive pager is active, headers and separator are reprinted at the
  top of each page (page 2 onward is preceded by an ANSI clear-screen sequence).

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

## Pager prompt (stderr)

```
-- [<current>/<total>] Space/Enter=next  p=prev  q=quit --
```

Displayed in **reverse video** (`\033[7m` … `\033[0m`).  Rendered with `\r\033[K`
erasure after a key is pressed.

### Pager key bindings

| Key | Action |
|-----|--------|
| PgDn, Down-arrow, Space, Enter, any other key | Next page |
| PgUp, Up-arrow, `p`, `P`, `b` | Previous page |
| `q`, `Q`, ESC, Ctrl-C (code 3) | Quit pager |

Multi-byte escape sequences (PgDn = `ESC[6~`, PgUp = `ESC[5~`, arrows = `ESC[A/B`)
are fully consumed before returning so no stray bytes remain in the input buffer.

---

## Trailing status lines (stdout)

Written by `main` after every command:

| Outcome | Output |
|---------|--------|
| Success | `\nSuccess: Fetch complete.\n` |
| Failure | `\nFailed. Check logs in <log-path>\n` (to stderr) |
