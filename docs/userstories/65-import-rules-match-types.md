# US-65 — Import Rules: "begins with" / "ends with" Match Types

**As a** user importing Thunderbird filters,
**I want** `begins with` and `ends with` match types to be automatically converted to glob patterns,
**so that** these common conditions work correctly after import without manual editing.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `begins with X` is converted to glob `X*` for the corresponding `if-from`/`if-subject`/`if-to`. |
| 2 | `ends with X` is converted to glob `*X`. |
| 3 | `is X` (exact match) is converted to glob `X` (no wildcards). |
| 4 | `isn't X` produces a `[warn]` (negation not yet supported — see US-68). |
| 5 | No `[warn]` is printed for `begins with`, `ends with`, or `is` match types. |
| 6 | Conversion is case-insensitive at match time (fnmatch FNM_CASEFOLD). |

---

## Conversion table

| Thunderbird | → | rules.ini glob |
|---|---|---|
| `from,begins with,@github` | | `if-from = @github*` |
| `from,ends with,.com` | | `if-from = *.com` |
| `subject,is,[GitHub]` | | `if-subject = [GitHub]` |
| `subject,isn't,junk` | | warn: negation not supported |

---

## Implementation notes

Only `src/main_import_rules.c` needs to change (no engine changes).
In the condition parser, extend the `supported_match` check:

```c
if (strstr(f2, "begins with")) snprintf(glob, sizeof(glob), "%s*", f3);
else if (strstr(f2, "ends with")) snprintf(glob, sizeof(glob), "*%s", f3);
else if (strcmp(f2, "is") == 0) snprintf(glob, sizeof(glob), "%s", f3);
else if (strcmp(f2, "contains") == 0) snprintf(glob, sizeof(glob), "*%s*", f3);
```

---

## Related

* US-64: warn on unsupported elements
* US-68: negation conditions
