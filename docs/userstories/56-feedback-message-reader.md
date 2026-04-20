# US-56 — Feedback: Message Reader

**As a** user reading a message (Gmail or IMAP),
**I want to** see operation feedback after pressing hotkeys in the reader,
**so that** I can confirm label and flag changes without leaving the reading view.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The message reader reserves the second-to-last row as the feedback line. |
| 2 | After **r** (remove current label): `Label removed: <label>`. |
| 3 | After **d** (Gmail: remove label / IMAP: done toggle): same as list view. |
| 4 | After **D** (Gmail: trash): `Moved to Trash`. |
| 5 | After **f**: `Starred` or `Unstarred`. |
| 6 | After **n**: `Marked as unread` or `Marked as read`. |
| 7 | After **t** (label picker): summary per US-49. |
| 8 | After **a** (archive): `Archived`; if already archived: `Already in Archive — no change`. |
| 9 | The feedback line is empty when the reader is first opened. |
| 10 | Pressing **q** / ESC to leave the reader clears the feedback line. |

---

## Layout example (Gmail reader after pressing 'f')

```
  From:    Boss <boss@example.com>
  Date:    2026-04-16 09:30
  To:      user@gmail.com
  Subject: Quarterly review
  Labels:  INBOX, ★ Starred, Work
  ───────────────────────────────────────────────────────────────────

  Hi, please find the quarterly review attached...

  Starred                                                       ← feedback
  r=remove  d=trash  f=star  n=unread  t=labels  q=back        ← status bar
```

---

## Related

* US-44: infrastructure
* US-45 – US-51: per-key messages
* US-29: Gmail message reader layout
