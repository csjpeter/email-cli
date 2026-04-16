[![CI](https://github.com/csjpeter/email-cli/actions/workflows/ci.yml/badge.svg)](https://github.com/csjpeter/email-cli/actions/workflows/ci.yml)
[![Valgrind](https://github.com/csjpeter/email-cli/actions/workflows/valgrind.yml/badge.svg)](https://github.com/csjpeter/email-cli/actions/workflows/valgrind.yml)
[![Coverage](https://csjpeter.github.io/email-cli/coverage-badge.svg)](https://csjpeter.github.io/email-cli/)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

# email-cli

A terminal-based IMAP email client suite written in C.
Provides read-only batch/TUI access for scripting and AI agents, and a full-featured
interactive TUI (`email-tui`) for human users that supports reading, flagging, composing,
and replying to messages.

---

> **Disclaimer:** This software is provided as-is, without warranty of any kind.
> The author accepts no responsibility for any damage, data loss, or unintended
> consequences resulting from the use or malfunction of this program.

---

## Table of Contents

- [Binaries](#binaries)
- [Security](#security)
- [Installation](#installation)
- [Configuration](#configuration)
- [Interactive Mode](#interactive-mode)
- [CLI Batch Mode](#cli-batch-mode)

---

## Binaries

| Binary | Write ops | Intended use |
|--------|-----------|--------------|
| `email-cli` | Send only | Interactive TUI and batch scripting; supports send but no compose UI |
| `email-tui` | Yes | Full-featured interactive TUI; compose, reply, send, flag |
| `email-cli-ro` | No | Truly read-only; no sync, no flag changes |
| `email-sync` | No | Standalone background sync daemon / cron helper |

All four binaries share the same configuration file and local message cache.

---

## Security

- **Read-only commands:** `email-cli` and `email-cli-ro` issue only `FETCH` (with
  `BODY.PEEK`, which never sets `\Seen`) and `SEARCH` IMAP commands. They never send,
  move, delete, or modify messages or flags on the server — not even the `\Seen` flag.
- **Write operations:** `email-tui` can set IMAP keyword flags (`n`/`f`/`d` keys) and
  send outgoing mail via SMTP. Write operations are confined to this binary.
- **Credentials at rest:** The configuration file is written with `0600` permissions,
  readable only by the owning user. Passwords are stored in plaintext — keep the file
  private.
- **Transport:** IMAP connections use `imaps://` (TLS 1.2+) by default. Plain `imap://`
  is supported for local testing only. `SSL_NO_VERIFY=1` disables certificate
  verification — never use it in production.
- **Local cache:** Fetched messages are stored in `~/.local/share/email-cli/` with
  directory permissions `0700`. No external service has access to the cache.
- **Logs:** Session diagnostics are written to `~/.cache/email-cli/logs/session.log`.
  Logs may include IMAP server responses; rotate or delete them as needed.
- **Memory safety:** The codebase is tested with AddressSanitizer and Valgrind on every
  CI run to eliminate memory leaks and buffer overflows. See the [developer docs](docs/)
  for details.

---

## Installation

```bash
./manage.sh deps    # Install system dependencies (Ubuntu 24.04 / Rocky 9)
./manage.sh build   # Build → bin/email-cli  bin/email-tui  bin/email-cli-ro  bin/email-sync
```

Run the interactive client:

```bash
bin/email-tui       # full-featured TUI (compose, reply, send)
bin/email-cli       # read + flag TUI (batch send, no compose)
```

---

## Configuration

### IMAP setup

On the first run, `email-tui` (or `email-cli`) starts an interactive setup wizard that
asks for:

- **IMAP server address** — e.g. `imaps://imap.example.com`
- **Username** — your email address
- **Password** — your IMAP password
- **Default folder** — e.g. `INBOX`

The configuration is saved to `~/.config/email-cli/config.ini` and reused on subsequent
runs.

To reconfigure, delete or edit that file manually:

```bash
rm ~/.config/email-cli/config.ini
bin/email-tui
```

### SMTP setup

To enable composing and sending mail in `email-tui`, configure SMTP settings either from
the CLI:

```bash
bin/email-cli config smtp
```

or by pressing `e` on the accounts screen inside `email-tui`. The wizard asks for:

- **SMTP server address** — e.g. `smtps://smtp.example.com`
- **Username** — usually your email address
- **Password** — your SMTP password (may differ from IMAP password)

SMTP credentials are appended to `~/.config/email-cli/config.ini` (mode `0600`).

> **One account at a time.** To switch accounts, replace or edit the config file.

### Provider Notes

| Provider | IMAP host | SMTP host | Notes |
|----------|-----------|-----------|-------|
| Gmail | `imaps://imap.gmail.com` | `smtps://smtp.gmail.com` | Enable IMAP in Gmail settings. If 2FA is on, use an [App Password](https://myaccount.google.com/apppasswords). |
| Outlook / Hotmail | `imaps://outlook.office365.com` | `smtps://smtp.office365.com` | Use your full email address as username. |
| Fastmail | `imaps://imap.fastmail.com` | `smtps://smtp.fastmail.com` | Standard credentials. |
| Self-hosted | `imaps://mail.yourdomain.com` | `smtps://mail.yourdomain.com` | For self-signed certificates set `SSL_NO_VERIFY=1` — development only. |

---

## Interactive Mode

When run without arguments in a terminal, `email-tui` and `email-cli` open a full-screen
interactive TUI. Navigation uses keyboard shortcuts — no mouse required.

### Accounts Screen (email-tui only)

`email-tui` opens at the accounts screen, which shows the configured IMAP account.

```
Email Account

  user@example.com  (imap.example.com)

  Enter=open  e=SMTP settings  ESC=quit
```

| Key | Action |
|-----|--------|
| `Enter` | Open the message list for the default folder |
| `e` | Open the SMTP setup wizard |
| `ESC` / Ctrl-C | Quit |

`email-cli` skips the accounts screen and opens the message list directly.

### Message List View

The default view on launch for `email-cli`, or reached via Enter on the accounts screen
in `email-tui`.

```
1-20 of 42 unread message(s) in INBOX.

  UID    From                  Subject               Date
  ═════  ════════════════════  ════════════════════  ════════════════
▶  1234  Alice <alice@ex.com>  Re: Meeting tomorrow  2026-03-28 09:14
   1230  Bob <bob@ex.com>      Invoice attached      2026-03-27 18:01
   ...

  ↑↓=step  PgDn/PgUp=page  Enter=open  s=sync  R=refresh  Backspace=folders  ESC=quit  [1/42]
```

#### Navigation keys

| Key | Action |
|-----|--------|
| `↑` / `↓` | Move cursor one row up/down |
| `PgDn` | Move cursor one page down |
| `PgUp` | Move cursor one page up |
| `Enter` | Open selected message |
| `Backspace` | Go to folder browser |
| `ESC` / Ctrl-C | Quit |

#### Flag keys (email-cli and email-tui)

| Key | Action |
|-----|--------|
| `n` | Mark selected message as new / unflag `\Seen` |
| `f` | Toggle `\Flagged` keyword flag |
| `d` | Mark as done (sets `done` IMAP keyword) |

#### Compose / reply keys (email-tui only)

| Key | Action |
|-----|--------|
| `c` | Open compose window for a new message |
| `r` | Open reply window for the selected message |

#### Background sync

| Key | Action |
|-----|--------|
| `s` | Start background sync (`email-sync` runs as a child process) |
| `R` | Refresh the message list from local cache |

While sync is running the count line shows `⟳ syncing...`. When the child process exits,
the next keypress shows `✉ New mail may have arrived! R=refresh`. Press `R` to reload the
list.

Unread messages are shown first. In `--all` mode an `N` marker appears in the `S` column
for unread messages.

### Folder Browser

Opened by pressing `Backspace` in the message list.

```
Folders (12)

├── INBOX
│   ├── Sent
│   ├── Drafts
│   └── Archive
│       └── 2025
└── Trash

  ↑↓=step  PgDn/PgUp=page  Enter=select  t=flat  Backspace/ESC=quit  [1/12]
```

| Key | Action |
|-----|--------|
| `↑` / `↓` | Move cursor one row up/down |
| `PgDn` | Move cursor one page down |
| `PgUp` | Move cursor one page up |
| `Enter` | Open the selected folder's message list |
| `t` | Toggle between tree and flat list view |
| `Backspace` / `ESC` / Ctrl-C | Quit |

### Message Reader View

Opened by pressing `Enter` on a message in the list, or directly via `email-cli show <uid>`.

```
From:    Alice <alice@example.com>
Subject: Re: Meeting tomorrow
Date:    Fri, 28 Mar 2026 09:14:00 +0100
────────────────────────────────────────
Hi,

Just confirming the meeting is on for 10:00.

-- [1/3] PgDn/↓=scroll  PgUp/↑=back  Backspace=list  ESC=quit --
```

| Key | Action |
|-----|--------|
| `PgDn` | Scroll forward one page |
| `PgUp` | Scroll back one page |
| `↓` | Scroll forward one line |
| `↑` | Scroll back one line |
| `Backspace` | Return to message list |
| `ESC` / Ctrl-C | Quit entirely |

Messages are cached locally after the first fetch.

### Navigation Hierarchy (email-tui)

```
Accounts screen  (Enter=open, e=SMTP settings, ESC=quit)
  └─ Message list  (Backspace=folders, s=sync, R=refresh, c=compose, r=reply, ESC=quit)
       └─ Folder browser  (Backspace=back/up, ESC=quit)
```

---

## CLI Batch Mode

Pass `--batch` or pipe output to a file/command to disable the interactive TUI. All
commands print plain text suitable for scripting.

### list

```
email-cli list [--all] [--folder <name>] [--limit <n>] [--offset <n>] [--batch]
```

| Option | Description |
|--------|-------------|
| _(none)_ | Show all messages; unread ones marked `N`, listed first |
| `--all` | Same as no option (all messages are always shown) |
| `--folder <name>` | Use a different folder instead of the configured default |
| `--limit <n>` | Number of messages per page (default: terminal height) |
| `--offset <n>` | Start from the nth message, 1-based (for paging scripts) |
| `--batch` | Disable interactive pager; use fixed limit of 100 |

Examples:

```bash
email-cli list
email-cli list --all
email-cli list --all --offset 21
email-cli list --folder INBOX.Sent --limit 50
email-cli list --all --batch | grep "Invoice"
```

### show

```
email-cli show <uid>
```

Displays the full content of the message with the given UID. The UID is shown in the
`list` output.

```bash
email-cli show 1234
email-cli --batch show 1234 | less
```

### folders

```
email-cli folders [--tree]
```

Lists all IMAP folders available on the server.

```bash
email-cli folders          # flat list
email-cli folders --tree   # folder hierarchy as a tree
```

Example tree output:

```
└── INBOX
    ├── Sent
    ├── Drafts
    └── Archive
        └── 2025
```

### config smtp

```
email-cli config smtp
```

Interactive wizard to set or update SMTP credentials. Equivalent to pressing `e` on the
accounts screen in `email-tui`.

### help

```
email-cli help [command]
```

```bash
email-cli help
email-cli help list
email-cli help show
email-cli help folders
email-cli help config
```
