# US-24 — Account Configuration Management

**As a** user managing one or more email accounts,
**I want** to reconfigure both IMAP and SMTP settings for any account without
losing other settings,
**so that** I can update server details, credentials, or folders at any time
from the CLI or the TUI.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `email-cli config show` prints the current account's IMAP and SMTP settings; passwords are masked as `****`. |
| 2 | `email-cli config smtp` reconfigures SMTP settings interactively using the setup wizard; only SMTP fields are changed. |
| 3 | `email-cli config imap` reconfigures IMAP settings interactively using the setup wizard; only IMAP fields are changed. |
| 4 | With multiple accounts, `--account <email>` selects which account to configure; the flag is accepted before or after `config`. |
| 5 | Without `--account` and with multiple accounts configured, the CLI prints a list of account names with instructions to re-run with `--account <email>` and exits non-zero. |
| 6 | In the TUI accounts screen, `e` opens the SMTP sub-wizard for the selected account; `i` opens the IMAP sub-wizard. |
| 7 | Changes are saved to the account's config file immediately after the wizard completes; a confirmation message is printed. |

---

## CLI interface

```
email-cli [--account <email>] config show
email-cli [--account <email>] config imap
email-cli [--account <email>] config smtp
email-cli [--account <email>] config --help
```

### `config show` output format

```
email-cli configuration (user@example.com):

  IMAP:
    Host:     imaps://imap.example.com
    User:     user@example.com
    Password: ****
    Folder:   INBOX

  SMTP:
    Host:     smtps://smtp.example.com
    Port:     465
    User:     user@example.com
    Password: ****
```

If SMTP is not yet configured:

```
  SMTP:
    (not configured — will be derived from IMAP host)
```

### Multiple-account guard (no `--account`)

```
Multiple accounts configured. Re-run with --account <email>:
  user1@example.com
  user2@example.org
```

---

## TUI accounts screen keys (updated)

| Key | Action |
|-----|--------|
| `i` | Open IMAP setup wizard for selected account |
| `e` | Open SMTP setup wizard for selected account |

The status bar shows: `↑↓=select  Enter=open  n=add  d=delete  i=IMAP  e=SMTP  ESC=quit`

---

## Implementation notes

* `setup_wizard_imap(cfg)` and `setup_wizard_smtp(cfg)` each update only their
  respective fields in the `Config` struct, then the caller persists the updated
  config with `config_save_to_store(cfg)`.
* After either wizard completes successfully, a confirmation line is printed:
  `IMAP configuration saved.` or `SMTP configuration saved.`
* `email_service_account_interactive()` in `email_service.c` returns `4` when
  `i` is pressed (IMAP edit) and `2` when `e` is pressed (SMTP edit);
  `main_tui.c` dispatches accordingly.
* `help_config()` in `main.c` documents `--account`, `show`, `imap`, and `smtp`.
