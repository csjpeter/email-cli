---
name: email-cli
description: Use when reading, searching, sending or sorting e-mail from the command line with email-cli (IMAP and Gmail accounts), or when a script or agent needs a message's text, headers or attachments. Use email-cli-ro for looking, email-cli only for changing. Do not use for the interactive terminal UI (email-tui), for background sync (email-sync), or to type a password or authorise a Gmail account on the user's behalf.
---

# email-cli

Batch-mode e-mail client. Output is plain text on stdout, errors on stderr,
exit code 0 on success and non-zero on failure. Full reference: `man email-cli`
and `email-cli help <command>`.

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
email-cli-ro list --from alice --since 2026-10-01
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

Search runs on the locally cached mail and works offline. `--json` keeps stdout a
single parseable document; notices go to stderr.

## Changing

```
email-cli send --to a@example.org --subject "Hi" --body "Text" [--attach FILE]...
email-cli mark-read 42 | mark-unread 42 | mark-starred 42 | remove-starred 42
email-cli mark-junk 42 | mark-notjunk 42
email-cli create-folder NAME | delete-folder NAME          # IMAP
email-cli create-label NAME | delete-label ID | add-label 42 LABEL | remove-label 42 LABEL   # Gmail
email-cli rules list | rules apply [--dry-run] | rules add --name N --if-from G --add-label L | rules remove --name N
```

`send` needs SMTP settings already configured. Always try `rules apply --dry-run`
before `rules apply`.

## Do not run these as an agent

They prompt for secrets or open a browser authorisation, and block on a terminal:
`add-account`, `config imap`, `config smtp`, `config password`, `config reauth`,
`migrate-credentials`. Tell the user which one to run; for a refused credential
the message already names the command (`email-cli config password` for IMAP,
`email-cli config reauth` for Gmail). `config show` is safe: passwords are masked.

## Traps

- A UID is per folder; pass `--folder` when `show` reports several candidates.
- Gmail uses labels, not folders: `list-labels`, `--label`; folder commands
  (`create-folder`, `list-folders`) are IMAP-only.
- Rules and flag changes made locally are not guaranteed to reach a Gmail server
  (see the project's open tickets); do not rely on them for Gmail accounts.
- `email-sync` downloads mail in the background; `list`/`show` read the local
  store and fetch a message on first access.
