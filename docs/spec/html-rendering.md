# HTML Rendering Specification

This document specifies how `email-cli` renders HTML email bodies to terminal
text, including MIME part selection, the HTML parser, the renderer, ANSI colour
policy, and pager integration.

---

## MIME Part Selection

When displaying a message body (`show` command, interactive or batch), the
program selects content in the following priority order:

1. **`text/html`** — extracted via `mime_get_html_part()`, rendered through the
   HTML renderer.
2. **`text/plain`** — extracted via `mime_get_text_body()`, word-wrapped and
   printed as-is.
3. **Neither found** — the literal string `(no readable text body)` is printed.

`multipart/alternative` messages contain both parts; the HTML part is preferred.

---

## HTML Parser

Source: `src/core/html_parser.c`.  API: `html_parse(html)` → `HtmlNode *` tree.

### Tokenizer

The tokenizer is a state machine with the following states:

| State | Description |
|-------|-------------|
| `TEXT` | Outside any tag — accumulating text content |
| `TAG_OPEN` | Seen `<`, deciding element vs comment vs doctype |
| `TAG_NAME` | Reading the tag name |
| `ATTR_NAME` | Reading an attribute name |
| `ATTR_EQ` | Seen `=`, waiting for value |
| `ATTR_VALUE_Q` | Reading a quoted attribute value |
| `ATTR_VALUE_UQ` | Reading an unquoted attribute value |
| `COMMENT` | Inside `<!-- … -->` — content discarded |
| `DOCTYPE` | Inside `<!DOCTYPE …>` — content discarded |
| `SCRIPT` | Inside `<script>…</script>` — content discarded |
| `STYLE` | Inside `<style>…</style>` — content discarded |

Tag names are normalised to lowercase.

### Void Elements

The following elements are treated as self-closing (no closing tag expected):
`br`, `hr`, `img`, `input`, `link`, `meta`, `area`, `base`, `col`, `embed`,
`param`, `source`, `track`, `wbr`.

### Error Recovery

- Unmatched closing tags are silently discarded.
- Implicitly-closed elements (`<li>`, `<p>`, `<dt>`, `<dd>`, `<tr>`, `<td>`,
  `<th>`): if a same-level predecessor is still open, it is implicitly closed
  before the new element is opened.

### Entity Decoding

Performed in `html_decode_entities()` on all text content and attribute values.

**Named entities:**

| Entity | Codepoint | UTF-8 result |
|--------|-----------|--------------|
| `&amp;` | U+0026 | `&` |
| `&lt;` | U+003C | `<` |
| `&gt;` | U+003E | `>` |
| `&quot;` | U+0022 | `"` |
| `&apos;` | U+0027 | `'` |
| `&nbsp;` | U+00A0 | `\xC2\xA0` |
| `&zwnj;` | U+200C | `\xE2\x80\x8C` |
| `&zwj;` | U+200D | `\xE2\x80\x8D` |
| `&shy;` | U+00AD | `\xC2\xAD` |
| `&copy;` | U+00A9 | `©` |
| `&reg;` | U+00AE | `®` |
| `&mdash;` | U+2014 | `—` |
| `&ndash;` | U+2013 | `–` |
| `&hellip;` | U+2026 | `…` |
| `&bull;` | U+2022 | `•` |
| `&trade;` | U+2122 | `™` |
| `&euro;` | U+20AC | `€` |
| `&laquo;` | U+00AB | `«` |
| `&raquo;` | U+00BB | `»` |
| `&pound;` | U+00A3 | `£` |
| `&cent;` | U+00A2 | `¢` |
| `&yen;` | U+00A5 | `¥` |
| `&times;` | U+00D7 | `×` |
| `&divide;` | U+00F7 | `÷` |
| `&middot;` | U+00B7 | `·` |

**Numeric entities:** `&#N;` (decimal) and `&#xHH;` (hex) are decoded to UTF-8.

Unknown entities are passed through unchanged.

---

## HTML Renderer

Source: `src/core/html_render.c`.  API:

```c
char *html_render(const char *html, int width, int ansi);
```

- `html` — UTF-8 HTML input string.
- `width` — wrap column; `0` = no wrapping.
- `ansi` — `1` emit ANSI SGR escapes; `0` plain text only.
- Returns a heap-allocated string; caller must `free()`.  Returns `NULL` on OOM.

### Block Elements

Block elements emit at most one blank line of separation (`\n\n`) at their
boundaries.  The `compact_lines()` post-processor ensures no more than one
consecutive blank line appears anywhere in the output.

| Element | Block behaviour |
|---------|----------------|
| `<p>`, `<div>`, `<h1>`–`<h6>` | blank line before and after |
| `<br>` | single newline |
| `<hr>` | line of `—` characters |
| `<blockquote>` | each line prefixed with `> ` |
| `<ul>`, `<ol>` | nested; each `<li>` on its own line |
| `<table>`, `<tr>` | blank line separation |
| `<td>`, `<th>` | tab separator between cells |
| `<pre>`, `<code>` | literal whitespace preserved; word-wrap disabled |

### Bullet and Numbered Lists

- `<ul>` items are prefixed with `•` (U+2022).
- `<ol>` items are prefixed with `1.`, `2.`, … (per nesting level).
- Nested lists are indented by two spaces per level.

### Images

`<img alt="…">` renders as `[alt-text]`.  If the `alt` attribute is absent,
empty, or contains only invisible characters (space, U+00A0 NBSP, U+200C ZWNJ,
U+200D ZWJ, U+00AD SHY), no output is produced.

