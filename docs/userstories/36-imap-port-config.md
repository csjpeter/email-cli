# US-36 — IMAP Port Configuration

**As a** user with an IMAP server on a non-standard port,
**I want** the setup wizard to ask for the IMAP port,
**so that** I can connect to servers that don't use the default port 993.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The setup wizard prompts for `IMAP Port [993]` after the IMAP host. |
| 2 | Pressing Enter without input uses the default port 993 (no change to the URL). |
| 3 | Entering `993` explicitly has the same effect as pressing Enter (no `:993` appended). |
| 4 | Entering a different port (e.g. `10993`) appends `:10993` to the `imaps://` URL. |
| 5 | The port is only appended if the URL does not already contain a port (e.g. user typed `imap.example.com:995` as the host). |
| 6 | The IMAP sub-wizard (`config imap`) also supports port configuration. |
| 7 | Invalid port input (non-numeric, zero, negative) is treated as default (993). |

---

## Examples

| Host input | Port input | Resulting URL |
|-----------|-----------|---------------|
| `imap.example.com` | _(Enter)_ | `imaps://imap.example.com` |
| `imap.example.com` | `993` | `imaps://imap.example.com` |
| `imap.example.com` | `10993` | `imaps://imap.example.com:10993` |
| `imaps://imap.example.com` | _(Enter)_ | `imaps://imap.example.com` |
| `imap.example.com:995` | _(Enter)_ | `imaps://imap.example.com:995` |

---

## Implementation notes

* `setup_wizard.c`: port prompt is after host normalization. If port != 993
  and URL has no `:port` suffix, `asprintf` appends it.
* No new `Config` field — port is embedded in the `host` URL string.
* SMTP already has a separate port prompt (`SMTP Port [587]`).
