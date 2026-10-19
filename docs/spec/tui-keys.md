# TUI key tables

One table per view: key, label on the screen, behaviour, and the PTY test that
drives it (`tests/pty/test_pty_views.c`).  Studied view by view under EMAIL-140.
`h` is not a help key anywhere; help is `?`.

## Accounts list (EMAIL-141)

Header `Email Accounts (N)`; with no account the table says
`No accounts configured.`  The status line shows
`j/k select  Enter open  n add  d delete  i IMAP  e SMTP  ESC quit  ?`
(lowest priority hints drop first at 80 columns; Esc and the position
`[n/N]` stay).  `:` shows as `actions` only on a Gmail account.

| Key | Label | Behaviour | With no account | Test |
|-----|-------|-----------|-----------------|------|
| `j`, Down | `j/k select` | Cursor one row down; stops at the last | nothing | `test_accounts_keys_cursor` |
| `k`, Up | `j/k select` | Cursor one row up; stops at the first | nothing | `test_accounts_keys_cursor` |
| PgDn / PgUp | - | Cursor a page (rows visible - 5) down / up, stops at the ends | nothing | `test_accounts_pages_by_the_visible_rows`, `test_accounts_keys_one_account`, `test_accounts_keys_no_account` |
| Home / End | - | First / last account | nothing | `test_accounts_keys_cursor` |
| Enter | `Enter open` | Opens the folder (IMAP) or label (Gmail) browser of the account | nothing | `test_tui_accounts_enter_opens_list`, `test_accounts_keys_delete_position` |
| `n` | `n add` | Setup wizard; Ctrl-D gives up with `Wizard aborted.` | works | `test_tui_accounts_new_key`, `test_accounts_keys_no_account` |
| `d` | `d delete` | Asks `Delete account NAME? (y/N)`; `y`/`Y` removes it and the status line says `Account removed. Local messages preserved: PATH`; any other key keeps it (`Account NAME kept.`) | nothing | `test_tui_accounts_delete_key`, `test_tui_accounts_delete_declined`, `test_accounts_keys_delete_position`, `test_accounts_keys_no_account` |
| `i` | `i IMAP` | IMAP wizard of the account; done: `IMAP settings saved.`, Ctrl-D: `Cancelled - nothing was changed.` | nothing | `test_tui_accounts_imap_edit_key`, `test_accounts_keys_one_account` |
| `e` | `e SMTP` | SMTP wizard of the account; same messages with SMTP | nothing | `test_accounts_screen_wizards_can_be_abandoned`, `test_accounts_keys_one_account` |
| `:` | `: actions` (Gmail) | Gmail: menu with renewal of the authorization; IMAP: notice `No actions for an IMAP account here` | nothing | `test_tui_accounts_colon_on_imap_account`, `test_accounts_keys_no_account` |
| `?` | `? help` | Help overlay `Accounts shortcuts`; Esc closes it, other keys change nothing; the `:` row only on a Gmail account | works | `test_tui_accounts_help_panel`, `test_accounts_keys_one_account`, `test_accounts_keys_no_account` |
| Esc | `ESC quit` | Quits with status 0 | works | `test_tui_accounts_esc_quit`, `test_accounts_keys_cursor` |
| `q`, Backspace | - | Ignored (top level) | ignored | `test_tui_accounts_esc_quit`, `test_tui_accounts_backspace_ignored` |
| Ctrl-C | - | Quits at once | works | `test_interactive_list_ctrl_c_quits_at_once` |
| any other key | - | Nothing | nothing | `test_accounts_keys_cursor` |
