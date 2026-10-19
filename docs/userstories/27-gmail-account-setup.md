# US-27 — Gmail Account Setup

**As a** Gmail user,
**I want to** set up my Gmail account via an OAuth2 authorization wizard,
**so that** I can use email-cli with my Google account without entering an IMAP password.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The setup wizard offers account type selection: `[1] IMAP` / `[2] Gmail`. |
| 2 | Selecting Gmail prompts only for the email address (no host/password fields). |
| 3 | The wizard starts an OAuth2 authorization (authorization code, loopback redirect; Google's device flow does not admit the Gmail scope), shows the URL and opens a browser. |
| 4 | The wizard waits for the redirect and displays a success/failure message. On a machine without a browser, `config reauth --no-browser` (SSH forward), `--paste` and `--import-token` obtain the token (EMAIL-113). |
| 5 | On success, `GMAIL_MODE=1`, `EMAIL_USER`, and `GMAIL_REFRESH_TOKEN` are saved to `~/.config/email-cli/accounts/<email>/config.ini` with mode `0600`. |
| 6 | No `EMAIL_HOST`, `EMAIL_PASS`, or `SMTP_*` fields are written for Gmail accounts. |
| 7 | If the user cancels (`^C`) during the authorization, no partial config is saved. |
| 8 | Built-in OAuth2 `client_id` / `client_secret` are compiled into the binary; users may override via config fields `GMAIL_CLIENT_ID` / `GMAIL_CLIENT_SECRET`. |
| 9 | If a refresh token expires or is revoked (`invalid_grant`), the failure names `email-cli config reauth`; the flow is not started automatically. |
| 10 | In `email-tui`, `:` on the accounts screen with a Gmail account under the cursor opens its action menu; "Renew the authorization..." asks for the way (`b` browser, `l` address only, `p` paste), runs the flow of `config reauth` on the plain terminal, announces the wait (`^C` cancels it), and on success stores the token and tests it. A refusal or a cancel leaves the account unchanged (EMAIL-130). |

---

## OAuth2 authorization

```
Account type:
  [1] IMAP (standard e-mail server)
  [2] Gmail (Google account — uses Gmail API, not IMAP)
Choice [1]: 2

Email address: user@gmail.com

Opening Gmail authorization...
  If the browser doesn't open, visit: https://accounts.google.com/o/oauth2/v2/auth?...
  (Waiting for authorization... ^C to cancel)

Authorization successful. Configuration saved.
```

### Steps

1. Build the authorization URL (`scope=https://mail.google.com/`,
   `redirect_uri=http://localhost:PORT/callback`) and show it.
2. Receive the code on the loopback listener (or pasted).
3. POST it to `https://oauth2.googleapis.com/token` for `access_token` +
   `refresh_token`.
4. Store `refresh_token` in config (persisted); hold `access_token` in memory
   only (expires ~3600s).

---

## Error handling

| Error | Behaviour |
|-------|-----------|
| `invalid_grant` | Report it, naming `email-cli config reauth` |
| `invalid_client` | Display: `"OAuth2 client credentials are invalid."` |
| User cancels (`^C`) | Return to account setup; no config saved |
| No redirect in 5 minutes | `Did not receive the authorization code (timed out)` |

---

## Config file (Gmail account)

```ini
GMAIL_MODE=1
EMAIL_USER=user@gmail.com
GMAIL_REFRESH_TOKEN=<long-lived token>
# Optional overrides:
# GMAIL_CLIENT_ID=...
# GMAIL_CLIENT_SECRET=...
```

---

## Implementation notes

* `setup_wizard.c` handles account type selection and branches to the
  Gmail-specific flow (email prompt + device flow) or the existing IMAP flow.
* `gmail_auth.c` implements `gmail_auth_authorize()` and `gmail_auth_refresh()`; `gmail_login.c` is the terminal side.
* Token refresh happens automatically on every `gmail_connect()` call.

---

## Related

* Spec: `docs/spec/gmail-api.md` sections 1, 2, 11
* GML milestones: GML-05, GML-12
