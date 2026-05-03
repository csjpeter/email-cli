# US-69 — Mail Rules: Body Condition (if-body)

**As a** user who wants to filter messages by content,
**I want to** write rules that match against the message body text,
**so that** I can sort messages that don't have consistent From or Subject patterns.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `if-body` is a new valid field in `rules.ini`, accepting a glob pattern. |
| 2 | `mail_rules_apply()` accepts an additional `body` parameter and evaluates `if-body` against it. |
| 3 | `email-cli rules apply` loads the full `.eml` file for each message and passes the decoded plain-text body. |
| 4 | `email-sync --verbose` evaluates `if-body` using the downloaded message body during sync. |
| 5 | `email-import-rules` converts `body,contains,X` → `if-body = *X*`. |
| 6 | `email-import-rules` converts `body,begins with,X` → `if-body = X*`. |
| 7 | No `[warn]` is emitted for `body` condition field once this story is implemented. |
| 8 | `email-cli rules add` accepts `--if-body <glob>`. |
| 9 | `email-cli rules list` displays `if-body` conditions. |
| 10 | Body matching is case-insensitive. |

---

## rules.ini syntax

```ini
[rule "Newsletters by body"]
if-body        = *unsubscribe*
then-add-label = Newsletter

[rule "Meeting invites"]
if-body        = *You have been invited*
then-add-label = Meeting
```

---

## Implementation notes

**`mail_rules.h`**: add `char *if_body` to `MailRule`.

**`mail_rules.c`**: `mail_rules_apply()` signature gains `const char *body`:
```c
int mail_rules_apply(const MailRules *rules,
                     const char *from, const char *subject,
                     const char *to, const char *labels_csv,
                     const char *body,          /* NEW */
                     char ***add_out, int *add_count,
                     char ***rm_out,  int *rm_count);
```

**`email_service.c`** (IMAP apply_rules): for each message, load `.eml` from local cache,
extract plain-text body with `mime_decode_body()`, pass to `mail_rules_apply`.
This is slower than manifest-based matching — document the performance trade-off.

**Performance note**: body matching during `rules apply` requires loading full `.eml`
files. For large local stores this may be slow. Consider a `--no-body-match` flag
to skip `if-body` rules in apply mode.

---

## Related

* US-64: warn on unsupported elements
* US-59: rules apply
* US-68: negation conditions
