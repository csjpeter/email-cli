# email-tui: key tables per view (PTY study, EMAIL-140)

One section per view: key, label on screen, behaviour, test.  The tests are PTY
tests against the mock servers; where a view is studied at 80x24 the test says
so.  Policy: `policies/tui-interface-model.md`.

## Attachment picker and save prompts (EMAIL-147)

The picker opens with `a` in the reader when the message has two or more
attachments; with exactly one, `a` goes straight to the save prompt; with none,
`a` shows `No attachments` in the reader.  Saving all attachments is in the `:`
menu of the reader (`Save all attachments`, prompt `Save all to:`).  The help
(`?`) is made from the key table view `attach-picker`
(`libemail/src/core/key_table.c`).  Tests are in `tests/pty/test_pty_views.c`
(80x24, the mock with `MOCK_IMAP_ATTACHMENTS`), test names below.

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Down`, `j` | `select` | cursor down one row, stops at the last | `test_picker_navigation` |
| `Up`, `k` | `select` | cursor up one row, stops at the first | `test_picker_navigation` |
| `PgDn` / `PgUp` | help: Page down / up | one page (rows of the screen less 4), clamped | `test_picker_navigation` |
| `Home`, `g` / `End`, `G` | help: First / Last attachment | first / last row | `test_picker_navigation` |
| `Enter` | `choose` | the row under the cursor; prompt `Save as:` with the path in `Downloads` or the home directory | `test_picker_navigation`, `test_picker_choose_second_then_cancel`, `test_show_save_single` |
| `Esc`, `q`, `Backspace` | `Esc/q/BS back` | back to the reader | `test_picker_back_keys` |
| `ctrl-c` | help: Quit | the program quits at once | `test_picker_ctrl_c_quits` |
| `?` | help | overlay from the key table; `Esc` closes only the overlay | `test_picker_help` |
| any other key | | changes nothing, shows nothing | `test_picker_unbound_key` |
| `[n/N]` | status line | cursor row of the count | `test_picker_back_keys` |

Save prompts (`Save as:`, `Save all to:`; the line editor is in `input-line.md`):

| Key | Behaviour | Test |
|---|---|---|
| `Enter` | save and say `Saved: PATH` / `Saved N/N files to: DIR` | `test_show_save_single`, `test_a_one_attachment`, `test_show_save_all_confirm` |
| `Esc` | cancel, nothing written, back to the reader | `test_picker_choose_second_then_cancel`, `test_a_one_attachment`, `test_show_save_all_cancel` |
| `Home` `End` `Left` `Right` `Backspace` `Delete` | line editing | `test_show_save_input_line_cursor` |
| `Tab` / `Shift-Tab` | path completion forward / back | `test_show_save_tab_completion` |

0 attachments: `test_a_without_attachments`; 1 attachment: `test_a_one_attachment`;
many (25, paging): `test_picker_navigation`.

Findings of the study, all fixed with EMAIL-147: EMAIL-176 (`q` was ignored),
EMAIL-177 (`ctrl-c` went back instead of quitting), EMAIL-178 (no `j k g G
Home End`, `PgUp`/`PgDn` moved one row), EMAIL-179 (no `?`), EMAIL-180 (`a`
without attachments said nothing).  Already right: `Esc` and `Backspace` back,
arrows, `Enter`, the save and cancel paths, the file name sanitising, the
count in the status line.

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
