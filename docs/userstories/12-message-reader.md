# User Story: Interactive Message Reader

## Summary
As a user in the interactive TUI, I want to open and read a message with a scrollable
pager inside the terminal.

## Trigger
Pressing Enter on a message in the interactive message list.

## Behaviour
1. The message body is loaded from the local cache or fetched from the server.
2. The header section shows: From, Subject, Date, followed by a separator line.
3. The body is rendered:
   - HTML body is converted to plain text.
   - Plain-text body is word-wrapped to terminal width (max 80 columns).
4. The body is paginated to terminal height minus header lines.
5. Navigation keys:
   | Key | Action |
   |-----|--------|
   | PgDn / ↓ | Scroll forward one page / one line |
   | PgUp / ↑ | Scroll back one page / one line |
   | `r` | Reply to this message (email-tui only; opens `$EDITOR`) |
   | `a` | Save attachment (if present) |
   | ESC / q  | Return to message list |
   | Backspace | Return to message list |
6. A status bar at the bottom shows:
   `-- [page/total] PgDn/↓=scroll  PgUp/↑=back  r=reply  Backspace/ESC/q=list --`
   (attachments add `a=save  A=save-all(N)` before `Backspace`)

## Caching
- Raw message is saved to `~/.local/share/email-cli/accounts/<host>/messages/<folder>/<uid>.eml`
  after first fetch.
- Headers are also cached separately in `headers/<folder>/<uid>.hdr`.

## Non-interactive mode
When accessed via `email-cli show <uid>` with piped output, the body is printed
to stdout all at once without interactive navigation.
