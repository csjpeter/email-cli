# US-66 — Import Rules: Flag Actions (Mark as read, starred, junk, delete)

**As a** user importing Thunderbird filters,
**I want** common flag actions to be automatically converted to email-cli label actions,
**so that** these automations work after import without any manual edits.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `Mark as read` is converted to `then-remove-label = UNREAD` (Gmail) or `then-add-label = _read` (IMAP). |
| 2 | `Mark as starred` / `Mark as flagged` is converted to `then-add-label = _flagged`. |
| 3 | `Mark as junk` is converted to `then-add-label = _junk`. |
| 4 | `Delete` is converted to `then-add-label = _trash`. |
| 5 | No `[warn]` is printed for these four action types. |
| 6 | The rules engine (IMAP path) maps `_read` to clearing `MSG_FLAG_UNSEEN`, and `_trash` to a new `MSG_FLAG_TRASH` bit (or `then-move-folder = Trash`). |
| 7 | `email-cli rules apply` correctly applies `_read`, `_trash` from the local store. |

---

## Conversion table

| Thunderbird action | → | rules.ini |
|---|---|---|
| `Mark as read` | | `then-remove-label = UNREAD` |
| `Mark as starred` | | `then-add-label = _flagged` |
| `Mark as junk` | | `then-add-label = _junk` |
| `Delete` | | `then-add-label = _trash` |

---

## Implementation notes

**Import converter** (`src/main_import_rules.c`): extend the action branch to
recognise these strings and emit the correct `then-add-label` / `then-remove-label`.

**Rules engine** (`libemail/src/domain/email_service.c`): extend `lmap[]` with:

```c
{ "_read",  ~MSG_FLAG_UNSEEN },   /* clear unseen bit */
{ "_trash", MSG_FLAG_TRASH   },   /* needs new bit or use move-folder */
```

---

## Related

* US-64: warn on unsupported elements
* US-59: rules apply
