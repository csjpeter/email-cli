# User Story: Interactive TUI Launch

## Summary
As a user, I want to launch email-cli without any arguments and see my unread messages
in an interactive full-screen terminal interface.

## Invocation
```
email-cli
```

## Preconditions
- Configuration exists at `~/.config/email-cli/config.ini`
- stdout is a terminal (TTY)

## Flow
1. email-cli starts with no command argument and stdout is a TTY.
2. The interactive message list opens for the folder configured in `cfg->folder`
   (defaults to `INBOX`).
3. The list shows unread messages first, then read messages, paginated to terminal height.
4. User navigates with arrow keys / PgDn / PgUp; pressing Enter opens a message.
5. Pressing Backspace from the message list opens the interactive folder browser.
6. Selecting a folder in the browser returns to the message list for that folder.
7. Pressing ESC or `q` from the message list exits the application.

## Note on email-tui
`email-tui` follows the same flow but adds an accounts screen as the outermost TUI level:
the user sees the accounts screen first and presses Enter to reach the message list.
See [US-18](18-email-tui.md) for the full `email-tui` navigation hierarchy.

## First-Run Behaviour
- If no configuration exists, the setup wizard runs interactively before the TUI.

## Non-TTY Fallback
- If stdout is not a TTY (piped/redirected), the general help page is printed and
  the process exits with code 0.
