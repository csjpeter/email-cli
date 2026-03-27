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

- `--offset N` sets the 0-based start index to `N − 1` (1-based input).
  If offset ≥ total message count, a "No messages at offset N" message is printed
  and the command returns 0.
- `--limit N` sets the page size.  Default: `page_size` (see above).

### Batch mode

Prints the current page and exits.  If more messages remain beyond the window, a
hint line is printed:

```
  -- <remaining> more message(s) --  use --offset <next> for next page
```

### Interactive pager mode

After the last row of each page (when more pages remain), the pager prompt is
displayed on **stderr** and the program waits for a keypress.  On the next page,
the screen is cleared with `\033[H\033[2J` before printing the table header and
rows again.

Pager navigation:

| Key | Effect |
|-----|--------|
| PgDn, Down-arrow, Space, Enter, or any other key | Advance to next page |
| PgUp, Up-arrow, `p`, `P`, `b` | Go back one page |
| `q`, `Q`, ESC, Ctrl-C | Exit pager immediately |

Going back on the first page is a no-op (cursor stays at page 1).

### Page count display

The pager prompt shows `[current/total]` where:

```
total_pages = ceil(show_count / limit)
cur_page    = cur_index / limit + 1
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
without pagination.

### First page

Headers and separator are printed once before the pager loop.  The first page of
body text is printed; if this is also the last page, the loop exits immediately
without showing a prompt.

### Subsequent pages

The screen is cleared (`\033[H\033[2J`) and headers + separator are reprinted at
the top of each subsequent page, followed by the next `rows_avail` lines of body.

### Navigation

Same key bindings as `list`.  Going back on page 1 is a no-op.

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
