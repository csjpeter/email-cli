# US-52 — Feedback: Gmail Label List View

**As a** Gmail user browsing messages in any label list (INBOX, Work, etc.),
**I want to** see operation feedback on a dedicated line below the message rows,
**so that** I understand the result of every key I press.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The Gmail label list reserves the second-to-last row as the feedback line (US-44 criterion 1). |
| 2 | After **d**: `Label removed: <label>` |
| 3 | After **d** (undo): `Undo: <label> restored` |
| 4 | After **D**: `Moved to Trash` |
| 5 | After **a** (not in Archive view): `Archived` |
| 6 | After **f** (add): `Starred`; after **f** (remove): `Unstarred` |
| 7 | After **n** (unread): `Marked as unread`; after **n** (read): `Marked as read` |
| 8 | After **t** label picker (see US-49): summary of label change(s) |
| 9 | The feedback line is empty on initial list load and after **R** / ESC. |
| 10 | In INBOX view the feedback line is present and follows the same rules; the `a` action shows `Archived`. |

---

## Layout example (INBOX view after pressing 'd')

```
  Labels — user@gmail.com > INBOX  (3 messages)

  Date              Sts   Subject                      From
  ════════════════  ════  ═══════════════════════════  ════════
▶ 2026-04-16 ████  ════  Quarterly review             Boss    ← yellow
  2026-04-15        ----  Hello there                  Alice
  2026-04-14        ----  Invoice                      Bank

  Label removed: INBOX                                         ← feedback
  r=remove  d=trash  a=archive  f=star  t=labels  q=back       ← status bar
```

---

## Related

* US-44: infrastructure
* US-45 – US-51: per-key messages
