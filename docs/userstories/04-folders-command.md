# User Story: Folders Command

## Summary
As a user, I want to list all IMAP folders available on my mail server.

## Invocation
```
email-cli folders [--tree]
email-cli folders --help
```

## Options
| Option | Description |
|--------|-------------|
| `--tree` | Render the folder hierarchy as a tree using box-drawing characters |

## Behaviour
1. Tries the local folder cache first (`<account_base>/folders.cache`), populated by `sync`.
2. On a cache miss, connects to the IMAP server and issues `LIST "" "*"`.
3. Decodes IMAP Modified UTF-7 folder names to UTF-8 for display.
4. For each folder, shows message counts from the local manifest: `INBOX (3/42)`
   where `3` = unread, `42` = total.
5. Folders with no local manifest data are shown without counts.

## Flat mode output example
```
INBOX (3/42)
INBOX.Sent
INBOX.Drafts
INBOX.Trash
```

## Tree mode output example
```
INBOX (3/42)
├── Sent
├── Drafts
└── Trash
```

## Folder Browser
The interactive folder browser (launched from TUI via Backspace) uses the same folder
list and supports tree/flat toggle with `t`, cursor navigation, and Enter to select.

## Exit Codes
- `0` success
- `1` server connection error
