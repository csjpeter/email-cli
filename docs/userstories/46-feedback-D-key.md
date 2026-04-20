# US-46 — Feedback: `D` key (Trash)

**As a** user pressing **D** to trash a message,
**I want to** see a "Moved to Trash" confirmation,
**so that** I can distinguish a successful trash operation from any other
red-strikethrough action.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | After pressing **D** in any Gmail list view, the feedback line shows: `Moved to Trash`. |
| 2 | The feedback appears simultaneously with the red strikethrough on the row. |
| 3 | Pressing **R** (refresh) clears the feedback and the row disappears from the list. |

---

## Message text

| Context | Action | Feedback text |
|---------|--------|---------------|
| Any Gmail list | `D` | `Moved to Trash` |

---

## Related

* US-44: infrastructure
* US-46 (this story), US-54: Trash view
