# Credential Obfuscation

## Overview

email-cli obfuscates stored passwords and tokens by default. Rather than storing
credentials in plain text, they are encrypted with a key derived from stable
system-specific data that is unique to the installation.

This means:
- Config files copied to another machine cannot be decrypted there.
- Casual inspection of config files (e.g. by an AI agent browsing your home
  directory, or someone with brief physical access) will not reveal your passwords.
- No master password is required — the program derives the key automatically on
  each run.

## Important: When Obfuscation Can Break

The key is derived from data that is normally stable but may change in exceptional
circumstances (such as reinstalling the OS, generating a new SSH key, or certain
hardware changes). **If the key source data changes, email-cli will no longer be
able to decrypt the stored credentials.** In that case, re-enter your passwords
using `email-cli config password` (IMAP) or re-run the OAuth2 setup
(`email-cli add-account`) for Gmail accounts.

## Stored Format

When obfuscation is enabled, credential fields in
`~/.config/email-cli/accounts/<email>/config.ini` look like:

```ini
EMAIL_PASS=enc:aGVsbG8gd29ybGQ...
```

The `enc:` prefix indicates that the value is encrypted. The rest is a base64-encoded
blob containing a random IV, the ciphertext, and an authentication tag. Plaintext
values (no prefix) are always accepted on load for backward compatibility.

## Disabling Obfuscation

To store credentials as plain text, write the drop-in
`~/.config/email-cli/settings.d/99-operator.conf` (the only file an editor of
this project writes, atomically, holding just the keys that differ from the
shipped value; it is removed when nothing differs) with:

```ini
credential_obfuscation=false
```

Then run:

```
email-cli migrate-credentials
```

This re-saves all account config files using the new setting. Until you run
`migrate-credentials`, existing `enc:` values are still readable (decryption
happens transparently on load).

## Re-enabling Obfuscation

Set `credential_obfuscation=true` (or remove the line; 1 is the default) and run:

```
email-cli migrate-credentials
```

## Toggling Summary

| Goal | 99-operator.conf | Command |
|------|-------------|---------|
| Encrypt existing plaintext configs | `credential_obfuscation=true` (default) | `email-cli migrate-credentials` |
| Decrypt to plaintext | `credential_obfuscation=false` | `email-cli migrate-credentials` |
| Check current setting | — | `email-cli config show` (names the source: shipped, a drop-in, operator) |

## migrate-credentials

```
email-cli migrate-credentials
```

Loads all configured accounts and re-saves them using the current obfuscation
setting. The command is idempotent — running it multiple times is safe.

## Scope of Protection

Obfuscation protects credentials from:

- Casual reading of config files by other programs or scripts.
- AI agents browsing your home directory.
- Brief physical access to an unlocked machine (the config file is not
  immediately human-readable).
- Config files that end up in backups or are accidentally shared.

Obfuscation does **not** protect against:

- A determined local attacker with full access to your user session.
- Malware running as your user.

For the strongest protection, combine obfuscation with full-disk encryption and
a locked screen when away from the machine.

## Settings drop-ins (EMAIL-294)

- The shipped value is built in (`credential_obfuscation=true`).
- `~/.config/email-cli/settings.d/*.conf` override it in name order; another
  program writes a drop-in of its own name (e.g. `50-infra-mgr.conf`), never
  `99-operator.conf`.  An unknown key in a drop-in is reported on stderr.
- Setting a key to the value the shipped file and the other drop-ins already
  give removes its line; an empty `99-operator.conf` is removed.
- A legacy `~/.config/email-cli/settings.ini` is read once, its value moved into
  `99-operator.conf` (when it differs) and the old file removed.
