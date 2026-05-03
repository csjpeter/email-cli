# BUG-001 — `isn't` match type silently converts to positive exact match

**Severity:** Medium  
**Component:** `src/main_import_rules.c`  
**Related US:** US-68, US-74  

---

## Description

In `parse_tb_filter_file()`, the `supported_match` check uses:

```c
int supported_match = (strstr(f2, "contains") != NULL ||
                       strstr(f2, "is") != NULL);
```

`strstr("isn't", "is")` returns non-NULL, so `"isn't"` is treated as a
supported match type and silently converted:

```c
else
    snprintf(glob, sizeof(glob), "%s", f3);  /* exact-match glob */
```

As a result, a Thunderbird condition `(from,isn't,spam)` is imported as:

```ini
if-from = spam
```

…which is the **opposite** of the intended logic.  The correct behaviour
is to emit a `[warn]` and skip the term (same as `"doesn't contain"`).

---

## Steps to reproduce

```
name="Negation rule"
condition="AND (from,isn't,spam@example.com)"
action="Move to folder"
actionValue="imap://user@server/NotSpam"
```

Run `email-import-rules --dry-run`.

**Actual:** `if-from = spam@example.com` (wrong positive match, no warning)  
**Expected:** `[warn] Rule "Negation rule": match type "isn't" is not supported …`

---

## Fix

Change the `supported_match` check to use exact comparisons or exclude
negated variants:

```c
int supported_match = (strcmp(f2, "contains") == 0 ||
                       strcmp(f2, "is")        == 0);
```

This ensures `"doesn't contain"` and `"isn't"` both fall through to the
warn branch.

---

## Acceptance test

Add to Phase 38:
```
check "38.X warn for isn't match type" "\[warn\].*isn" "$IR_OUT"
```
