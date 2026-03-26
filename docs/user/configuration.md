# Configuration Reference

## Config File Location

```
~/.config/email-cli/config.ini
```

The file is created automatically by the setup wizard on first run.
Permissions are set to `0600` (owner read/write only) to protect the password.

## Format

Plain key=value, one entry per line, no sections:

```ini
EMAIL_HOST=imaps://imap.gmail.com
EMAIL_USER=you@gmail.com
EMAIL_PASS=yourpassword
EMAIL_FOLDER=INBOX
SSL_NO_VERIFY=0
```

## Keys

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `EMAIL_HOST` | yes | — | IMAP server URL. Use `imaps://` for TLS (recommended), `imap://` for plain. |
| `EMAIL_USER` | yes | — | IMAP login username (usually your full email address). |
| `EMAIL_PASS` | yes | — | IMAP password or app-specific password. Stored in plaintext — keep the file private. |
| `EMAIL_FOLDER` | no | `INBOX` | Mailbox folder to read from. |
| `SSL_NO_VERIFY` | no | `0` | Set to `1` to skip TLS certificate verification. **Use only for self-signed certs in private/test environments.** |

## TLS Behaviour

- `imaps://` — TLS is required. Minimum version: TLS 1.2 (enforced by libcurl).
- `imap://` — Plain connection. Avoid for production use.
- `SSL_NO_VERIFY=1` — Disables certificate and hostname verification. The connection
  is still encrypted, but a man-in-the-middle is not detected.

## Re-running the Wizard

To reconfigure, delete the config file and run again:

```bash
rm ~/.config/email-cli/config.ini
./bin/email-cli
```

Or edit the file directly with any text editor.

## Multiple Accounts

`email-cli` currently supports one account at a time. To switch accounts,
edit the config file or replace it entirely.
