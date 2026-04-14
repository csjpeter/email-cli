# US-21 — Multi-Account Support

**As a** user who has more than one email account,
**I want to** manage and switch between multiple IMAP accounts within a single TUI session,
**so that** I can read, compose, and reply without leaving the application or editing config files.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Launching `email-tui` always shows the **Accounts screen** first. |
| 2 | The Accounts screen lists every configured account (email address + IMAP host). |
| 3 | Pressing **Enter** on an account opens its inbox (message list). |
| 4 | Pressing **n** starts the **Setup Wizard** to add a new account; the wizard saves to `~/.config/email-cli/accounts/<email>/config.ini`. |
| 5 | Pressing **d** deletes the selected account's profile directory (non-legacy accounts only). |
| 6 | Pressing **e** opens the SMTP wizard for the selected account and saves the result. |
| 7 | Pressing **Backspace** from the folder-root level returns to the Accounts screen. |
| 8 | Each account's IMAP local-store is isolated (`email-cli` re-initialises the store when switching). |
| 9 | Pressing **h** or **?** shows the accounts keyboard shortcuts overlay. |

---

## Navigation hierarchy

```
Accounts screen  (ESC/q → quit)
    └─ Folder browser  (Backspace at root → Accounts)
           └─ Message list  (Backspace → Folder browser)
                  └─ Message reader  (Backspace → Message list)
```

---

## Storage layout

```
~/.config/email-cli/
  config.ini                      ← legacy / last-used account (CLI tools)
  accounts/
    alice@example.com/config.ini  ← named profile
    bob@work.example/config.ini
```

`config_list_accounts()` returns the legacy entry first (if present and not a
duplicate), followed by all named profiles found in `accounts/`.

---

## Key bindings — Accounts screen

| Key | Action |
|-----|--------|
| ↑ / ↓ | Move cursor |
| Enter | Open selected account |
| n | Add new account (runs Setup Wizard) |
| d | Delete selected account |
| e | Edit SMTP settings for selected account |
| ESC / q / Backspace | Quit |
| h / ? | Show help overlay |

---

## Implementation notes

* `email_service_account_interactive(Config **cfg_out)` returns:
  * `0` = quit
  * `1` = open account (`*cfg_out` set, caller must `config_free`)
  * `2` = edit SMTP (`*cfg_out` set)
  * `3` = add new account (`*cfg_out` NULL)
* `main_tui.c` owns the outer `for(;;)` loop; `sel_cfg` is freed at the end of
  each account session.
* The legacy `config.ini` is still used by `email-cli` and `email-sync` CLI
  tools (single-account commands).
