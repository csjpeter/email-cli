---
name: email-cli
description: Use when reading, searching, sending or sorting e-mail from the command line with email-cli (IMAP and Gmail accounts), or when a script or agent needs a message's text, headers or attachments. Use email-cli-ro for looking, email-cli only for changing. Do not use for the interactive terminal UI (email-tui), for background sync (email-sync), or to type a password or authorise a Gmail account on the user's behalf.
---

# email-cli

Batch-mode e-mail client. Output is a table or text for a person on stdout,
errors on stderr, exit code 0 on success and non-zero on failure. A script
reads `--json`, not the table. Full reference: `man email-cli`,
`email-cli help <command>` and `email-cli <command> --help`; `--version` prints
the version.

## Two programs

- **`email-cli-ro`** reads only: `list`, `show`, `list-folders`, `list-labels`,
  `list-attachments`, `save-attachment`, `list-accounts`. It has no
  send or write command. **Use it whenever you only need to look.**
- **`email-cli`** has the same reading commands and, in addition, everything that
  changes mail, folders, labels, rules and configuration.

## Choosing the account

`email-cli [<account>] <command>` or `--account <address>`. With one configured
account it is implied; with several it is required. `list-accounts` shows them.
`list --all-accounts` goes through every account.

## Reading

```
email-cli-ro list                       # unread messages of the configured folder
email-cli-ro list --all --limit 50      # everything; --offset N to page
email-cli-ro list --from alice --since 2026-10-01 --before 2026-10-15
email-cli-ro list --json                # one object per message, never truncated
email-cli-ro list --folder __unread__   # virtual folders: __unread__ __flagged__
                                        # __answered__ __forwarded__ __junk__ __all__
email-cli-ro list --folder "__search__:3:invoice"   # 0 Subject 1 From 2 To 3 Body
email-cli-ro show 42                    # decoded text; File: line gives the stored path
email-cli-ro show 42 --raw              # undecoded RFC 2822 source
email-cli-ro show 42 --folder Archive   # UIDs are only unique within a folder
email-cli-ro list-attachments 42
email-cli-ro save-attachment 42 report.pdf [DIR]
```

Search runs on the locally cached mail and works offline.

## `--json`

`--json` prints the complete record as one parseable document on stdout (notices
and errors stay text on stderr; a failure still exits non-zero). It is accepted by
`list` (array of messages), `show` (object with headers, attachment names, body;
not with `--raw`), `list-folders` (array: name, separator, unread, flagged,
total; `--tree` is ignored), `list-labels` (array: name, id), `list-attachments`
(array: filename, size), `list-accounts` (array: account, type, server) and
`rules list` (array of accounts with their rules). `list --all-accounts` cannot
be combined with it. `list-folders --tree` is the human view of the folders.

## Changing

```
email-cli send --to a@example.org --subject "Hi" --body "Text" [--attach FILE]...
email-cli reply 42 [--all] [--body "Text"] [--folder F]   # quoted, Re:, In-Reply-To; marks answered locally until next sync
email-cli mark-read 42 | mark-unread 42 | mark-starred 42 | remove-starred 42
email-cli mark-junk 42 | mark-notjunk 42
email-cli trash 42 [--folder F]   # IMAP: to Trash folder; Gmail: TRASH label; in Trash: deletes for good
email-cli archive 42 [--folder F]  # IMAP: to Archive folder; Gmail: removes labels, marks read
email-cli create-folder NAME | delete-folder NAME          # IMAP
email-cli create-label NAME | delete-label ID | add-label 42 LABEL | remove-label 42 LABEL   # Gmail
email-cli rules list | rules apply [--dry-run] [--verbose] | rules remove --name N
email-cli rules edit --name N [--new-name M] [--if-from G] [--if-subject G] [--if-to G] [--if-label G] \
    [--add-label L] [--remove-label L] [--move-folder F]   # changes only what is given
email-cli rules add --name N [--if-from G] [--if-subject G] [--if-to G] [--if-label G] \
    [--add-label L] [--remove-label L] [--move-folder F]
email-cli remove-account ADDRESS     # deletes the account's configuration; local mail is kept
```
`send` needs SMTP settings already configured. Always try `rules apply --dry-run`
before `rules apply`. Run `remove-account` only when the user asked for exactly
that account to be removed. Every command that takes a UID also takes
`--folder NAME` (Gmail: `--label NAME`) for the mailbox the UID belongs to.

## Do not run these as an agent

They prompt for secrets or open a browser authorisation, and block on a terminal:
`add-account`, `config imap`, `config smtp`, `config password`, `config reauth`,
`migrate-credentials`. Tell the user which one to run; for a refused credential
the message already names the command (`email-cli config password` for IMAP,
`email-cli config reauth` for Gmail). `config reauth` also has `--no-browser`
(print the URL only), `--paste` (paste the redirect address back) and
`--import-token` (read a refresh token from stdin). `config show` is safe:
passwords are masked.

## Traps

- A UID is per folder; pass `--folder` when `show` reports several candidates.
- Gmail uses labels, not folders: `list-labels`, `--label`; folder commands
  (`create-folder`, `delete-folder`, `list-folders`) are IMAP-only.
- Flag, star, junk and label changes are sent to the server by the command
  itself; `rules apply` works on the locally cached messages, so run
  `email-sync` first when the cache may be stale.
- `email-sync` downloads mail in the background; `list`/`show` read the local
  store and fetch a message on first access.
