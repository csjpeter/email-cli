# US-44 — Operation Feedback Line Infrastructure

**As a** user of any interactive list view,
**I want to** see a brief text confirmation of the last operation I performed,
**so that** I can verify my keystrokes had the intended effect without waiting
for a full refresh.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Every interactive list view (Gmail and IMAP) reserves the second-to-last terminal row as the **feedback line**. |
| 2 | The feedback line is rendered in **dim** style (`\033[2m`) and left-aligned with two leading spaces. |
| 3 | The feedback line is empty when the list is first opened (no previous operation). |
| 4 | The feedback line is cleared (emptied) when the user presses **R** (refresh), **ESC**, or **Backspace** (back to folder browser). |
| 5 | The feedback line is NOT cleared by cursor movement keys (arrows, PgUp/PgDn) or by flag-only operations that have no text to report. |
| 6 | The feedback line is updated synchronously in the same render pass as the key that triggered the operation — no perceptible delay. |
| 7 | In non-pager (CLI / batch) mode the feedback line is not printed; it is a TUI-only feature. |
| 8 | The feedback line is erased to end-of-line (`\033[K`) to prevent stale text from a longer previous message showing. |
| 9 | The visible message row count decreases by one (to `terminal_rows − 4`) to make room for the feedback line; the window scroll logic accounts for this. |

---

## Example

```
  Labels — user@gmail.com > INBOX  (3 messages)

  Date              Sts   Subject                      From
  ════════════════  ════  ═══════════════════════════  ════════
  2026-04-16        ----  Quarterly review             Boss
  2026-04-15 ████  ════  Hello there                  Alice   ← yellow (d pressed)
  2026-04-14        ----  Invoice                      Bank

  Label removed: INBOX                                         ← feedback line
  r=remove  d=trash  a=archive  f=star  t=labels  q=back       ← status bar
```

---

## Implementation notes

* `email_service.c`: add `char feedback_msg[256]` to the list view local state.
* Render loop: print feedback line at `rows − 2` before the status bar.
* Each key handler: `snprintf(feedback_msg, …)`.
* See `docs/dev/feedback-line-feature.md` for full design.

---

## Related

* FEAT-44: design document
* US-45 – US-56: per-operation and per-view stories
