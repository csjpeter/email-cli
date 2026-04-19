[![CI](https://github.com/csjpeter/email-cli/actions/workflows/ci.yml/badge.svg)](https://github.com/csjpeter/email-cli/actions/workflows/ci.yml)
[![Valgrind](https://github.com/csjpeter/email-cli/actions/workflows/valgrind.yml/badge.svg)](https://github.com/csjpeter/email-cli/actions/workflows/valgrind.yml)
[![Coverage](https://csjpeter.github.io/email-cli/coverage-badge.svg)](https://csjpeter.github.io/email-cli/)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

# email-cli

A terminal-based email client suite written in C, supporting both standard **IMAP** and the
native **Gmail REST API** (OAuth2). Offers four binaries for different use cases: a
read-only batch tool for scripting and AI agents, a full read-write CLI, a full-featured
interactive TUI, and a background sync daemon.

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
| `email-cli` | Batch | Yes | CLI scripting: list, show, send, flag, label management |
| `email-cli-ro` | Batch | No | Read-only CLI; safe for AI agents and automation |
| `email-tui` | Interactive | Yes | Full-featured TUI: compose, reply, send, flag, labels |
| `email-sync` | Batch | Sync only | Background sync daemon / cron helper |

All four binaries share the same configuration and local message cache.

**`email-cli-ro`** is strictly read-only: it only issues `FETCH` (with `BODY.PEEK`) and
`SEARCH` IMAP commands — never sends, moves, deletes, or modifies flags on the server.

---

## Security

- **Read-only variant:** `email-cli-ro` never writes to the server — not even `\Seen`.
- **Full write operations:** `email-cli` and `email-tui` support marking, flagging,
  composing, replying, sending, and Gmail label management.
- **IMAP credentials at rest:** The configuration file is written with `0600` permissions,
  readable only by the owning user. IMAP passwords are stored in plaintext — keep the file
  private.
- **Gmail OAuth2:** Gmail accounts use OAuth2 — no password is stored. The
  `refresh_token` is stored in `~/.config/email-cli/accounts/<email>/config.ini` with
  mode `0600`. The `client_id` and `client_secret` identify the app (not truly secret for
  native apps — per Google's own guidelines).
- **Transport:** IMAP connections use `imaps://` (TLS 1.2+) by default. Plain `imap://`
  is supported for local testing only. `SSL_NO_VERIFY=1` disables certificate
  verification — never use it in production.
- **Local cache:** Fetched messages are stored in `~/.local/share/email-cli/` with
  directory permissions `0700`. No external service has access to the cache.
- **Logs:** Session diagnostics are written to `~/.cache/email-cli/logs/session.log`.
  Logs may include server responses; rotate or delete them as needed.
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
bin/email-cli list  # batch CLI (list, show, send, config, labels)
```

---

## Configuration

### Account setup

On the first run, `email-tui` (or `email-cli`) starts an interactive setup wizard that
asks which account type to add:

```
Select account type:
  [1] IMAP
  [2] Gmail (native API, OAuth2)
```

Multiple accounts are supported. Each account is stored in its own directory:
`~/.config/email-cli/accounts/<email>/config.ini`.

### IMAP setup

The wizard asks for:

- **IMAP server address** — e.g. `imaps://imap.example.com`
- **Username** — your email address
- **Password** — your IMAP password
- **Default folder** — e.g. `INBOX`

### Gmail setup (native API, OAuth2)

Gmail accounts use the Gmail REST API — not IMAP. You need OAuth2 credentials from a
Google Cloud project. Run the built-in guide for step-by-step instructions:

```bash
email-cli help gmail
```

The wizard asks for your **Client ID** and **Client Secret**, then opens a browser for
the OAuth2 authorization flow. The `refresh_token` is saved automatically.

### SMTP setup

To enable composing and sending mail, configure SMTP settings from the CLI:

```bash
email-cli config smtp
```

or by pressing `e` on the accounts screen inside `email-tui`. The wizard asks for:

- **SMTP server address** — e.g. `smtps://smtp.example.com`
- **Username** — usually your email address
- **Password** — your SMTP password (may differ from IMAP password)

SMTP credentials are saved in the account's `config.ini` (mode `0600`).

### Provider Notes

| Provider | Protocol | IMAP / API host | SMTP host | Notes |
|----------|----------|-----------------|-----------|-------|
| **Gmail** | Gmail REST API | — (native API) | `smtps://smtp.gmail.com` | Select [2] Gmail in the wizard. Run `email-cli help gmail` for OAuth2 setup. No IMAP needed. |
| Outlook / Hotmail | IMAP | `imaps://outlook.office365.com` | `smtps://smtp.office365.com` | Use your full email address as username. |
| Fastmail | IMAP | `imaps://imap.fastmail.com` | `smtps://smtp.fastmail.com` | Standard credentials. |
| Self-hosted | IMAP | `imaps://mail.yourdomain.com` | `smtps://mail.yourdomain.com` | For self-signed certificates set `SSL_NO_VERIFY=1` — development only. |

---

## Interactive Mode (email-tui)

The interactive TUI is provided exclusively by `email-tui`. The other binaries
(`email-cli`, `email-cli-ro`, `email-sync`) are batch-only.

### Accounts Screen

`email-tui` opens at the accounts screen, which lists all configured accounts with
unread and starred/flagged message counts.

```
Accounts (2)

  user@gmail.com         (Gmail)    Unread:  5   Starred:  2
  work@example.com       (IMAP)     Unread: 12   Flagged:  1

  ↑↓=select  Enter=open  n=new account  d=delete  e=SMTP settings  ESC=quit
```

| Key | Action |
|-----|--------|
| `↑` / `↓` | Move cursor |
| `Enter` | Open the selected account |
| `n` | Add a new account (IMAP or Gmail) |
| `d` | Delete the selected account |
| `e` | Open the SMTP setup wizard for the selected account |
| `i` | Edit IMAP settings for the selected account |
| `ESC` / Ctrl-C | Quit |

### IMAP: Folder Browser

Opened by pressing `Backspace` in the IMAP message list, or directly from the accounts
screen.

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
| `PgDn` / `PgUp` | Page down/up |
| `Enter` | Open the selected folder's message list |
| `t` | Toggle between tree and flat list view |
| `Backspace` / `ESC` / Ctrl-C | Back / quit |

### Gmail: Label Browser

Gmail accounts use labels instead of folders. The label browser shows all labels with
message counts.

```
Labels (8)

  INBOX          (47)
  STARRED        ( 2)
  Sent Mail      (...)
  Work           (12)
  Personal       ( 5)
  _nolabel       Archive

  ↑↓=step  PgDn/PgUp=page  Enter=open  Backspace/ESC=back
```

| Key | Action |
|-----|--------|
| `↑` / `↓` | Move cursor |
| `Enter` | Open the selected label's message list |
| `Backspace` / `ESC` | Back to accounts |

### IMAP: Message List View

```
1-20 of 42 unread message(s) in INBOX.

  UID    From                  Subject               Date
  ═════  ════════════════════  ════════════════════  ════════════════
▶  1234  Alice <alice@ex.com>  Re: Meeting tomorrow  2026-03-28 09:14
   1230  Bob <bob@ex.com>      Invoice attached      2026-03-27 18:01

  ↑↓=step  PgDn/PgUp=page  Enter=open  s=sync  R=refresh  Backspace=folders  ESC=quit
```

#### Navigation

| Key | Action |
|-----|--------|
| `↑` / `↓` | Move cursor one row up/down |
| `PgDn` / `PgUp` | Page down/up |
| `Enter` | Open selected message |
| `Backspace` | Go to folder browser |
| `ESC` / Ctrl-C | Quit |

#### IMAP flag keys

| Key | Action |
|-----|--------|
| `n` | Mark as new / unflag `\Seen` |
| `f` | Toggle `\Flagged` keyword flag |
| `d` | Toggle `done` IMAP keyword |

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

### Gmail: Message List View

Gmail message lists show label-tagged messages. The current label is shown in the
header.

```
INBOX — 47 messages

  UID               From                  Subject               Date
  ════════════════  ════════════════════  ════════════════════  ════════════════
▶ 18c9b46d67a6…   Alice <alice@ex.com>  Re: Meeting tomorrow  2026-03-28 09:14
  d99192cdf1df3    Bob <bob@ex.com>      Invoice attached      2026-03-27 18:01

  n=toggle-read  f=toggle-starred  a=archive  d=rm-label  D=trash  u=untrash  s=sync  R=refresh
```

#### Gmail flag and label keys

| Key | Action |
|-----|--------|
| `n` | Toggle read/unread |
| `f` | Toggle starred |
| `a` | Archive — remove INBOX label |
| `d` | Remove current label from message |
| `D` | Move to Trash |
| `u` | Untrash — restore saved labels |
| `l` | Open label picker (add/remove labels) |
| `c` | Compose new message |
| `r` | Reply to selected message |
| `s` | Start background sync |
| `R` | Refresh message list from local cache |
| `?` / `h` | Show key help |

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
Accounts screen  (Enter=open, n=new, d=delete, e=SMTP, ESC=quit)
  └─ IMAP: Folder browser  (Backspace=back, ESC=quit)
  │       └─ Message list  (Backspace=folders, s=sync, R=refresh, c=compose, r=reply, ESC=quit)
  │                └─ Message reader  (Backspace=list, ESC=quit)
  └─ Gmail: Label browser  (Backspace=back, ESC=quit)
          └─ Message list  (a=archive, d=rm-label, D=trash, u=untrash, l=labels, ESC=quit)
                   └─ Message reader  (Backspace=list, ESC=quit)
```

---

## CLI Batch Mode (email-cli / email-cli-ro)

`email-cli` and `email-cli-ro` are batch-only tools — all output is plain text suitable
for scripting and piping. There is no interactive pager or TUI.

`email-cli-ro` supports only read commands (`list`, `show`, `folders`, `attachments`).
`email-cli` supports all commands including write operations and Gmail label management.

### list

```
email-cli list [--all] [--folder <name>] [--limit <n>] [--offset <n>]
```

| Option | Description |
|--------|-------------|
| _(none)_ | Show unread messages only |
| `--all` | Show all messages; unread ones marked `N`, listed first |
| `--folder <name>` | Use a different folder / Gmail label |
| `--limit <n>` | Maximum number of messages to show (default: 100) |
| `--offset <n>` | Start from the nth message, 1-based (for paging scripts) |

```bash
email-cli list
email-cli list --all
email-cli list --all --offset 21
email-cli list --folder INBOX.Sent --limit 50
email-cli list --folder STARRED        # Gmail: starred messages
email-cli list --all | grep "Invoice"
```

### show

```
email-cli show <uid>
```

Displays the full content of the message with the given UID.

```bash
email-cli show 1234
email-cli show 18c9b46d67a6…
```

### folders

```
email-cli folders [--tree]
```

Lists all IMAP folders (or Gmail labels) available on the server.

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

### Flags and starring

```
email-cli mark-read <uid>
email-cli mark-unread <uid>
email-cli mark-starred <uid>
email-cli remove-starred <uid>
```

Works for both IMAP (`\Seen` / `\Flagged`) and Gmail (UNREAD / STARRED labels).

### Gmail label management

```
email-cli add-label <uid> <label>       # Add a label to a message
email-cli remove-label <uid> <label>    # Remove a label from a message
email-cli list-labels                   # List all labels in the account
email-cli create-label <name>           # Create a new label
email-cli delete-label <name>           # Delete a label
```

These commands require a Gmail account (`GMAIL_MODE=1`).

### Account management

```
email-cli list-accounts                 # List configured accounts
email-cli add-account                   # Interactive wizard to add an account
email-cli remove-account <email>        # Remove an account and its config
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
email-cli help gmail           # Gmail OAuth2 setup guide
email-cli help add-label
email-cli help list-labels
```
