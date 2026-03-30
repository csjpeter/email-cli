# Pagination Specification

Both `list` and `show` support paginated output.  The behaviour depends on whether
the program is running in **batch mode** or **interactive mode**.

---

## Mode detection

```
batch_mode = (--batch flag present) OR (isatty(STDOUT_FILENO) == 0)
pager      = !batch_mode
page_size  = detect_page_size(batch_mode)
```

`detect_page_size`:

1. If batch mode: return `BATCH_DEFAULT_LIMIT` (100).
2. Query terminal size via `ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)`.
3. If successful and `ws.ws_row > LIST_HEADER_LINES + 2`: return
   `ws.ws_row − LIST_HEADER_LINES` where `LIST_HEADER_LINES = 6`.
4. Fallback: return 20.

The computed `page_size` is used as the default `--limit` when the user does not
supply `--limit` explicitly.

---

## `list` pagination

### Offset and limit

- `--offset N` sets the initial cursor position to `N − 1` (1-based input,
  converted to 0-based internally).
- `--limit N` sets the window height (visible rows).  Default: `page_size`.

### Batch mode

Prints the current window (rows `[offset, offset+limit)`) and exits.  If more
messages remain beyond the window, a hint line is printed:

```
  -- <remaining> more message(s) --  use --offset <next> for next page
```

### Interactive cursor mode (pager enabled)

`list` in interactive mode uses a **cursor-based TUI**:

- `cursor` — 0-based index of the highlighted entry (initially from `--offset`).
- `wstart` — 0-based index of the topmost visible row.

**Window tracking:** before each redraw, `wstart` is adjusted so that `cursor`
is always within `[wstart, wstart + limit)`:

```
if cursor < wstart:          wstart = cursor
if cursor >= wstart + limit: wstart = cursor - limit + 1
```

**Never auto-exits:** the loop runs until the user explicitly presses `q`, `Q`,
Ctrl-C, or ESC.  Reaching the first or last row with cursor movement is a no-op
(cursor clamps).

**On each iteration:**

1. Clear screen (`\033[H\033[2J`).
2. Print count/status line and column header.
3. Print rows `[wstart, wend)`.  The row at `cursor` is highlighted.
4. Print status bar below the table.
5. Wait for a keypress; update `cursor` accordingly or open the selected
   message.

**Cursor movement:**

| Key | Effect |
|-----|--------|
| ↓ / `j` | `cursor++` (clamp at `show_count − 1`) |
| ↑ / `k` | `cursor--` (clamp at 0) |
| PgDn / Space | `cursor += limit` (clamp at `show_count − 1`) |
| PgUp / `p` / `P` / `b` | `cursor -= limit` (clamp at 0) |
| Enter | Open `show_uid_interactive(uid[cursor])` |
| `q` / `Q` / Ctrl-C / ESC | Exit list |

### Status bar

Printed after the table on each redraw (dim style):

```
  ↑↓=step  PgDn/PgUp=page  Enter=open  q=quit  [<cursor+1>/<total>]
```

---

## `show` pagination

### Body line counting

`count_lines(body_text)` counts newline characters; the result is the number of
body lines.

### Available rows per page

```
rows_avail = page_size − SHOW_HDR_LINES
```

`SHOW_HDR_LINES = 5` (From, Subject, Date, separator line, one blank).

If `!pager || page_size <= SHOW_HDR_LINES`, the entire body is printed at once
without pagination (standalone `show` only).

### Each page (both standalone and interactive context)

1. Clear screen (`\033[H\033[2J`).
2. Print headers + separator.
3. Print `rows_avail` lines of body starting at `cur_line`.
4. Print pager status bar to **stderr**.
5. Wait for keypress; update `cur_line`.

### Navigation

| Key | Effect |
|-----|--------|
| PgDn / Down / `j` / Space / Enter | `cur_line += rows_avail` |
| PgUp / Up / `k` / `p` / `P` / `b` | `cur_line -= rows_avail` (clamp at 0) |
| ↓ / `j` (line-by-line) | `cur_line += 1` |
| ↑ / `k` (line-by-line) | `cur_line -= 1` (clamp at 0) |
| ESC | Standalone: quit; from list: return to list |
| `q` / `Q` / Ctrl-C | Quit entirely |

Reaching the last line with PgDn/Enter:
- Standalone `show`: loop exits.
- From list (`show_uid_interactive`): returns 0 (back to list).

Going back on page 1 is a no-op (`cur_line` stays at 0).

---

## ANSI State Replay at Page Boundaries

When the pager displays page *N* > 1, it skips the first `(N-1) × rows_avail`
body lines.  Those skipped lines may contain open ANSI SGR escapes (e.g. a
heading or coloured span that starts on line 0 but closes many lines later).
Without correction, the visible text on page 2+ would inherit whatever
SGR state was left open — producing unreadable output such as white-on-white
when the email's colour and the user's terminal theme conflict.

**Fix:** before printing the first visible line of any page with `from_line > 0`,
`print_body_page()`:

1. Scans all skipped bytes (`body[0 .. first_visible_byte)`) with `ansi_scan()`
   to accumulate the SGR state that would be in effect at `from_line`.
2. Calls `ansi_replay()` to re-emit the minimal set of escapes needed to restore
   that state.

`ansi_scan()` tracks: **bold**, **italic**, **underline**, **strikethrough**,
**fg colour** (24-bit RGB), **bg colour** (24-bit RGB).  It honours `\033[0m`
(full reset) anywhere in the skipped content.

`ansi_replay()` emits (only for attributes that are active in the accumulated
state):

| Attribute | Escape |
|-----------|--------|
| bold | `\033[1m` |
| italic | `\033[3m` |
| underline | `\033[4m` |
| strikethrough | `\033[9m` |
| fg colour | `\033[38;2;R;G;Bm` |
| bg colour | `\033[48;2;R;G;Bm` |

### ANSI Reset at Page Start

Each page redraw begins with `\033[0m\033[H\033[2J` (reset all attributes +
home cursor + clear screen).  After `print_body_page()` returns, `\033[0m` is
emitted again to close any ANSI state left open by the page body (the body
terminates at an arbitrary line boundary, which may be inside a styled span
that `ansi_replay` re-opened).

The status bar is printed to **stderr** after the body; this ordering
(stdout body + `\033[0m`, then stderr status) ensures the terminal attribute
state is clean when the status bar's reverse-video (`\033[7m`) is activated.

---

## Raw terminal mode

The pager prompt reads a single keypress without requiring Enter.  This is
implemented by switching stdin to raw mode:

1. Save current `termios` with `tcgetattr(STDIN_FILENO, &old)`.
2. Set `raw = old`; clear `ICANON | ECHO | ISIG`; set `VMIN=1`, `VTIME=0`.
3. Apply with `tcsetattr(STDIN_FILENO, TCSANOW, &raw)`.
4. Read one character with `getchar()`.
5. Restore original settings with `tcsetattr(STDIN_FILENO, TCSANOW, &old)`.

If `tcgetattr` fails (e.g. stdin is not a terminal), `−1` is returned; the caller
treats this as "quit".
