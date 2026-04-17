# US-30 — Gmail Label Picker

**As a** Gmail user,
**I want to** toggle labels on a message via an interactive checkbox popup,
**so that** I can organise my messages into labels without leaving the TUI.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Pressing **t** on a message (in list or reader) opens a label picker overlay. |
| 2 | The picker shows all toggleable labels: system labels (INBOX, STARRED, UNREAD) and all user labels. |
| 3 | TRASH, SPAM, DRAFTS, SENT are **excluded** from the picker (managed by compound API calls). |
| 4 | `[x]` indicates the message currently has the label; `[ ]` indicates it does not. |
| 5 | Pressing **Enter** toggles the selected label immediately via `messages.modify()`. |
| 6 | The checkbox updates in-place after the API call. |
| 7 | **↑ / ↓** navigate the picker list. |
| 8 | **q / ESC** close the picker and return to the previous view. |
| 9 | If toggling results in the message having **no labels at all**, it migrates to Archive (`_nolabel.idx`). |
| 10 | Popup width adapts to label name lengths, capped at 40 columns. |
| 11 | If labels exceed available popup height, the list scrolls vertically. |

---

## Label picker layout

```
  ┌─ Labels ─────────────┐
  │ [x] INBOX            │
  │ [ ] Starred          │
  │ [x] Work             │
  │ [ ] Personal         │
  │ [ ] Project-X        │
  │                      │
  │ Enter=toggle  q=done │
  └──────────────────────┘
```

---

## Key bindings — Label picker

| Key | Action |
|-----|--------|
| ↑ / ↓ | Navigate the label list |
| Enter | Toggle selected label on/off |
| q / ESC | Close picker, return to previous view |

---

## Status bar (while picker is open)

```
Enter=toggle  ↑↓=navigate  q=done
```

---

## Implementation notes

* The picker overlay is centred horizontally on the terminal.
* Label state is read from local `.idx` files for instant display.
* Each toggle fires a single `gmail_modify_message()` call.
* After closing the picker, the message list / reader refreshes to reflect
  any label changes.

---

## Related

* Spec: `docs/spec/gmail-api.md` section 13
* GML milestones: GML-21, GML-23
