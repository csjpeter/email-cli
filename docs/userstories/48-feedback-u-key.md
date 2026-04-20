# US-48 — Feedback: `u` key (Untrash / Restore)

**As a** user pressing **u** to restore a message from Trash,
**I want to** see a "Restored to Inbox" confirmation,
**so that** I know the message was moved back to Inbox and is no longer in Trash.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | After pressing **u** in the Gmail Trash view, the feedback line shows: `Restored to Inbox`. |
| 2 | The feedback appears simultaneously with the green strikethrough on the row (US-43 criterion 21). |
| 3 | Pressing **R** clears the feedback and the row disappears from the Trash list. |

---

## Message text

| Context | Action | Feedback text |
|---------|--------|---------------|
| Gmail Trash view | `u` | `Restored to Inbox` |

---

## Related

* US-44: infrastructure
* US-54: Gmail Trash view
