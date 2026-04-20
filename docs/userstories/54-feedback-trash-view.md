# US-54 — Feedback: Gmail Trash View

**As a** Gmail user browsing the Trash view,
**I want to** see confirmation messages for restore and re-trash operations,
**so that** I can manage trashed messages confidently.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The Trash view reserves the second-to-last row as the feedback line. |
| 2 | After pressing **u** (restore to Inbox): `Restored to Inbox`. Green strikethrough on row. |
| 3 | After the label picker (**t**) adds a real label (implicit untrash): `<label> added — restored from Trash`. Green strikethrough. |
| 4 | After **D** (re-trash a message already in trash, idempotent): `Already in Trash — no change`. No row change. |
| 5 | After **f**: `Starred` or `Unstarred`. |
| 6 | The feedback line is empty on initial load and after **R** / ESC. |
| 7 | When the Trash folder is empty the feedback line is also empty. |

---

## Layout example (after pressing 'u')

```
  Labels — user@gmail.com > Trash  (1 message)

  Date              Sts   Subject                      From
  ════════════════  ════  ═══════════════════════════  ════════
▶ 2026-04-10 ████  ════  Old newsletter               Sender  ← green

  Restored to Inbox                                            ← feedback
  u=restore  t=labels  s=sync  R=refresh  [1/1]               ← status bar
```

---

## Related

* US-44: infrastructure
* US-48: 'u' key feedback
* US-49: label picker feedback
