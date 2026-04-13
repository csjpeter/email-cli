# User Story: Interactive List Navigation

## Summary
As a user in the interactive TUI, I want to navigate the message list using keyboard
shortcuts and toggle message flags without opening a message.

## Trigger
The interactive list is active (opened by `email-cli` with no arguments, or by selecting
a folder in the folder browser).

## Navigation Keys
| Key | Action |
|-----|--------|
| ↑ / ↓ | Move cursor one row up / down |
| PgUp | Scroll list one page up |
| PgDn | Scroll list one page down |
| Enter | Open the selected message in the show view |
| Backspace | Open the folder browser |
| ESC / q | Exit the application |

## Flag Keys (act on the selected message)
| Key | Action |
|-----|--------|
| `n` | Toggle `\Seen` flag (mark as new / mark as read) |
| `f` | Toggle `\Flagged` flag |
| `d` | Toggle `\Deleted` flag (mark as done/deleted) |

## Behaviour
- The cursor highlights the selected message row in reverse video.
- Pressing a flag key immediately queues a flag change; the change is sent to the
  server in the background and reflected in the list.
- Navigation wraps: pressing ↓ at the last message keeps the cursor at the last message
  (no wrap-around); pressing ↑ at the first message keeps the cursor at the first message.
- The status bar always shows the current cursor position `[n/total]`.

## Status Bar Format
```
  ↑↓=step  PgDn/PgUp=page  Enter=open  n=new  f=flag  d=done  Backspace=folders  ESC=quit  [cursor/total]
```
