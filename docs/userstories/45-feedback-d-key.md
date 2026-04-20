# US-45 — Feedback: `d` key (Remove Label / Done toggle)

**As a** user pressing **d** in a message list,
**I want to** see a confirmation message naming the label that was removed,
**so that** I know exactly which label was affected and can undo if needed.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | After pressing **d** in a Gmail label view, the feedback line shows: `Label removed: <label-name>` where `<label-name>` is the currently filtered label (e.g. `INBOX`, `Work`). |
| 2 | If pressing **d** a second time (undo), the feedback line shows: `Undo: <label-name> restored`. |
| 3 | In an IMAP list view, **d** toggles the Done flag; the feedback line shows `Marked done` or `Marked not done` accordingly. |
| 4 | The feedback message appears immediately (same render pass as the yellow strikethrough). |
| 5 | Pressing **R** (refresh) clears the feedback line. |

---

## Message text

| Context | Action | Feedback text |
|---------|--------|---------------|
| Gmail list — any label | first `d` | `Label removed: INBOX` (or `Work`, etc.) |
| Gmail list — any label | second `d` (undo) | `Undo: INBOX restored` |
| IMAP list | `d` (mark done) | `Marked done` |
| IMAP list | `d` (unmark done) | `Marked not done` |

---

## Related

* US-44: feedback line infrastructure
* US-52: Gmail label list view
* US-55: IMAP list view
