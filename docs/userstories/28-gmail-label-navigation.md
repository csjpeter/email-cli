# US-28 — Gmail Label Navigation

**As a** Gmail user,
**I want to** browse my messages by labels instead of folders,
**so that** I can use Gmail's native label model without IMAP folder emulation.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Gmail accounts show a **Label List** view instead of the IMAP folder browser. |
| 2 | The label list has three sections separated by dotted dividers: system views, user labels, special views. |
| 3 | System views section shows: INBOX, Starred, Unread, Sent, Drafts — in that fixed order. |
| 4 | User labels section shows all user-created labels sorted alphabetically. |
| 5 | Special views section shows: Archive, Spam, Trash. |
| 6 | Each label displays an unread count on the right (where applicable). |
| 7 | Pressing **Enter** on a label opens the message list filtered to that label. |
| 8 | Pressing **Backspace** returns to the accounts screen. |
| 9 | The header shows `Labels — user@gmail.com  (N)` where N is the total label count. |
| 10 | `IMPORTANT` and `CATEGORY_*` labels are filtered out and never displayed. |
| 11 | The account list shows a **Type** column (`IMAP` / `Gmail`) for each account. |
| 12 | Entering a Gmail account from the account list navigates to the Label List; entering an IMAP account navigates to the Folder Browser. |

---

## Label list layout

```
  Labels — user@gmail.com  (12)

  INBOX                          3
  Starred
  Unread                         8
  Sent
  Drafts                         1
  ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
  Work                           5
  Personal
  Project-X
  ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
  Archive                        (no labels)
  Spam                           2
  Trash                          (auto-delete: 30 days)
```

---

## Account list (Type column)

```
  Accounts  (2)

  Email                    Type    Status
  ═══════════════════════  ══════  ═══════════
  user@example.com         IMAP    3 unread
  user@gmail.com           Gmail   8 unread
```

---

## Key bindings — Label list

| Key | Action |
|-----|--------|
| ↑ / ↓ | Move cursor |
| Enter | Open: show messages with this label |
| Backspace | Return to accounts screen |
| ESC / q | Quit |

---

## Archive view semantics

A message is in the Archive view **if and only if it has no labels** — no INBOX,
no user labels, no STARRED, nothing.  This is intentionally non-overlapping:
a message is either in one or more label views, or in Archive, never both.

---

## Status bar

```
↑↓=navigate  Enter=open  Backspace=accounts  q=quit
```

---

## Implementation notes

* `email_service.c` checks `mail_client_uses_labels()` to decide between
  label list and folder browser rendering.
* Label data comes from local `.idx` files (no network required for navigation).
* `gmail_client.c` provides `gmail_list_labels()` for initial label discovery.

---

## Related

* Spec: `docs/spec/gmail-api.md` sections 3, 4, 12
* GML milestones: GML-16, GML-17
