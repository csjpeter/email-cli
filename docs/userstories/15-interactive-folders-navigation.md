# US-15 — Interactive Folder Browser Navigation

## Summary
As a user in the interactive TUI, I want to browse, navigate, and switch between IMAP
folders using keyboard shortcuts, and see at a glance how much unread and flagged mail
is in each folder — including inside collapsed subtrees.

## Trigger
Pressing Backspace in the interactive message list opens the folder browser.

## Navigation Keys
| Key | Action |
|-----|--------|
| ↑ / ↓ | Move cursor to the previous / next folder |
| PgUp / PgDn | Scroll the folder list one page up / down |
| Enter | Select the highlighted folder and open its message list |
| Backspace | In flat view with subfolder prefix: navigate up one level. At root: return to the Accounts screen. |
| ESC / q | Exit the application |
| `t` | Toggle between tree view and flat view |

## Columns
```
  Unread  Flagged  Folder          Total
  ══════  ═══════  ══════════════  ═══════
```

| Column | Description |
|--------|-------------|
| Unread | Unseen messages in this folder (flat mode: includes all descendants when collapsed) |
| Flagged | Flagged messages in this folder (flat mode: includes all descendants when collapsed) |
| Folder | Folder name; in flat mode a `/` suffix indicates the folder has children |
| Total  | Total messages stored locally |

## Aggregate counts in flat mode

When a folder row is shown with a `/` suffix (it has sub-folders and the user has
not navigated into it yet), the **Unread** and **Flagged** counts are the **sum of
the folder itself plus all its descendants**.  This lets the user see how much
unread mail is hidden inside a collapsed subtree without navigating into it.

In tree mode all folders are always visible, so each row shows only its own counts.

## Behaviour
- Folders are displayed as a tree by default (sub-folders shown indented under parent).
- Pressing `t` toggles between tree and flat view; the status bar updates to show the
  other available mode (`t=flat` in tree mode, `t=tree` in flat mode).
- The selected folder row is highlighted in reverse video.
- Empty folders are dimmed.
- Pressing Enter on a folder immediately reloads the message list for that folder.
- Counts are read from local manifests — no network connection is required.

## Status Bar Format (tree mode)
```
  ↑↓=step  PgDn/PgUp=page  Enter=open/select  t=flat  Backspace=back  ESC=quit
```
