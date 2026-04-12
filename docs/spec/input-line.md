# InputLine & PathComplete Specification

## Overview

`InputLine` is a reusable single-line interactive text-editing widget for
terminal TUI screens.  `PathComplete` is a filesystem path Tab-completion
module that attaches to an `InputLine`.

Both live in `src/core/` and have no dependency on domain or infrastructure
code.

---

## InputLine (`src/core/input_line.{h,c}`)

### Data structure

```c
struct InputLine {
    char  *buf;                              /* caller-owned buffer          */
    size_t bufsz;                            /* capacity incl. NUL           */
    size_t len;                              /* current length in bytes      */
    size_t cur;                              /* cursor byte offset [0..len]  */
    int    trow;                             /* terminal row (set by run())  */
    void (*render_below)(const InputLine *); /* hook: draw below input row   */
    void (*tab_fn)(InputLine *);             /* Tab key callback             */
    void (*shift_tab_fn)(InputLine *);       /* Shift+Tab callback           */
};
```

All callback fields are `NULL` after `input_line_init()`; callers set them
before calling `input_line_run()`.

### `input_line_init(il, buf, bufsz, initial_text)`

- Sets `il->buf = buf`, `il->bufsz = bufsz`.
- Copies `initial_text` into `buf` (NUL-terminated); truncates to `bufsz-1`
  bytes if necessary.
- Sets `il->len` to the byte length of the stored string.
- Sets `il->cur = il->len` (cursor starts at the end of the text).
- Sets `il->trow = 0` and all callbacks to `NULL`.

### `input_line_run(il, trow, prompt)` → int

Enters an interactive edit loop on terminal row `trow` (1-based).

**Rendering** (each iteration):
1. Move to `(trow, 1)`, clear the line, print `"  " + prompt + buf`.
2. Call `il->render_below(il)` if set (may draw on rows below `trow`).
3. Move the real terminal cursor to `(trow, col)` where  
   `col = 3 + display_cols(prompt) + display_cols(buf[0..cur])`.  
   Show the cursor (`\033[?25h`).

**Key bindings**:

| Key          | Action                                              |
|--------------|-----------------------------------------------------|
| `Enter`      | Hide cursor, clear `trow+1`, return **1**           |
| `ESC`        | Hide cursor, clear `trow+1`, return **0**           |
| `Ctrl-C`     | Same as ESC → return **0**                          |
| `←`          | Move cursor one UTF-8 code point left               |
| `→`          | Move cursor one UTF-8 code point right              |
| `Home`       | Move cursor to byte offset 0                        |
| `End`        | Move cursor to `len`                                |
| `Backspace`  | Delete the code point immediately before the cursor |
| `Delete`     | Delete the code point at the cursor                 |
| `Tab`        | Call `il->tab_fn(il)` if non-NULL                   |
| `Shift+Tab`  | Call `il->shift_tab_fn(il)` if non-NULL             |
| Printable    | Insert the ASCII character at the cursor            |
| Everything else | Silently ignored                                 |

**UTF-8 handling**:
- Cursor movement skips over continuation bytes (`0x80`–`0xBF`) so `cur`
  always points to the start of a code point.
- Display column widths are computed with `terminal_wcwidth()`.
- Only printable ASCII (32–126) is inserted by this widget; multi-byte
  input is not handled (the caller may extend via `tab_fn` or direct buffer
  manipulation).

**Return value**: 1 = confirmed (Enter), 0 = cancelled (ESC / Ctrl-C).

---

## PathComplete (`src/core/path_complete.{h,c}`)

### Public API

```c
void path_complete_attach(InputLine *il);
void path_complete_reset(void);
```

#### `path_complete_attach(il)`

Sets `il->tab_fn`, `il->shift_tab_fn`, and `il->render_below` to the
internal path-completion implementations.  Must be called after
`input_line_init()` and before `input_line_run()`.

#### `path_complete_reset()`

Releases all memory held by the completion state.  Must be called after
`input_line_run()` returns.

### Completion state

One global instance of completion state is maintained.  It holds:

| Field        | Description                                      |
|--------------|--------------------------------------------------|
| `names[]`    | Sorted array of matching entry names (bare, no trailing `/`) |
| `count`      | Number of entries in `names`                     |
| `idx`        | Index of the currently selected entry            |
| `view_start` | First entry visible in the completion bar        |
| `dir`        | Directory part of the last scan path             |
| `expected`   | `buf[0..cur]` as it was when this list was built |
| `suffix`     | `buf[cur..len]` captured at scan time            |

