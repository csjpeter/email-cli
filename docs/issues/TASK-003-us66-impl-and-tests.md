# TASK-003 — Implement US-66 and add functional tests

**Type:** Feature + Test  
**Related US:** US-66  

## Implementation

### `src/main_import_rules.c` — extend action handler

```c
if (strstr(val, "Move"))           { /* existing */ }
else if (strcmp(val, "Mark as read") == 0)
    mail_rule_add_rm_label(cur, "UNREAD");   /* then-remove-label */
else if (strcmp(val, "Mark as starred") == 0 ||
         strcmp(val, "Mark as flagged") == 0)
    mail_rule_add_add_label(cur, "_flagged");
else if (strcmp(val, "Mark as junk") == 0)
    mail_rule_add_add_label(cur, "_junk");
else if (strcmp(val, "Delete") == 0)
    mail_rule_add_add_label(cur, "_trash");
else { /* [warn] */ }
```

### Engine impact

`email_service_apply_rules` (IMAP path) must map virtual labels:
- `_flagged` → set `MSG_FLAG_FLAGGED` on the message
- `_junk` → set `MSG_FLAG_JUNK` (or move to Junk folder if no flag exists)
- `_trash` → `then-move-folder = Trash` equivalent

For Gmail, `UNREAD` removal is already handled; `_flagged`/`_junk`/`_trash` map to
Gmail `STARRED`, `SPAM`, `TRASH` label operations.

## Functional tests (Phase 40)

Synthetic TB profile:
```
name="Read rule"
condition="AND (from,contains,@newsletter.com)"
action="Mark as read"

name="Junk rule"
condition="AND (from,contains,@spam.example.com)"
action="Mark as junk"

name="Delete rule"
condition="AND (subject,contains,Unsubscribe)"
action="Delete"
```

Expected checks:
- `40.1` "Read rule" → `then-remove-label = UNREAD`
- `40.2` "Junk rule" → `then-add-label = _junk`
- `40.3` "Delete rule" → `then-add-label = _trash`
- `40.4` No `[warn]` for `Mark as read`
- `40.5` No `[warn]` for `Mark as junk`
- `40.6` No `[warn]` for `Delete`

## Definition of done

All Phase 40 checks pass, no regression in earlier phases.
