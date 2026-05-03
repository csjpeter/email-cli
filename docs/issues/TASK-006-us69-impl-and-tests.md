# TASK-006 — Implement US-69 and add functional tests

**Type:** Feature + Test  
**Related US:** US-69  

## Implementation

### 1. `libemail/src/infrastructure/mail_rules.h`

```c
char *if_body;   /* glob, NULL = any */
```

### 2. `libemail/src/infrastructure/mail_rules.c`

`mail_rules_apply()` signature extension:
```c
int mail_rules_apply(const MailRules *rules,
                     const char *from, const char *subject,
                     const char *to, const char *labels_csv,
                     const char *body,   /* NEW — NULL = skip body check */
                     char ***add_out, int *add_count,
                     char ***rm_out,  int *rm_count);
```

`mail_rule_matches()`: add `const char *body` parameter; if `rule->if_body` is set
and body is non-NULL, return 0 unless `fnmatch(rule->if_body, body, FNM_CASEFOLD) == 0`.

`mail_rules_load()` / `mail_rules_save()`: parse and emit `if-body`.

`mail_rules_free()`: free `if_body`.

### 3. `libemail/src/domain/email_service.c`

For IMAP `rules apply`: load `.eml` from local store, extract plain-text body
via `mime_decode_body()` (or an equivalent helper), pass to `mail_rules_apply`.
If the `.eml` is not locally available, pass `NULL` (body check skipped).

### 4. `src/main.c` (`rules add` / `rules list`)

Accept `--if-body <glob>`.  Display `if-body` in `rules list`.

### 5. `src/main_import_rules.c`

When condition field is `"body"` and match type is `"contains"`:
```c
snprintf(glob, sizeof(glob), "*%s*", f3);
cur->if_body = strdup(glob);
```
When `"begins with"`:  `X*`
No `[warn]` emitted for these cases once implemented.

## Update call sites

Every existing `mail_rules_apply()` call site must pass the new `body` parameter
(pass `NULL` if body is not available to preserve existing behavior).

## Unit tests

Add to `tests/unit/test_mail_rules.c`:
- Rule `if-body = *unsubscribe*` matches body "please unsubscribe here"
- Same rule does NOT match body "hello world"
- `NULL` body → rule with `if-body` does NOT match (conservative)

## Functional tests (Phase 43)

Synthetic TB profile:
```
name="Body newsletter"
condition="AND (body,contains,unsubscribe)"
action="Move to folder"
actionValue="imap://user@server/Newsletter"
```

Expected checks:
- `43.1` "Body newsletter" → `if-body = *unsubscribe*`
- `43.2` No `[warn]` for `body` condition

## Definition of done

Unit tests pass, Phase 43 checks pass, no regression in Phase 38 (which tests
the warn-before-implementation state — adjust or replace check 38.5 once
body conversion is implemented).
