# email-cli

A terminal-based IMAP email client written in C.

[![CI](https://github.com/csjpeter/email-cli/actions/workflows/ci.yml/badge.svg)](https://github.com/csjpeter/email-cli/actions/workflows/ci.yml)
[![Valgrind](https://github.com/csjpeter/email-cli/actions/workflows/valgrind.yml/badge.svg)](https://github.com/csjpeter/email-cli/actions/workflows/valgrind.yml)
[![Coverage](https://csjpeter.github.io/email-cli/coverage-badge.svg)](https://csjpeter.github.io/email-cli/)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

> **Read-only client.** `email-cli` only reads your mailbox — it never sends, moves, deletes, or marks messages as read. Your email stays exactly as it is on the server.

---

## Table of Contents

- [Installation](#installation)
- [Configuration](#configuration)
  - [Provider Notes](#provider-notes)
- [Interactive Mode](#interactive-mode)
  - [Message List View](#message-list-view)
  - [Folder List View](#folder-list-view)
  - [Message Reader View](#message-reader-view)
- [CLI Batch Mode](#cli-batch-mode)
  - [list](#list)
  - [show](#show)
  - [folders](#folders)
  - [help](#help)
- [Security](#security)

---

## Installation

```bash
./manage.sh deps    # Install system dependencies (Ubuntu 24.04 / Rocky 9)
./manage.sh build   # Build → bin/email-cli
```

Run it:

```bash
bin/email-cli
```

---

## Configuration

On the first run, `email-cli` starts an interactive setup wizard that asks for:

- **IMAP server address** — e.g. `imaps://imap.example.com` or `imap://localhost:993`
- **Username** — your email address
- **Password** — your IMAP password
- **Default folder** — e.g. `INBOX`

The configuration is saved to `~/.config/email-cli/config.ini` and reused on subsequent runs.

To reconfigure, delete or edit that file manually:

```bash
rm ~/.config/email-cli/config.ini
bin/email-cli
```

> **One account at a time.** To switch accounts, replace or edit the config file.

### Provider Notes

| Provider | IMAP host | Notes |
|----------|-----------|-------|
| Gmail | `imaps://imap.gmail.com` | Enable IMAP in Gmail settings. If 2FA is on, use an [App Password](https://myaccount.google.com/apppasswords) instead of your regular password. |
| Outlook / Hotmail | `imaps://outlook.office365.com` | Use your full email address as username. |
| Fastmail | `imaps://imap.fastmail.com` | Standard credentials. |
| Self-hosted | `imaps://mail.yourdomain.com` | For self-signed certificates set `SSL_NO_VERIFY=1` in the config file — development only. |

---

## Interactive Mode

When run without arguments in a terminal, `email-cli` opens a full-screen interactive TUI starting with the unread messages in your configured folder. Navigation uses keyboard shortcuts — no mouse required.

### Message List View

The default view on launch, or opened with `email-cli list` / `email-cli list --all`.

```
1-20 of 42 unread message(s) in INBOX.

  UID    From                  Subject               Date
  ═════  ════════════════════  ════════════════════  ════════════════
▶  1234  Alice <alice@ex.com>  Re: Meeting tomorrow  2026-03-28 09:14
   1230  Bob <bob@ex.com>      Invoice attached      2026-03-27 18:01
   ...

  ↑↓=step  PgDn/PgUp=page  Enter=open  Backspace=folders  ESC=quit  [1/42]
```

| Key | Action |
|-----|--------|
| `↑` / `↓` | Move cursor one row up/down |
| `PgDn` | Move cursor one page down |
| `PgUp` | Move cursor one page up |
| `Enter` | Open selected message |
| `Backspace` | Go to folder list |
| `ESC` or Ctrl-C | Quit |

Unread messages are shown first. In `--all` mode an `N` marker appears in the `S` column for unread messages.

### Folder List View

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

Messages are cached locally at `~/.cache/email-cli/messages/<folder>/<uid>.eml` after the first fetch.

---

## CLI Batch Mode

Pass `--batch` or pipe output to a file/command to disable the interactive TUI. All commands print plain text suitable for scripting.

### list

```
email-cli list [--all] [--folder <name>] [--limit <n>] [--offset <n>] [--batch]
```

| Option | Description |
|--------|-------------|
| _(none)_ | Show unread messages in the configured folder |
| `--all` | Show all messages; unread ones marked `N`, listed first |
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

Displays the full content of the message with the given UID. The UID is shown in the `list` output.

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

### help

```
email-cli help [command]
```

```bash
email-cli help
email-cli help list
email-cli help show
email-cli help folders
```

---

## Security

- **Read-only:** `email-cli` issues only `FETCH` and `SEARCH` IMAP commands. It never sends, moves, deletes, or modifies messages or flags on the server.
- **Credentials at rest:** The configuration file is written with `0600` permissions, readable only by the owning user. The password is stored in plaintext — keep the file private.
- **Transport:** Connections use `imaps://` (TLS 1.2+) by default. Plain `imap://` is supported for local testing only. `SSL_NO_VERIFY=1` disables certificate verification — never use it in production.
- **Local cache:** Fetched messages are stored in `~/.cache/email-cli/` with directory permissions `0700`. No external service has access to the cache.
- **Logs:** Session diagnostics are written to `~/.cache/email-cli/logs/session.log`. Logs may include IMAP server responses; rotate or delete them as needed.
- **Memory safety:** The codebase is tested with AddressSanitizer and Valgrind on every CI run to eliminate memory leaks and buffer overflows. See the [developer docs](docs/) for details.
