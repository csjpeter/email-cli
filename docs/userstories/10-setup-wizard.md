# User Story: Setup Wizard

## Summary
As a new user, I want an interactive setup wizard to guide me through configuring
email-cli when no configuration file exists yet.

## Trigger
Automatically launched on first run when no named account profile exists yet
(i.e. `~/.config/email-cli/accounts/` is empty or missing).

## Host normalisation rules

Both IMAP and SMTP host inputs follow the same rules:

| User input | Behaviour |
|------------|-----------|
| Plain hostname, e.g. `imap.example.com` | Protocol auto-completed: `imaps://imap.example.com` is used and confirmed on screen |
| `imaps://imap.example.com` | Accepted as-is |
| `imap://imap.example.com` | Rejected with error; user prompted to re-enter |
| Any other explicit `xxx://` prefix | Rejected with error; user prompted to re-enter |

The same rule applies to SMTP: plain hostname → `smtps://` prepended;
`smtps://…` accepted; anything else with `://` rejected.

An invalid or unsupported configuration is **never saved**.

## Flow
1. The wizard prompts for:
   - IMAP host (plain hostname or `imaps://` URL)
   - IMAP username
   - IMAP password (input hidden)
   - Default folder (default: `INBOX`)
2. Optional SMTP section (Enter to skip each field):
   - SMTP host (plain hostname or `smtps://` URL; default derived from IMAP host)
   - SMTP port (default: 587)
   - SMTP username (default: same as IMAP)
   - SMTP password (default: same as IMAP)
3. The configuration is saved to
   `~/.config/email-cli/accounts/<imap-user>/config.ini` with mode `0600`.
4. email-cli continues with the requested command after wizard completion.

## Example Interaction
```
IMAP Host (e.g. imap.example.com): imap.example.com
  → using imaps://imap.example.com
Email Username: user@example.com
Email Password: ********
  → password updated (8 characters)
Default Folder [INBOX]:

--- SMTP (outgoing mail) — press Enter to skip ---
SMTP Host [Enter = smtps://imap.example.com] (e.g. smtp.example.com):
SMTP Port [587]:
...

Configuration collected.
Configuration saved. Run 'email-cli sync' to download your mail.
Checking the connection to imaps://imap.example.com...
Connected: user@example.com accepted by imaps://imap.example.com.
```

The connection check is made by the caller after the config has been saved —
for a long time this line promised a check that nothing performed.

A Gmail address does not belong in the IMAP host field: account type **[2]
Gmail** uses the Gmail API, and an IMAP host under gmail.com is rejected.

## Changing only the password later

`email-cli config password` asks for the password alone and then tests it; a
Gmail account is sent to `email-cli config reauth`, which renews the OAuth2
authorization that a Google password change revokes.

## Re-entry on bad protocol
```
IMAP Host (e.g. imap.example.com): imap://imap.example.com
Error: 'imap://imap.example.com' uses an unsupported protocol (only imaps:// is supported).
IMAP Host (e.g. imap.example.com):
```

## Exit Behaviour
- If the user aborts (Ctrl-D / EOF), the wizard exits without saving.
- A config with a bad/unsupported host protocol is never written to disk.

## Exit Codes
- `0` configuration saved
- `1` user aborted
