# US-22 — Context-Sensitive Help Panel

**As a** user who is unsure which keys are available in the current view,
**I want to** press `h` or `?` in any view and see a popup listing all shortcuts,
**so that** I can discover and use keyboard commands without leaving the application.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Pressing `h` or `?` in any TUI view opens a two-column help overlay. |
| 2 | The overlay is displayed centred over the current view without destroying it. |
| 3 | The overlay title identifies the current view (e.g. "Message list shortcuts"). |
| 4 | The left column shows the key name; the right column shows a short description. |
| 5 | A footer reads "Press any key to close". |
| 6 | Any keypress dismisses the overlay and returns to the same view. |
| 7 | The overlay is available in: **Accounts**, **Folder browser**, **Message list**, **Message reader**. |

---

## Views and their shortcuts

### Accounts

| Key | Description |
|-----|-------------|
| ↑ / ↓ | Move cursor |
| Enter | Open selected account |
| n | Add new account |
| d | Delete selected account |
| e | Edit SMTP settings |
| ESC / q | Quit |
| h / ? | Show this help |

### Folder browser

| Key | Description |
|-----|-------------|
| ↑ / ↓ | Move cursor |
| PgUp / PgDn | Move cursor one page |
| Enter | Open folder / navigate into subfolder |
| t | Toggle tree / flat view |
| Backspace | Go up one level (or back to accounts) |
| ESC / q | Quit |
| h / ? | Show this help |

### Message list

| Key | Description |
|-----|-------------|
| ↑ / ↓ | Move cursor up / down |
| PgUp / PgDn | Move cursor one page |
| Enter | Open selected message |
| r | Reply to selected message |
| c | Compose new message |
| n | Toggle New (unread) flag |
| f | Toggle Flagged (starred) flag |
| d | Toggle Done flag |
| s | Start background sync |
| R | Refresh after sync |
| Backspace | Open folder browser |
| ESC / q | Quit |
| h / ? | Show this help |

### Message reader

| Key | Description |
|-----|-------------|
| PgDn / ↓ | Scroll down one page / one line |
| PgUp / ↑ | Scroll up one page / one line |
| r | Reply to this message |
| a | Save an attachment |
| A | Save all attachments |
| Backspace | Back to message list |
| ESC / q | Back to message list |
| h / ? | Show this help |

---

## Implementation notes

* `show_help_popup(title, rows[][2], n)` renders the overlay to **stderr** so
  it overlays the existing stdout content without clearing the view.
* The popup is drawn using box-drawing characters in reverse-video style.
* After dismissal the popup area is cleared (spaces written over it).
* The overlay is entirely self-contained inside `email_service.c`; no changes
  to callers are needed.
