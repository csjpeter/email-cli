# US-47 — Feedback: `a` key (Archive)

**As a** user pressing **a** to archive a message,
**I want to** see a confirmation when the message is archived and a clear
explanation when the action is skipped,
**so that** I always know whether the archive action had any effect.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | After pressing **a** in a Gmail list view (not Archive), the feedback line shows: `Archived`. |
| 2 | After pressing **a** in the **Archive** view (`_nolabel`), where the message is already archived, the feedback line shows: `Already in Archive — no change`. |
| 3 | In criterion 2, no visual change occurs on the row (no strikethrough), consistent with US-43 criterion 3. |
| 4 | The feedback appears immediately, before any network operation completes. |
| 5 | In an IMAP list view, **a** archives the message (removes from current folder); the feedback line shows `Archived`. |

---

## Message text

| Context | Action | Feedback text |
|---------|--------|---------------|
| Gmail list (non-Archive) | `a` | `Archived` |
| Gmail Archive view | `a` (no-op) | `Already in Archive — no change` |
| IMAP list | `a` | `Archived` |

---

## Related

* US-43 criterion 3: 'a' in Archive view is a no-op
* US-44: infrastructure
* US-53: Archive view
