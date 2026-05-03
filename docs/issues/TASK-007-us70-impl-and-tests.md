# TASK-007 — Implement US-70 and add functional tests

**Type:** Feature + Test  
**Related US:** US-70  

## Implementation

### 1. `libemail/src/infrastructure/mail_rules.h`

```c
int if_age_gt;   /* days, 0 = disabled */
int if_age_lt;   /* days, 0 = disabled */
```

### 2. `libemail/src/infrastructure/mail_rules.c`

`mail_rules_apply()` signature extension:
```c
int mail_rules_apply(...,
                     time_t message_date,   /* NEW — 0 = unknown, skip age checks */
                     ...);
```

`mail_rule_matches()`: add `time_t message_date`.

```c
if (rule->if_age_gt > 0 && message_date > 0) {
    int age = (int)((time(NULL) - message_date) / 86400);
    if (age <= rule->if_age_gt) return 0;
}
if (rule->if_age_lt > 0 && message_date > 0) {
    int age = (int)((time(NULL) - message_date) / 86400);
    if (age >= rule->if_age_lt) return 0;
}
```

`mail_rules_load()` / `mail_rules_save()`: parse and emit `if-age-gt` / `if-age-lt`.

### 3. `libemail/src/domain/email_service.c`

For IMAP `rules apply`: extract the `Date` field from the manifest entry,
parse it with `mime_parse_date()`, pass the resulting `time_t` to `mail_rules_apply`.

### 4. `src/main.c` (`rules add` / `rules list`)

Accept `--if-age-gt <days>` and `--if-age-lt <days>`.

### 5. `src/main_import_rules.c`

When condition field is `"age"` and match type is `"greater than"`:
```c
cur->if_age_gt = atoi(f3);
cur_converted_cond++;
```
When `"less than"`: `cur->if_age_lt = atoi(f3)`.
No `[warn]` for these once implemented.

## Update call sites

Every existing `mail_rules_apply()` call site must pass the new `message_date`
parameter (pass `0` if date is unknown to preserve existing behavior).

## Unit tests

Add to `tests/unit/test_mail_rules.c`:
- `if-age-gt = 30` does NOT match message from yesterday (age = 1 day)
- `if-age-gt = 30` DOES match message from 60 days ago
- `if-age-lt = 7` DOES match message from yesterday
- `if-age-lt = 7` does NOT match message from 30 days ago
- `message_date = 0` → age conditions are skipped (always matches for GT/LT)

## Functional tests (Phase 44)

Synthetic TB profile:
```
name="Old messages"
condition="AND (age,greater than,90)"
action="Move to folder"
actionValue="imap://user@server/Archive"

name="Recent messages"
condition="AND (age,less than,7)"
action="Move to folder"
actionValue="imap://user@server/Recent"
```

Expected checks:
- `44.1` "Old messages" → `if-age-gt = 90`
- `44.2` "Recent messages" → `if-age-lt = 7`
- `44.3` No `[warn]` for `age,greater than`
- `44.4` No `[warn]` for `age,less than`

## Definition of done

Unit tests pass, Phase 44 checks pass, no regression.
