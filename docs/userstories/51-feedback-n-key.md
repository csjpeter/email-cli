# US-51 — Feedback: `n` key (Unread / Read Toggle)

**As a** user pressing **n** to toggle the read/unread state of a message,
**I want to** see a "Marked unread" or "Marked read" confirmation,
**so that** I can confirm the state change without re-reading the Sts column.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | After pressing **n** and the message becomes unread, the feedback line shows: `Marked as unread`. |
| 2 | After pressing **n** and the message becomes read, the feedback line shows: `Marked as read`. |
| 3 | The feedback applies in Gmail list views and in the message reader. |
| 4 | The feedback applies in IMAP list views (where `n` toggles `\Seen`). |
| 5 | The `N` indicator in the Sts column updates immediately; the feedback confirms it. |

---

## Message text

| Context | Action | Feedback text |
|---------|--------|---------------|
| Any view | `n` (mark unread) | `Marked as unread` |
| Any view | `n` (mark read) | `Marked as read` |

---

## Related

* US-44: infrastructure
