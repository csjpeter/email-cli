# US-49 — Feedback: `t` key (Label Picker)

**As a** user who opens the label picker with **t** and toggles labels,
**I want to** see a summary of what changed after closing the picker,
**so that** I know which labels were added or removed without re-opening the picker.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | After closing the label picker, the feedback line shows a summary of changes. |
| 2 | If exactly one label was added: `Label added: <name>`. |
| 3 | If exactly one label was removed: `Label removed: <name>`. |
| 4 | If a label was added that moved the message out of Archive (`_nolabel`): `<label> added — moved out of Archive`. |
| 5 | If a label was added that moved the message out of Trash: `<label> added — restored from Trash`. |
| 6 | If multiple labels were changed: `Labels updated`. |
| 7 | If the picker was closed without any change (ESC with no toggles): the feedback line is not changed. |
| 8 | The feedback message includes the **display name** of the label (e.g. `Work`, not `Label_123`). |

---

## Message text

| Context | Change | Feedback text |
|---------|--------|---------------|
| Any view | 1 label added | `Label added: Work` |
| Any view | 1 label removed | `Label removed: Work` |
| Archive view | real label added | `Work added — moved out of Archive` |
| Trash view | real label added | `Work added — restored from Trash` |
| Any view | multiple changes | `Labels updated` |
| Any view | no change | *(unchanged)* |

---

## Related

* US-44: infrastructure
* US-53: Archive view (unarchive case)
* US-54: Trash view (untrash via picker)
