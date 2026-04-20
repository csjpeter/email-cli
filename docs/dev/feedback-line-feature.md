# FEAT-44 — Operation Feedback Line

## Summary

Add a persistent **operation feedback line** to every interactive list view.
After each user action the line shows a brief, human-readable confirmation
(e.g. *"INBOX label removed"*, *"Moved to Trash"*, *"Starred"*) so the user
never has to guess whether their keystroke had any effect.

---

## Problem

Currently the TUI gives only implicit feedback: the row turns yellow/red/green
and (after a refresh) disappears.  There is no textual confirmation of what
actually happened, making the interface harder to learn and more error-prone.

---

## Proposed layout

```
  ┌─────────────────────────────────────────────────────────────────────┐
  │  Title row                                                          │
  │  Column headers                                                     │
  │  Message rows  (terminal_rows − 4)                                  │
  │  ...                                                                │
  │  Feedback line  ← NEW: last operation result (row N−2)             │
  │  Status bar     (row N−1)                                           │
  └─────────────────────────────────────────────────────────────────────┘
```

The feedback line is dimmed (`\033[2m`) and left-aligned.  It is cleared
(set to empty) on `R` (refresh), `ESC`, or navigation away.  It is NOT
cleared by arrow keys or other non-modifying actions.

---

## System design

### State

```c
char feedback_msg[256];   /* "" = no feedback to show */
```

Allocated on the stack alongside `pending_remove[]`.  Cleared at list entry
and on `R` / ESC / navigation.

### Render

At the bottom of the render loop, before the status bar:

```c
if (opts->pager && feedback_msg[0]) {
    move_cursor_to(rows - 2, 0);
    printf("\033[2m  %s\033[0m\033[K", feedback_msg);
}
```

### Setting the message

Each key handler calls:

```c
snprintf(feedback_msg, sizeof(feedback_msg), "...");
```

### Clearing the message

```c
feedback_msg[0] = '\0';
```

Called by: `'R'`, `ESC`, `PTY_KEY_BACK`, pager navigation to reader.

---

## Scope

| Layer | File |
|-------|------|
| Domain | `libemail/src/domain/email_service.c` |
| Display | Same file, render loop |
| Tests | `tests/pty/test_pty_gmail_tui.c`, `tests/pty/test_pty_views.c` |

---

## Related user stories

| Story | Topic |
|-------|-------|
| US-44 | Infrastructure: feedback line rendering |
| US-45 | `d` — remove label feedback |
| US-46 | `D` — trash feedback |
| US-47 | `a` — archive feedback |
| US-48 | `u` — untrash feedback |
| US-49 | `t` — label picker feedback |
| US-50 | `f` — star toggle feedback |
| US-51 | `n` — unread toggle feedback |
| US-52 | Gmail label list view |
| US-53 | Gmail Archive view |
| US-54 | Gmail Trash view |
| US-55 | IMAP list view |
| US-56 | Message reader |