### Tab behaviour

On each Tab press:

1. Extract the **head** = `il->buf[0..il->cur]`.
2. If an active list exists (`count > 0`) and `head == expected`:  
   advance `idx = (idx + 1) % count` → **cycle forward**.
3. Otherwise: **fresh scan**:
   a. Store `suffix = il->buf[il->cur..len]`.
   b. Split `head` at the last `/`:
      - `dir`    = everything up to and including the last `/` (or `"./"` if no `/`).
      - `prefix` = everything after the last `/`.
   c. Open `dir` with `opendir()`.  Skip `.` and `..`.
   d. Skip hidden entries (names starting with `.`) unless `prefix` itself
      starts with `.`.
   e. Include only entries whose names start with `prefix`.
   f. Sort all matches alphabetically (`strcmp`).
   g. Set `idx = 0`, `view_start = 0`.
4. Apply selected entry:  
   `buf = dir + names[idx] + suffix`; `cur = strlen(dir + names[idx])`.
5. Update `expected = buf[0..cur]`.

If no matches are found the buffer and cursor are left unchanged.

### Shift+Tab behaviour

1. Extract `head = il->buf[0..il->cur]`.
2. If `count == 0` or `head != expected`: **no-op** (list not active or
   buffer was edited since last Tab).
3. Otherwise: `idx = (idx + count - 1) % count` → **cycle backward**.
4. Apply selected entry (same as step 4 above).

### Entering a subdirectory

After a Tab completion the user can append `/` to the accepted directory
name.  On the next Tab:

- `head` now ends with `/`, so it does not match `expected` (which ended
  without `/`).
- A fresh scan is triggered using the new `head`, effectively entering
  the subdirectory.

### Suffix preservation

Text that lies **after the cursor** at the moment Tab is first pressed is
captured as `suffix`.  Every subsequent apply step appends `suffix` to the
buffer after the completed name.  The cursor always lands at the end of the
completed prefix, before `suffix`.

Example:

```
buf before Tab:  /home/user/|file.pdf      (cursor at |)
dir:             /home/user/
prefix:          (empty)
suffix captured: file.pdf

After Tab (first match "docs"):
buf:             /home/user/docs|file.pdf
cursor at:       end of "docs"
```

### Completion bar rendering

When `count > 0`, `render_below` draws a single line at `trow + 1`:

- Two-space indent.
- If `view_start > 0`: dim `<·` prefix to indicate hidden entries to the left.
- Entry names separated by two spaces; current selection in reverse video.
- If entries exceed the terminal width: `...` suffix, and `view_start` is
  advanced to keep the selected entry visible.
- If `count == 0`: the row is not touched (the status bar remains intact).

### Hidden files

- With an empty prefix (cursor just after `/`): entries whose names begin
  with `.` are excluded.
- With a prefix starting with `.`: all matching dot-files are included.

---

## Testing

Unit tests are in `tests/unit/test_input_line.c` and
`tests/unit/test_path_complete.c`.

| Test | Coverage |
|------|----------|
| `test_input_line` | `input_line_init`: buffer, length, cursor, truncation, callbacks, UTF-8 content |
| `test_attach_sets_callbacks` | All three callbacks non-NULL after attach |
| `test_reset_idempotent` | Double reset does not crash |
| `test_no_match_leaves_buf_unchanged` | Empty dir / non-matching prefix |
| `test_single_match_completes_fully` | One match: full completion, cursor at end |
| `test_cycles_forward_through_matches` | 3 matches: Tab cycles 0→1→2→0 |
| `test_cycles_backward_with_shift_tab` | Shift+Tab cycles 0→2→1→0 |
| `test_shift_tab_without_active_list_is_noop` | Shift+Tab before Tab: no-op |
| `test_suffix_preserved_when_cursor_in_middle` | Text after cursor survives |
| `test_edit_after_cycle_triggers_fresh_scan` | Manual edit resets cycling |
| `test_typing_slash_enters_subdirectory` | `/` after completion → enters dir |
| `test_results_are_sorted_alphabetically` | Matches in strict `strcmp` order |
| `test_hidden_files_excluded_with_empty_prefix` | Dot-files hidden by default |
| `test_hidden_files_included_with_dot_prefix` | Dot-prefix reveals dot-files |

Full interactive behaviour (key dispatch, rendering, cursor positioning) is
covered by PTY tests in `tests/pty/`.
