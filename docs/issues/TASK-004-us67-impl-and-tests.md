# TASK-004 — Implement US-67 and add functional tests

**Type:** Feature + Test  
**Related US:** US-67  

## Implementation

`src/main_import_rules.c` — extend action handler for `action=Label`:

```c
else if (strcmp(val, "Label") == 0) {
    /* actionValue will be set in the next line; store placeholder */
    cur->pending_label_action = 1;
    cur_converted_act++;
}
```

In the `actionValue` handler, add label mapping:

```c
static const char *tb_label_name(const char *tag) {
    if (strcmp(tag, "$label1") == 0) return "Important";
    if (strcmp(tag, "$label2") == 0) return "Work";
    if (strcmp(tag, "$label3") == 0) return "Personal";
    if (strcmp(tag, "$label4") == 0) return "TODO";
    if (strcmp(tag, "$label5") == 0) return "Later";
    /* Unknown $labelN */
    static char buf[32];
    if (tag[0] == '$' && strncmp(tag, "$label", 6) == 0)
        snprintf(buf, sizeof(buf), "Label%s", tag + 6);
    else
        snprintf(buf, sizeof(buf), "%s", tag);
    return buf;
}

/* in actionValue handler: */
if (cur->pending_label_action) {
    const char *lname = tb_label_name(val);
    /* add label to then_add_label array */
    cur->pending_label_action = 0;
}
```

Add `int pending_label_action` to the local `cur` tracking (not to `MailRule`).

## Functional tests (Phase 41)

Synthetic TB profile:
```
name="Important label"
condition="AND (subject,contains,URGENT)"
action="Label"
actionValue="$label1"

name="Work label"
condition="AND (from,contains,@company.com)"
action="Label"
actionValue="$label2"

name="Unknown label"
condition="AND (from,contains,@other.com)"
action="Label"
actionValue="$label9"
```

Expected checks:
- `41.1` "Important label" → `then-add-label = Important`
- `41.2` "Work label" → `then-add-label = Work`
- `41.3` "Unknown label" → `then-add-label = Label9`
- `41.4` No `[warn]` for `Label` action
- `41.5` No `[warn]` for `$label1`–`$label5`

## Definition of done

All Phase 41 checks pass, no regression in earlier phases.
