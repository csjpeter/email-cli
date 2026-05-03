# TASK-002 — Implement US-65 and add functional tests

**Type:** Feature + Test  
**Related US:** US-65  

## Implementation

`src/main_import_rules.c` — extend condition parser:

```c
if (strcmp(f2, "contains") == 0)
    snprintf(glob, sizeof(glob), "*%s*", f3);
else if (strcmp(f2, "begins with") == 0)
    snprintf(glob, sizeof(glob), "%s*", f3);
else if (strcmp(f2, "ends with") == 0)
    snprintf(glob, sizeof(glob), "*%s", f3);
else if (strcmp(f2, "is") == 0)
    snprintf(glob, sizeof(glob), "%s", f3);
else {
    /* warn */
}
```

No engine changes required — glob patterns already support these cases.

## Functional tests (Phase 39)

Add a Phase 39 section to `tests/functional/run_functional.sh` with a synthetic
Thunderbird profile containing:

```
name="Begins rule"
condition="AND (from,begins with,noreply@)"
action="Move to folder"
actionValue="imap://user@server/Noreply"

name="Ends rule"
condition="AND (subject,ends with,[GitHub])"
action="Move to folder"
actionValue="imap://user@server/GitHub"
```

Expected checks:
- `39.1` "Begins rule" → `if-from = noreply@*`
- `39.2` "Ends rule" → `if-subject = *[GitHub]`
- `39.3` No `[warn]` for `begins with`
- `39.4` No `[warn]` for `ends with`

## Definition of done

All Phase 39 checks pass, no regression in Phase 38.