### Links

`<a href="…">text</a>` renders as `text` only.  The URL is not shown.

### Suppressed Elements

`<script>` and `<style>` and all their descendant text content produce no
output.

### Word-wrap

When `width > 0` and `pre` mode is not active, words are wrapped at `width`
visible columns.  ANSI escape sequences are not counted in the visible column
width.  Multi-byte UTF-8 codepoints are measured via `html_medium_char_width()`
(platform-specific: `wcwidth()` on POSIX).

Words wider than `width` are emitted on their own line without breaking.

### Blank Line Compaction

`compact_lines()` is applied to the renderer output:

1. Runs of more than one consecutive blank line are reduced to exactly one
   blank line.
2. Trailing ASCII whitespace and invisible Unicode characters (`\xC2\xA0`
   NBSP, `\xE2\x80\x8C` ZWNJ, `\xE2\x80\x8D` ZWJ, `\xC2\xAD` SHY) are
   stripped from each line's right edge.

---

## ANSI Style Output

When `ansi=1`, the following SGR escape pairs are emitted:

### Semantic Tags

| Tag | On | Off |
|-----|----|-----|
| `<b>`, `<strong>` | `\033[1m` | `\033[22m` |
| `<i>`, `<em>` | `\033[3m` | `\033[23m` |
| `<u>` | `\033[4m` | `\033[24m` |
| `<s>`, `<del>` | `\033[9m` | `\033[29m` |
| `<h1>`–`<h6>` | `\033[1m` (bold) | `\033[22m` |

### Inline `style=` Attributes

| CSS property | Effect |
|---|---|
| `font-weight: bold` | bold on/off |
| `font-style: italic` | italic on/off |
| `text-decoration: underline` | underline on/off |
| `color: X` (bright only — see below) | `\033[38;2;R;G;Bm` / `\033[39m` |
| `background-color: X` | **suppressed** — see colour policy |

### Style Balancing

Every ANSI style opened is guaranteed to be closed before the renderer returns:

- `traverse()` uses a snapshot/restore pattern: before visiting a node's
  children, the current depth counters for all styles are saved; after
  `tag_close()`, any counters that increased are decremented by emitting the
  corresponding off-escapes.
- This means closing any parent element automatically closes all styles opened
  by its children, regardless of whether the source HTML contained the
  corresponding closing tags.

---

## Colour Policy

### Background Colours — Always Suppressed

`background-color` CSS property values are **never emitted** as ANSI SGR
sequences.  Background colours set by email authors break dark-theme terminals
and produce unreadable combinations when the terminal's own background differs
from the author's intended canvas.

### Foreground Colours — Brightness Threshold

`color` CSS property values are emitted **only** if the parsed colour is bright
enough to remain readable on a dark terminal background:

```
max(R, G, B) >= 160   →  emitted as \033[38;2;R;G;Bm
max(R, G, B) <  160   →  suppressed (no escape emitted)
```

Examples:

| CSS value | R,G,B | max | Result |
|-----------|-------|-----|--------|
| `white` / `#FFFFFF` | 255,255,255 | 255 | emitted |
| `red` / `#FF0000` | 255,0,0 | 255 | emitted |
| `#0000CC` | 0,0,204 | 204 | emitted |
| `gray` / `#808080` | 128,128,128 | 128 | **suppressed** |
| `#666666` | 102,102,102 | 102 | **suppressed** |
| `#333333` | 51,51,51 | 51 | **suppressed** |
| `navy` / `#000080` | 0,0,128 | 128 | **suppressed** |
| `black` / `#000000` | 0,0,0 | 0 | **suppressed** |

When a colour is suppressed, neither the open (`\033[38;2;…m`) nor the close
(`\033[39m`) escape is emitted.  The style-balance invariant is preserved
because the depth counter is not incremented for suppressed colours.

### Named CSS Colours

The 16 basic CSS colour names are resolved to their standard RGB values.
Hex notation `#RRGGBB` and shorthand `#RGB` (expanded by multiplying each
nibble by 17) are also supported.

---

## Pager Integration

### ANSI Reset at Page Boundaries

When the pager renders page *N* > 1, it skips the first `(N-1) * rows_avail`
body lines.  Those skipped lines may contain open ANSI SGR escapes (e.g. a
`<div style="color:white">` spanning many paragraphs sets the foreground colour
in line 0 but closes it many lines later).

Without correction, the text on page 2+ would appear in the terminal's default
colours while only part of the styled span is visible — potentially producing
white-on-white or other unreadable combinations when the email's background
colour and the user's terminal theme differ.

**Fix:** `print_body_page()` scans the skipped content with `ansi_scan()` to
accumulate the SGR state that would be in effect at `from_line`, then calls
`ansi_replay()` to re-emit exactly the escapes needed to restore that state
before the first visible line.

`ansi_scan()` tracks: bold, italic, underline, strikethrough, fg colour,
bg colour.  It honours `\033[0m` (full reset) within the skipped content.

### ANSI Reset at Page Start

Each page redraw begins with `\033[0m\033[H\033[2J` (reset all attributes +
home cursor + clear screen).  After `print_body_page()` returns, `\033[0m` is
emitted again to close any ANSI state that the page body may have left open
(the body text terminates at an arbitrary line boundary, which may be inside a
styled span if `ansi_replay` re-opened one).

The status bar is printed to **stderr** after the body; this ordering (stdout
body + `\033[0m]`, then stderr status) ensures the terminal attribute state is
clean when the status bar's reverse-video (`\033[7m]`) is activated.
