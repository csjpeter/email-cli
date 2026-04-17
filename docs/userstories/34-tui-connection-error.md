# US-34 — TUI Connection Error Handling

**As a** user with multiple email accounts,
**I want** the TUI to return to the accounts screen when a connection fails,
**so that** I can switch to another account instead of being dropped to the shell.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | When `email_service_list` fails (connection error, auth failure, etc.), the TUI shows an error message instead of exiting. |
| 2 | The error message is displayed below the last screen content with `Connection failed. Press any key to return to accounts...`. |
| 3 | After pressing any key, the user returns to the accounts screen with cursor position preserved. |
| 4 | The accounts screen is fully functional after returning from an error (can open other accounts, add new ones, quit). |
| 5 | The TUI never exits with a non-zero exit code due to a single account connection failure. |
| 6 | If ALL operations fail (the user explicitly presses ESC/q), the TUI exits with code 0 (normal quit). |

---

## Error scenarios

| Scenario | Previous behaviour | New behaviour |
|----------|-------------------|---------------|
| IMAP login fails (bad password) | Exit to shell with error | Show error, return to accounts |
| IMAP server unreachable | Exit to shell with error | Show error, return to accounts |
| Gmail API auth failure | Exit to shell with error | Show error, return to accounts |
| Folder select fails | Exit to shell with error | Show error, return to accounts |

---

## Implementation notes

* `main_tui.c`: when `email_service_list` returns -1, set `back_to_accounts = 1`
  and break from the inner loop instead of the outer loop.
* Error message is shown in raw terminal mode with a "press any key" prompt to
  ensure the user sees it before the screen redraws.
