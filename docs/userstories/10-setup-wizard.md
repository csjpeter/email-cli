# User Story: Setup Wizard

## Summary
As a new user, I want an interactive setup wizard to guide me through configuring
email-cli when no configuration file exists yet.

## Trigger
Automatically launched on first run when `~/.config/email-cli/config.ini` does not exist.

## Flow
1. The wizard prompts for:
   - IMAP host URL (e.g. `imaps://imap.example.com`)
   - IMAP username
   - IMAP password (input hidden)
   - Default folder (e.g. `INBOX`)
2. The entered configuration is validated (optionally by attempting a connection).
3. The configuration is saved to `~/.config/email-cli/config.ini` with mode `0600`.
4. email-cli continues with the requested command after wizard completion.

## Example Interaction
```
Welcome to email-cli setup.

IMAP host (e.g. imaps://imap.example.com): imaps://imap.gmail.com
Username: user@gmail.com
Password: ********
Default folder [INBOX]:

Configuration saved. Run 'email-cli sync' to download your mail.
```

## Exit Behaviour
- If the user aborts (Ctrl-C or empty host), the wizard exits with an error message.

## Exit Codes
- `0` configuration saved
- `1` user aborted
