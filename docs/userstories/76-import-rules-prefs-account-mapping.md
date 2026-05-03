# US-76 — Import Rules: Per-Account Thunderbird Directory Mapping via prefs.js

**As a** user importing Thunderbird filters for multiple accounts,
**I want** `email-import-rules` to read Thunderbird's `prefs.js` to determine
which ImapMail subdirectory belongs to each of my accounts,
**so that** each account receives only its own rules — even when multiple accounts
share the same IMAP hostname (e.g. two Gmail accounts on `imap.gmail.com`).

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `email-import-rules` reads `<thunderbird-path>/prefs.js` to map each `mail.server.serverN` entry to a `(hostname, userName, directory-rel)` triple. |
| 2 | In single-account mode (`--account <email>`), the matching ImapMail subdirectory is resolved via prefs.js; if no entry matches, an error is printed and the command exits non-zero. |
| 3 | In multi-account mode (no `--account`), each configured email-cli account is matched independently to its own prefs.js entry. |
| 4 | Two accounts on the same hostname (e.g. two Gmail accounts) are distinguished by their `userName` field; each account gets only the rules from its own subdirectory. |
| 5 | An account that has no matching prefs.js entry is reported as "No matching Thunderbird account found — skipping" and no `rules.ini` is written for it. |
| 6 | Skipping an account without a match is not treated as an error; the command succeeds as long as at least one account is processed. |

---

## Notes

Thunderbird stores its account→directory mapping in `prefs.js` entries of the form:

```
user_pref("mail.server.server1.hostname",      "imap.gmail.com");
user_pref("mail.server.server1.userName",      "user@gmail.com");
user_pref("mail.server.server1.directory-rel", "[ProfD]ImapMail/imap.gmail.com");
user_pref("mail.server.server1.type",          "imap");
```

When Thunderbird adds a second account on the same host (e.g. a second Gmail),
it names the subdirectory `imap.gmail-2.com` (inserts `-N` before the TLD).
Without reading `prefs.js`, pattern-based directory scanning would merge rules
from both accounts into every matched account — the root cause of the
"all accounts get all rules" bug reported before this story.

The `directory-rel` value uses a `[ProfD]ImapMail/` prefix; the importer
strips this prefix to obtain the raw subdirectory name.

---

## Related

* US-64: warn infrastructure
* US-75: additional action support
* TASK-011: implementation of US-75 (includes dangling-pointer fix)

