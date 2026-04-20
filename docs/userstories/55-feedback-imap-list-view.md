# US-55 — Feedback: IMAP Message List View

**As a** user of an IMAP account browsing the message list,
**I want to** see a feedback line after flag and folder operations,
**so that** I know which flag was toggled or which folder action was taken.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The IMAP message list reserves the second-to-last row as the feedback line. |
| 2 | After **d** (mark done): `Marked done`. |
| 3 | After **d** (unmark done): `Marked not done`. |
| 4 | After **f** (flag): `Starred`. |
| 5 | After **f** (unflag): `Unstarred`. |
| 6 | After **n** (mark unread): `Marked as unread`. |
| 7 | After **n** (mark read): `Marked as read`. |
| 8 | After **a** (archive): `Archived`. |
| 9 | After **D** (trash, if supported): `Moved to Trash`. |
| 10 | The feedback line is empty on initial load and after **R** / ESC. |
| 11 | Flag changes are queued (pending flag queue); the feedback appears before the flag change is confirmed by the server. |

---

## Message text summary

| Key | Action | Feedback text |
|-----|--------|---------------|
| `d` | mark done | `Marked done` |
| `d` | unmark done | `Marked not done` |
| `f` | flag | `Starred` |
| `f` | unflag | `Unstarred` |
| `n` | mark unread | `Marked as unread` |
| `n` | mark read | `Marked as read` |
| `a` | archive | `Archived` |
| `D` | trash | `Moved to Trash` |

---

## Related

* US-44: infrastructure
* US-45: 'd' key
* US-50: 'f' key
* US-51: 'n' key
