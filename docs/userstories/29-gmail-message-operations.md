# US-29 — Gmail Message Operations

**As a** Gmail user viewing messages in a label,
**I want to** use Gmail-native operations (remove label, trash, archive, star, unread toggle),
**so that** my actions map to real Gmail semantics rather than IMAP flag emulation.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The Gmail message list shows columns: Date, Labels, Subject, From (no UID column). |
| 2 | The Labels column shows all labels on the message, with `★` for STARRED. |
| 3 | Unread messages are rendered in bold or reverse video. |
| 4 | Pressing **r** removes the currently-filtered label from the message via `messages.modify(removeLabelIds)`; the message disappears from the current view. |
| 5 | Pressing **d** trashes the message via `messages.trash()`; all labels are removed and TRASH is added. |
| 6 | Pressing **a** archives the message: removes **all** labels (INBOX, UNREAD, CATEGORY_*, user labels) so the message lives only in All Mail. The message is also marked as read. |
| 7 | Pressing **f** toggles the STARRED label on the message. |
| 8 | Pressing **n** toggles the UNREAD label on the message. |
| 9 | The message reader shows a **Labels** line (e.g. `Labels:  INBOX, ★ Starred, Work`); this line is absent for IMAP messages. |
| 10 | The UNREAD label is **not** shown in the reader Labels line (the user is currently reading it). |
| 11 | Pressing **u** (restore) removes TRASH and adds **INBOX** only — it does **not** attempt to restore pre-trash labels. |
| 12 | Server-side label modifications update the local `.idx` files immediately. |
| 13 | After pressing **d** (or **r**), the row is shown with red foreground and strikethrough until the next refresh, giving immediate visual feedback. |
| 14 | When the cursor is on a pending-remove row, the row is rendered with **both** inverse-video (cursor highlight) and red strikethrough, so the cursor position remains clearly visible. |
| 15 | Pressing **d** a second time on the same pending-remove row cancels the operation: the label is restored locally and the strikethrough is cleared. A subsequent refresh keeps the message visible. |
| 16 | Adding any real label (not UNREAD/STARRED) to a trashed message via the label picker automatically removes the TRASH label — the message is restored from trash without needing a separate untrash step. |
| 17 | When viewing the Trash folder (even when empty), the status bar shows `u=restore  t=labels` instead of the normal compose/reply/archive hints. The **u** key restores a message to its original labels (or Archive if none saved). |
| 18 | The label picker (`t`) includes UNREAD as the first entry so users can toggle read/unread status without leaving the picker. |
| 19 | Pressing **D** (trash) sets an immediate red strikethrough on the row, same as **d** (remove label). |
| 20 | Pressing **a** (archive) sets an immediate red strikethrough on the row — the message disappears from view on the next refresh. |
| 21 | Pressing **u** (restore from Trash) sets an immediate **green** strikethrough on the row, distinguishing restoration from deletion. The message disappears from Trash on the next refresh. |
| 22 | Adding a real label (not UNREAD/STARRED/CATEGORY_*) to an archived message via the label picker removes it from Archive (removes the `_nolabel` virtual index entry). |
| 23 | Archiving removes **all** labels including UNREAD and CATEGORY_* labels; the message is also marked read. The UNSEEN flag in the local `.hdr` store is cleared as well, so the message never appears as unread after archiving. |
| 24 | Messages that arrive from Gmail via sync already in Archive state (no user labels in Gmail) are also treated as read: the sync layer clears the UNSEEN flag in `.hdr` even if the message carries the UNREAD label in Gmail. Any existing archived messages with a stale UNSEEN flag are repaired on the next `email-sync` run. |

---

## Message list layout (Gmail)

```
  Labels — user@gmail.com > INBOX  (3 unread)

  Date              Labels         Subject              From
  ════════════════  ═════════════  ═══════════════════  ════════════
▶ 2026-04-16       ★ Work         Quarterly review     Boss
  2026-04-15                      Hello there          Alice
  2026-04-14       Personal       Invoice              Bank
```

---

## Message reader layout (Gmail)

```
  From:    Boss <boss@example.com>
  Date:    2026-04-16 09:30
  To:      user@gmail.com
  Subject: Quarterly review
  Labels:  INBOX, ★ Starred, Work
  ────────────────────────────────────

  Hi, please find the quarterly review attached...
```

---

## Key bindings (Gmail message list)

| Key | Operation | API call | Effect |
|-----|-----------|----------|--------|
| `r` | Remove label | `messages.modify(removeLabelIds)` | Removes current label; message survives with other labels |
| `d` | Trash | `messages.trash()` | All labels removed, TRASH added; auto-deleted after 30 days |
| `a` | Archive | `messages.modify(remove: INBOX)` | INBOX removed (only in INBOX view) |
| `f` | Star toggle | `messages.modify(STARRED)` | Add or remove STARRED label |
| `n` | Unread toggle | `messages.modify(UNREAD)` | Add or remove UNREAD label |

## Key bindings (Gmail message reader)

| Key | Operation |
|-----|-----------|
| `r` | Remove label (same as list) |
| `d` | Trash (same as list) |
| `f` | Star toggle |
| `n` | Unread toggle |
| `t` | Label picker |

---

## Remove label (`r`) vs Trash (`d`)

This is the core distinction of Gmail-native behaviour:

- **`r` (remove label)** = the label comes off, the message stays alive with its
  other labels.  If no other labels remain, the message moves to Archive.
- **`d` (trash)** = a compound operation that strips **all** labels and adds TRASH.
  Message is permanently deleted by Google after 30 days.

---

## Status bar (Gmail)

| View | Status bar text |
|------|----------------|
| Message list (INBOX) | `r=remove  d=trash  a=archive  f=star  t=labels  c=compose  q=back` |
| Message list (other) | `r=remove  d=trash  f=star  t=labels  c=compose  q=back` |
| Message reader | `r=remove  d=trash  f=star  n=unread  t=labels  q=back` |

---

## Implementation notes

* `email_service.c` dispatches Gmail-specific key bindings when
  `mail_client_uses_labels()` returns true.
* `gmail_client.c` provides `gmail_modify_message()`, `gmail_trash_message()`,
  and `gmail_untrash_message()`.
* Label changes update local `.idx` files via `local_store` functions.

---

## Related

* Spec: `docs/spec/gmail-api.md` sections 5, 9, 14, 15
* GML milestones: GML-18, GML-19, GML-20, GML-21
