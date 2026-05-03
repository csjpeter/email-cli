# US-61 — Mail Rules: Add and Remove Rules

**As a** user who wants to maintain sorting rules from the command line,
**I want to** add new rules and remove existing ones without manually editing the INI file,
**so that** I can manage my rules safely with proper validation.

---

## Acceptance criteria

### rules add

| # | Criterion |
|---|-----------|
| 1 | `email-cli rules add --name <name>` adds a new rule to the account's `rules.ini`. |
| 2 | At least one condition or one action must be specified; missing both prints an error. |
| 3 | `--if-from`, `--if-subject`, `--if-to`, `--if-label` set the respective glob conditions. |
| 4 | `--add-label <label>` and `--remove-label <label>` add the respective actions (repeatable). |
| 5 | `--move-folder <folder>` sets the IMAP move-to-folder action. |
| 6 | Adding a rule with a name that already exists prints an error and exits non-zero. |
| 7 | `--name` is required; omitting it prints an error and exits non-zero. |
| 8 | `email-cli rules add --help` shows the full usage page. |

### rules remove

| # | Criterion |
|---|-----------|
| 9 | `email-cli rules remove --name <name>` removes the named rule from `rules.ini`. |
| 10 | If the named rule does not exist, an error is printed and the command exits non-zero. |
| 11 | `--name` is required; omitting it prints an error and exits non-zero. |
| 12 | `email-cli rules remove --help` shows the full usage page. |

---

## Example usage

```bash
email-cli rules add --name "GitHub" \
    --if-from "*@github.com" \
    --add-label GitHub \
    --remove-label INBOX

email-cli rules remove --name "GitHub"
```

---

## Related

* US-57: rules list
* US-59: rules apply
* US-62: TUI rules editor
