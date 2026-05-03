# TASK-005 — Implement US-68 and add functional tests

**Type:** Feature + Test  
**Related US:** US-68  

## Implementation

### 1. `libemail/src/infrastructure/mail_rules.h`

Add negation fields to `MailRule`:
```c
char *if_not_from;
char *if_not_subject;
char *if_not_to;
```

### 2. `libemail/src/infrastructure/mail_rules.c`

`mail_rule_matches()`: for each `if_not_*` field, return 0 if
`fnmatch(pattern, value, FNM_CASEFOLD) == 0` (i.e. pattern matches → NOT matches → reject).

`mail_rules_load()` / `mail_rules_save()`: parse and emit `if-not-from` etc.

`mail_rules_free()`: free the new fields.

### 3. `src/main_import_rules.c`

In condition parser, when `f2` is `"doesn't contain"` or `"isn't"`:

```c
if (strcmp(f2, "doesn't contain") == 0) {
    snprintf(glob, sizeof(glob), "*%s*", f3);
    /* assign to if_not_from / if_not_subject / if_not_to */
} else if (strcmp(f2, "isn't") == 0) {
    snprintf(glob, sizeof(glob), "%s", f3);
    /* assign to if_not_* */
}
```

### 4. `src/main.c` (`rules add`)

Accept `--if-not-from`, `--if-not-subject`, `--if-not-to` options.

`rules list` must display `if-not-*` conditions.

## Unit tests

Add to `tests/unit/test_mail_rules.c`:
- `if-not-from = *@spam.com` does NOT match `sender@spam.com`
- `if-not-from = *@spam.com` DOES match `sender@legit.com`
- Combined: `if-from = *@legit.com` AND `if-not-subject = *spam*` only matches legit + non-spam-subject

## Functional tests (Phase 42)

Synthetic TB profile:
```
name="Not from spam"
condition="AND (from,doesn't contain,@spam.example.com)"
action="Move to folder"
actionValue="imap://user@server/NotSpam"

name="Subject isn't"
condition="AND (subject,isn't,Unsubscribe)"
action="Move to folder"
actionValue="imap://user@server/Regular"
```

Expected checks:
- `42.1` "Not from spam" → `if-not-from = *@spam.example.com*`
- `42.2` "Subject isn't" → `if-not-subject = Unsubscribe`
- `42.3` No `[warn]` for `doesn't contain`
- `42.4` No `[warn]` for `isn't`

## Definition of done

Unit tests pass, Phase 42 checks pass, no regression.
