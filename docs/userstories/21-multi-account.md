# US-21 — Multi-Account Support

**As a** user who has more than one email account,
**I want to** manage and switch between multiple IMAP accounts within a single TUI session,
**so that** I can read, compose, and reply without leaving the application or editing config files.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Launching `email-tui` always shows the **Accounts screen** first. |
| 2 | The Accounts screen lists every configured account with **Unread**, **Flagged**, **Account** and **Server** columns. |
| 3 | Pressing **Enter** on an account opens its inbox (message list). |
| 4 | Pressing **n** starts the **Setup Wizard** to add a new account; the wizard saves to `~/.config/email-cli/accounts/<email>/config.ini`. |
| 5 | Pressing **d** deletes the selected account's profile directory (non-legacy accounts only). |
| 6 | Pressing **e** opens the SMTP wizard for the selected account and saves the result. |
| 7 | Pressing **Backspace** from the folder-root level returns to the Accounts screen. |
| 8 | Each account's IMAP local-store is isolated (`email-tui` re-initialises the store when switching). |
| 9 | Pressing **h** or **?** shows the accounts keyboard shortcuts overlay. |
| 10 | **Backspace** on the Accounts screen itself is ignored (does not quit). Only **ESC** / **q** / Ctrl-C exit the program from the top level. |
| 11 | When returning to the Accounts screen (after navigating into an account and back), the cursor is positioned on the account that was previously open, not on the first one. |
| 12 | The **Unread** and **Flagged** counts are read from local manifests (no network required) and reflect the totals across all folders for each account. |

---

## Navigation hierarchy

```
Accounts screen  (ESC/q → quit; Backspace → ignored)
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

`config_list_accounts()` returns named profiles sorted alphabetically.

---

## Key bindings — Accounts screen

| Key | Action |
|-----|--------|
| ↑ / ↓ | Move cursor |
| Enter | Open selected account |
| n | Add new account (runs Setup Wizard) |
| d | Delete selected account |
| i | Edit IMAP settings for selected account |
| e | Edit SMTP settings for selected account |
| ESC / q | Quit |
| Backspace | Ignored (no action at top level) |
| h / ? | Show help overlay |

---

## Implementation notes

* `email_service_account_interactive(Config **cfg_out, int *cursor_inout)` returns:
  * `0` = quit
  * `1` = open account (`*cfg_out` set, caller must `config_free`)
  * `2` = edit SMTP (`*cfg_out` set)
  * `3` = add new account (`*cfg_out` NULL)
  * `4` = edit IMAP (`*cfg_out` set)
* `cursor_inout` is an in/out parameter: on entry the cursor is placed at this
  index; on any return the current cursor index is written back.  `main_tui.c`
  keeps an `account_cursor` variable across calls so position is restored.
* Unread/Flagged counts are computed by `get_account_totals()`: calls
  `local_store_init` + `local_folder_list_load` + `manifest_count_folder` for
  each account.  No network connection is required.
* `main_tui.c` owns the outer `for(;;)` loop; `sel_cfg` is freed at the end of
  each account session.
