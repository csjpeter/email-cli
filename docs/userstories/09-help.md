# User Story: Help

## Summary
As a user, I want to get help information about email-cli and its commands.

## Invocation
```
email-cli --help
email-cli help
email-cli help <command>
email-cli <command> --help
```

## Behaviour
- `email-cli --help` or `email-cli help`: prints the general help page listing all commands.
- `email-cli help <command>` or `email-cli <command> --help`: prints detailed help for
  the specified command.

## Supported help topics
- `list`
- `show`
- `folders`
- `sync`
- `cron`

## General Help Output
```
Usage: email-cli [--batch] <command> [options]

Global options:
  --batch           Disable interactive pager; use fixed page size (100).

Commands:
  list              List messages in the configured mailbox
  show <uid>        Display the full content of a message by its UID
  folders           List available IMAP folders
  sync              Download all messages in all folders to local store
  cron              Manage automatic background sync (setup/remove/status)
  help [command]    Show this help, or detailed help for a command

Run 'email-cli help <command>' for more information.
```

## Exit Codes
- `0` always
