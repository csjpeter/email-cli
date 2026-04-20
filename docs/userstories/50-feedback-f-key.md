# US-50 — Feedback: `f` key (Star Toggle)

**As a** user pressing **f** to star or unstar a message,
**I want to** see a "Starred" or "Unstarred" confirmation,
**so that** I know whether the star was added or removed.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | After pressing **f** and the message becomes starred, the feedback line shows: `Starred`. |
| 2 | After pressing **f** and the star is removed, the feedback line shows: `Unstarred`. |
| 3 | The feedback applies in Gmail list views and in the message reader. |
| 4 | The feedback applies in IMAP list views (where `f` toggles the `\Flagged` flag). |
| 5 | The star icon (`★`) in the Sts column updates immediately; the feedback line confirms it. |

---

## Message text

| Context | Action | Feedback text |
|---------|--------|---------------|
| Any view | `f` (add star) | `Starred` |
| Any view | `f` (remove star) | `Unstarred` |

---

## Related

* US-44: infrastructure
