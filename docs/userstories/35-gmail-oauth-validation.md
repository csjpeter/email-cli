# US-35 — Gmail OAuth Credential Validation

**As a** user adding a Gmail account,
**I want** clear guidance when OAuth2 credentials are missing,
**so that** I know exactly what to configure and where to find help.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | When opening a Gmail account in the TUI without OAuth2 credentials (no `GMAIL_CLIENT_ID` in config and no compiled-in default), an informative message is shown before any connection attempt. |
| 2 | The message includes the config file path (`~/.config/email-cli/accounts/<email>/config.ini`). |
| 3 | The message references the setup guide (`docs/dev/gmail-oauth2-setup.md`). |
| 4 | After dismissing the message (any key), the user returns to the accounts screen. |
| 5 | No IMAP connection is attempted for Gmail accounts missing OAuth credentials. |
| 6 | In the setup wizard (new account flow): if no compiled-in credentials exist, the wizard interactively prompts for `GMAIL_CLIENT_ID` and `GMAIL_CLIENT_SECRET` before starting the OAuth device flow. |
| 7 | If the user cancels the credential prompt (empty input), the wizard aborts cleanly (no partial config saved). |
| 8 | Credentials entered in the wizard are saved to the account's `config.ini` and reused on subsequent runs. |

---

## Validation points

| Location | Check | Action if missing |
|----------|-------|-------------------|
| Setup wizard (new account) | No compiled-in `GMAIL_DEFAULT_CLIENT_ID` and no `cfg->gmail_client_id` | Prompt for client_id and client_secret interactively |
| TUI account open | `gmail_mode=1` but no `gmail_refresh_token` and no `gmail_client_id` | Show warning with config path and guide reference, return to accounts |
| `gmail_auth_device_flow()` | `client_id` is empty string | Show detailed error explaining both config.ini and setup guide |

---

## Implementation notes

* `setup_wizard.c`: checks `GMAIL_DEFAULT_CLIENT_ID` macro and `cfg->gmail_client_id`
  before calling `gmail_auth_device_flow()`.
* `main_tui.c`: pre-flight check before entering folder/label browser.
* `gmail_auth.c`: last-resort error with full explanation if credentials are still missing.
