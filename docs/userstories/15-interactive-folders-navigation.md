# User Story: Interactive Folder Browser Navigation

## Summary
As a user in the interactive TUI, I want to browse, navigate, and switch between IMAP
folders using keyboard shortcuts.

## Trigger
Pressing Backspace in the interactive message list opens the folder browser.

## Navigation Keys
| Key | Action |
|-----|--------|
| ↑ / ↓ | Move cursor to the previous / next folder |
| PgUp / PgDn | Scroll the folder list one page up / down |
| Enter | Select the highlighted folder and open its message list |
| Backspace | In flat view with subfolder prefix: navigate up one level. At root: return to the previously active folder's message list. |
| ESC | Exit the application |
| `t` | Toggle between tree view and flat view |

## Behaviour
- Folders are displayed as a tree by default (sub-folders shown indented under parent).
- Pressing `t` toggles between tree and flat view; the status bar updates to show the
  other available mode (`t=flat` in tree mode, `t=tree` in flat mode).
- The selected folder row is highlighted in reverse video.
- Pressing Enter on a folder immediately reloads the message list for that folder.
- The folder browser shows the account separator and folder hierarchy from the IMAP server.

## Status Bar Format (tree mode)
```
  ↑↓=step  Enter=open/select  Backspace=back  t=flat  ESC=quit
```
