# US-43 — Gmail Message List: Pending-Row Visual Feedback

**As a** Gmail user interacting with messages in label views,
**I want to** see immediately meaningful colour cues when I trigger an operation,
**so that** I can distinguish between destructive (trash), neutral (label removal),
and restorative (label add / untrash) actions without waiting for a refresh.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Pressing **d** (remove current label) marks the row with a **yellow** strikethrough. This distinguishes a non-destructive label change from trashing. |
| 2 | Pressing **D** (trash message) marks the row with a **red** strikethrough — same as before. |
| 3 | Pressing **a** (archive) while already viewing the **Archive** folder (`_nolabel`) does **nothing** — no visual change, no API call. The message is already archived. |
| 4 | Pressing **a** in any other view (e.g. INBOX) still marks the row with a **red** strikethrough — the message will disappear on the next refresh. |
| 5 | When the label picker (**t**) is used in the **Archive** view and a real label is added to the message (which removes it from `_nolabel`), the row is marked with a **green** strikethrough — the message will leave Archive on the next refresh. |
| 6 | When the label picker is used and a label is only toggled (added/removed) without removing the message from the current folder, no row strikethrough is applied. |
| 7 | The colour-only distinction (yellow vs red vs green) is preserved when the cursor is on the pending row (inverse-video + colour + strikethrough). |
| 8 | Pressing **d** a second time on a yellow-strikethrough row cancels the operation and clears the strikethrough (existing undo behaviour, now with yellow instead of red). |

---

## Colour summary

| Key / action | View | Row colour |
|---|---|---|
| `d` — remove label | any | yellow + strikethrough |
| `D` — trash message | any | red + strikethrough |
| `a` — archive | non-Archive view | red + strikethrough |
| `a` — archive | Archive view | *(no change — already archived)* |
| `t` label picker — adds real label | Archive view | green + strikethrough |
| `u` — untrash | Trash view | green + strikethrough |

---

## Implementation notes

* Add a third `pending_label[]` array alongside `pending_remove[]` and
  `pending_restore[]`, rendered as `\033[33m\033[9m` (yellow + strikethrough).
* The `'d'` key handler sets `pending_label[cursor] = 1` instead of
  `pending_remove[cursor] = 1`.
* The `'a'` key handler gains a guard: `if (strcmp(folder, "_nolabel") == 0) break;`
* The `'t'` handler checks `label_idx_contains("_nolabel", uid)` before and after
  `show_label_picker()`; if the message left `_nolabel`, set `pending_restore[cursor] = 1`.

---

## Related

* US-29: Gmail Message Operations (criteria 13–15, 19–21)
* `libemail/src/domain/email_service.c` — key handlers and render loop
* `libs/libptytest/` — add `pty_cell_fg()` for colour-aware PTY assertions
