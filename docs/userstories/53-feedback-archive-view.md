# US-53 — Feedback: Gmail Archive View

**As a** Gmail user browsing the Archive (`_nolabel`) view,
**I want to** see operation feedback that is specific to archive semantics,
**so that** no-op actions are clearly explained and restorative actions are confirmed.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The Archive view reserves the second-to-last row as the feedback line. |
| 2 | After pressing **a** (already archived): `Already in Archive — no change`. No row strikethrough. |
| 3 | After the label picker (**t**) adds a real label that moves the message out of Archive: `<label> added — moved out of Archive`. The row shows green strikethrough (US-43 criterion 5). |
| 4 | After **D**: `Moved to Trash`. Red strikethrough. |
| 5 | After **f**: `Starred` or `Unstarred`. |
| 6 | After **n**: `Marked as unread` or `Marked as read`. |
| 7 | The feedback line is empty on initial list load and after **R** / ESC. |

---

## Layout example (after pressing 'a' — no-op)

```
  Labels — user@gmail.com > Archive  (1 message)

  Date              Sts   Subject                      From
  ════════════════  ════  ═══════════════════════════  ════════
▶ 2026-03-12        ----  Reminder                     Google

  Already in Archive — no change                               ← feedback
  d=trash  f=star  t=labels  q=back                            ← status bar
```

---

## Related

* US-44: infrastructure
* US-47: 'a' key feedback
* US-49: label picker feedback
* US-43 criterion 3, 5: visual row behaviour
