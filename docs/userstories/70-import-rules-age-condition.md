# US-70 — Mail Rules: Age / Date Condition

**As a** user who wants to automate management of old messages,
**I want to** write rules based on message age,
**so that** messages older than a threshold are automatically archived, labelled, or deleted.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `if-age-gt = N` matches messages older than N days. |
| 2 | `if-age-lt = N` matches messages newer than N days. |
| 3 | `mail_rules_apply()` accepts a `time_t message_date` parameter for age evaluation. |
| 4 | `email-cli rules apply` reads the `Date` header from the manifest (or .hdr) and computes age in days. |
| 5 | `email-sync --verbose` evaluates `if-age-gt` / `if-age-lt` during sync using the message's `Date` header. |
| 6 | `email-import-rules` converts `age,greater than,N` → `if-age-gt = N`. |
| 7 | `email-import-rules` converts `age,less than,N` → `if-age-lt = N`. |
| 8 | No `[warn]` is emitted for `age` condition field once this story is implemented. |
| 9 | `email-cli rules add` accepts `--if-age-gt <days>` and `--if-age-lt <days>`. |
| 10 | `email-cli rules list` displays `if-age-gt` / `if-age-lt` conditions. |

---

## rules.ini syntax

```ini
[rule "Archive old"]
if-age-gt      = 90
then-move-folder = Archive

[rule "Delete very old"]
if-age-gt      = 365
then-add-label = _trash

[rule "Recent only"]
if-age-lt      = 7
if-from        = *@news.example.com*
then-add-label = Recent
```

---

## Conversion table

| Thunderbird | → | rules.ini |
|---|---|---|
| `age,greater than,30` | | `if-age-gt = 30` |
| `age,less than,7` | | `if-age-lt = 7` |

---

## Implementation notes

**`mail_rules.h`**: add `int if_age_gt` and `int if_age_lt` to `MailRule` (0 = disabled).

**`mail_rules.c`**: `mail_rules_apply()` gains `time_t message_date`:
```c
int mail_rules_apply(..., time_t message_date, ...);
```
Age in days: `(time(NULL) - message_date) / 86400`.

**`email_service.c`**: parse the `Date` field from the manifest entry
using `mime_parse_date()` (or a new helper) and pass `time_t` to `mail_rules_apply`.

**`mail_rules_load()` / `mail_rules_save()`**: parse and emit `if-age-gt` / `if-age-lt`.

---

## Related

* US-64: warn on unsupported elements
* US-69: body condition (similar API extension pattern)
* US-59: rules apply
