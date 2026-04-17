# US-27 — Gmail Account Setup

**As a** Gmail user,
**I want to** set up my Gmail account via an OAuth2 device-flow wizard,
**so that** I can use email-cli with my Google account without entering an IMAP password.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The setup wizard offers account type selection: `[1] IMAP` / `[2] Gmail`. |
| 2 | Selecting Gmail prompts only for the email address (no host/password fields). |
| 3 | The wizard initiates an OAuth2 device authorization grant (RFC 8628) and displays a verification URL and user code. |
| 4 | The wizard polls for authorization completion and displays a success/failure message. |
| 5 | On success, `GMAIL_MODE=1`, `EMAIL_USER`, and `GMAIL_REFRESH_TOKEN` are saved to `~/.config/email-cli/accounts/<email>/config.ini` with mode `0600`. |
| 6 | No `EMAIL_HOST`, `EMAIL_PASS`, or `SMTP_*` fields are written for Gmail accounts. |
| 7 | If the user cancels (`^C`) during the device flow, no partial config is saved. |
| 8 | Built-in OAuth2 `client_id` / `client_secret` are compiled into the binary; users may override via config fields `GMAIL_CLIENT_ID` / `GMAIL_CLIENT_SECRET`. |
| 9 | If a refresh token expires or is revoked (`invalid_grant`), `gmail_connect()` auto-triggers the device flow for reauthorization. |

---

## OAuth2 Device Flow

```
Account type:
  [1] IMAP (standard e-mail server)
  [2] Gmail (Google account — uses Gmail API, not IMAP)
Choice [1]: 2

Email address: user@gmail.com

Opening Gmail authorization...
  Go to: https://accounts.google.com/device
  Enter code: XXXX-XXXX
  (Waiting for authorization... ^C to cancel)

Authorization successful. Configuration saved.
```

### Steps

1. POST `https://oauth2.googleapis.com/device/code` with `client_id` and
   `scope=https://www.googleapis.com/auth/gmail`.
2. Display `verification_url` and `user_code` to user.
3. Poll `https://oauth2.googleapis.com/token` every `interval` seconds until
   `access_token` + `refresh_token` are returned or `expires_in` is exceeded.
4. Store `refresh_token` in config (persisted); hold `access_token` in memory
   only (expires ~3600s).

---

## Error handling

| Error | Behaviour |
|-------|-----------|
| `invalid_grant` | Auto-trigger reauthorization device flow |
| `invalid_client` | Display: `"OAuth2 client credentials are invalid."` |
| Network error during poll | Retry silently until `expires_in` timeout |
| User cancels (`^C`) | Return to account setup; no config saved |
| Poll timeout | Display: `"Authorization timed out."` |

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
* `gmail_auth.c` implements `gmail_device_flow()` and `gmail_refresh_token()`.
* Token refresh happens automatically on every `gmail_connect()` call.

---

## Related

* Spec: `docs/spec/gmail-api.md` sections 1, 2, 11
* GML milestones: GML-05, GML-12
