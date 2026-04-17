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

| Binary | Mode | Write ops | Intended use |
|--------|------|-----------|--------------|
| `email-cli` | Batch | Yes | CLI scripting: list, show, send, config |
| `email-cli-ro` | Batch | No | Read-only CLI; safe for AI agents |
| `email-tui` | Interactive | Yes | Full-featured TUI: compose, reply, send, flag |
| `email-sync` | Batch | Sync only | Background sync daemon / cron helper |

All four binaries share the same configuration file and local message cache.

---

## Security

- **Truly read-only:** `email-cli-ro` issues only `FETCH` (with `BODY.PEEK`, which never
  sets `\Seen`) and `SEARCH` IMAP commands. It never sends, moves, deletes, or modifies
  messages or flags on the server — not even the `\Seen` flag.
- **Batch write:** `email-cli` is batch-only (no interactive TUI). It adds batch send
  (`email-cli send`) and configuration management. It links `libwrite` for SMTP.
- **Full interactive write:** `email-tui` is the interactive TUI with compose, reply,
  send, and flag operations. All write operations are available.
- **Sync only:** `email-sync` synchronizes the server with the local store. Only this
  binary can sync; the other three do not.
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

Run:

```bash
bin/email-tui       # full-featured interactive TUI
bin/email-cli list  # batch CLI (list, show, send, config)
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

## Interactive Mode (email-tui)

The interactive TUI is provided exclusively by `email-tui`. The other binaries
(`email-cli`, `email-cli-ro`, `email-sync`) are batch-only.

### Accounts Screen

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

### Message List View

Reached via Enter on the accounts screen.

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

#### Flag keys

| Key | Action |
|-----|--------|
| `n` | Mark selected message as new / unflag `\Seen` |
| `f` | Toggle `\Flagged` keyword flag |
| `d` | Mark as done (sets `done` IMAP keyword) |

#### Compose / reply keys

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

### Navigation Hierarchy

```
Accounts screen  (Enter=open, e=SMTP settings, ESC=quit)
  └─ Folder browser  (Backspace=back/up, ESC=quit)
       └─ Message list  (Backspace=folders, s=sync, R=refresh, c=compose, r=reply, ESC=quit)
            └─ Message reader  (Backspace=list, ESC=quit)
```

---

## CLI Batch Mode (email-cli / email-cli-ro)

`email-cli` and `email-cli-ro` are batch-only tools — all output is plain text suitable
for scripting and piping. There is no interactive pager or TUI.

### list

```
email-cli list [--all] [--folder <name>] [--limit <n>] [--offset <n>]
```

| Option | Description |
|--------|-------------|
| _(none)_ | Show unread (UNSEEN) messages only |
| `--all` | Show all messages; unread ones marked `N`, listed first |
| `--folder <name>` | Use a different folder instead of the configured default |
| `--limit <n>` | Maximum number of messages to show (default: 100) |
| `--offset <n>` | Start from the nth message, 1-based (for paging scripts) |

```bash
email-cli list
email-cli list --all
email-cli list --all --offset 21
email-cli list --folder INBOX.Sent --limit 50
email-cli list --all | grep "Invoice"
```

### show

```
email-cli show <uid>
```

Displays the full content of the message with the given UID.

```bash
email-cli show 1234
email-cli show 1234 | less
```

### folders

```
email-cli folders [--tree]
```

Lists all IMAP folders available on the server.

```bash
email-cli folders
email-cli folders --tree
```

### attachments

```
email-cli attachments <uid>
```

Lists all MIME attachments in a message (filename and size).

### save-attachment

```
email-cli save-attachment <uid> <filename> [dir]
```

Saves a single attachment to disk. Default directory: `~/Downloads` or `~`.

```bash
email-cli save-attachment 42 report.pdf
email-cli save-attachment 42 report.pdf /tmp
```

### send

```
email-cli send --to <addr> --subject <text> --body <text>
```

Sends a message non-interactively. SMTP must be configured first.

```bash
email-cli send --to friend@example.com --subject "Hello" --body "Hi there!"
```

### config

```
email-cli config show          # Print current config (passwords masked)
email-cli config imap          # IMAP setup wizard
email-cli config smtp          # SMTP setup wizard
```

Use `--account <email>` when multiple accounts are configured.

### help

```
email-cli help [command]
```

```bash
email-cli help
email-cli help list
email-cli help show
email-cli help folders
email-cli help attachments
email-cli help save-attachment
email-cli help send
email-cli help config
```
