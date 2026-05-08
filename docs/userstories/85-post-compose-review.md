# US-85 — Post-Compose Review Screen

**As a** user who has just finished writing an email body in the editor,
**I want** a review screen that shows a summary of the headers and attachments
and lets me edit them or add more attachments before sending,
**so that** I can catch mistakes without re-opening the editor, and can add
attachments I forgot during the header phase.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | After the editor exits the TUI shows a **Post-Compose Review** screen instead of a plain `Send? [y/n]` prompt. |
| 2 | The review screen displays the parsed headers: From, To, Cc (if non-empty), Bcc (if non-empty), Subject, and the number of body lines. |
| 3 | The review screen shows the attachment list (filenames only) or `(none)` if no attachments. |
| 4 | The key-binding bar shows: `[s] Send  [e] Edit headers  [b] Edit body  [a] Add attachment  [d] Remove attachment  [q] Cancel`. |
| 5 | Pressing `s` sends the message immediately (no further prompt). |
| 6 | Pressing `e` reopens the pre-compose dialog (US-CD-1) pre-filled with the current To, Cc, Bcc, Subject, and attachment list. After confirming the dialog the editor does **not** reopen; the review screen is shown again with updated headers. |
| 7 | Pressing `b` reopens the editor (`$EDITOR`) with the current draft. After saving and exiting the review screen is shown again with the updated body line count. |
| 8 | Pressing `a` opens an attachment picker (the same file-browser field from US-84) overlaid on the review screen. The picked file is appended to the attachment list and the review screen is redisplayed. |
| 9 | Pressing `d` removes the last attachment from the list and redisplays the review screen. If there are no attachments `d` is a no-op (no error). |
| 10 | Pressing `q` or `Esc` asks `Discard message? [y/n]` and cancels on `y`. |
| 11 | If the To field is empty (e.g. the user deleted it in the editor) the review screen shows an inline error `To: is required` and `s` is disabled until the user presses `e` to fix it. |
| 12 | The review screen replaces the current terminal content cleanly (no leftover editor artifacts). |
| 13 | Reply, Reply-All, and Forward flows use the same review screen after the editor exits. |

---

## Screen layout sketch

```
┌─ Review Message ────────────────────────────────────────────────────────┐
│                                                                          │
│  From:        me@example.com                                             │
│  To:          alice@example.com; bob@example.org                         │
│  Cc:          carol@example.org                                          │
│  Subject:     Meeting tomorrow                                           │
│                                                                          │
│  Attachments: report.pdf  notes.txt                                      │
│  Body:        12 lines                                                   │
│                                                                          │
│  [s] Send   [e] Edit headers   [b] Edit body   [a] Add attachment        │
│  [d] Remove last attachment    [q] Cancel                                │
└──────────────────────────────────────────────────────────────────────────┘
```

Error state (To empty):

```
│  To:          !! empty — press [e] to fix !!                             │
│  ...                                                                     │
│  [s] disabled   [e] Edit headers  ...                                    │
```

---

## Flow diagram

```
compose_dialog() confirmed
        │
        ▼
  $EDITOR opens (draft file)
        │
  user saves & exits editor
        │
        ▼
  post_compose_review() ◄────────────────────────────────────┐
        │                                                     │
   ┌────┴────┬────────┬──────────┬──────────┬──────────┐     │
   │ [s]     │ [e]    │ [b]      │ [a]      │ [d]      │     │
   │ Send    │ Edit   │ Edit     │ Add      │ Remove   │     │
   │         │ headers│ body     │ attach   │ attach   │     │
   └────┬────┴───┬────┴──┬───────┴──┬───────┴──────────┘     │
        │        │       │          │                          │
        ▼        ▼       ▼          ▼                          │
     smtp_  compose_  $EDITOR   file_       update list ──────┘
     send()  dialog()           browser()
        │        │       │
        │        └───────┴──► review again (no re-send of editor)
        ▼
  "Message sent."
```

---

## Technical notes

### New function

```c
/* Returns 0=sent, 1=cancelled */
static int post_compose_review(Config *cfg,
                               ComposeParams *p,
                               const char *draft_path);
```

- Called from `cmd_compose_interactive()` after the editor exits.
- Renders the review screen in a loop until the user sends or cancels.
- Mutates `*p` in place when the user edits headers or attachments.
- Calls `compose_build_message()` and `smtp_send()` on `s`.

### Backward compatibility

The old `Send? [y/n]` prompt is fully replaced by the review screen.
No config knob or `--batch` fallback is needed for the review screen itself;
`--batch` mode (non-interactive) bypasses both the old prompt and the new
review screen and sends immediately after the editor exits (unchanged
behaviour).

### Files changed

| File | Change |
|------|--------|
| `src/main_tui.c` | Replace y/n prompt with `post_compose_review()` in `cmd_compose_interactive()`, `cmd_reply()`, `cmd_reply_all()`, `cmd_forward()` |
| `src/main_tui.c` | Implement `post_compose_review()` |
| `libwrite/src/compose_service.h` | No change needed (ComposeParams already mutable) |

---

## Related user stories

- US-CD-1 … US-CD-12: Pre-compose dialog (base)
- US-84: Compose attachment support
