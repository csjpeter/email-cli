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

## Message list, IMAP account (EMAIL-143)

The list opens with `Enter` on a folder of the folder browser.  The table has
`rows - 5` data rows (19 at 80x24); the title bar names the range shown
(`a-b of N message(s)`) and the status line ends with `[cursor/total]`, or
`[N marked]` while rows are marked.  The help (`?`) is made from the key table
view `list-imap` (`libemail/src/core/key_table.c`).  Tests are in
`tests/pty/test_pty_views.c`, named `test_lk_*` (80x24, the mock with
`MOCK_IMAP_COUNT=30`: 20 unread listed first, newest first, then 10 read).
The Gmail list is studied in EMAIL-144.

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Down`, `j` / `Up`, `k` | `j/k step` | cursor one row, stops at the last / first | `test_lk_navigation` |
| `PgDn` / `PgUp` | `PgDn/PgUp page` | one page (the 19 rows of the table), clamped | `test_lk_navigation` |
| `Home` / `End` | help: Top / Bottom | first / last message | `test_lk_navigation` |
| `Enter` | `Enter open` | the reader on the cursor message; `Esc`, `q`, `Backspace` there return with the cursor kept | `test_lk_enter_opens_reader` |
| `Esc`, `q`, `Backspace` | `Esc/q/BS folders` | back to the folder browser (`Esc` first drops marks, then the filter) | `test_lk_back_keys`, `test_lk_marks` |
| `ctrl-c` | help: Quit | the program quits at once | `test_lk_back_keys` |
| `c` | `c compose` | new message dialog; after it the list returns with the cursor kept | `test_lk_compose_reply_forward` |
| `r` / `A` / `F` | `r reply`, `A r-all`, `F fwd` | the dialog for the cursor message (`Re: ...`, `Fwd: ...`); not on an empty filter result | `test_lk_compose_reply_forward`, `test_lk_filter_without_match` |
| `U` | `U refresh` | lists the folder again | `test_lk_refresh_and_rules` |
| `l` | `l rules` | the rules editor; `Esc` returns to the list | `test_lk_refresh_and_rules` |
| `Space` / `x` | `Space mark` | marks the row and steps down / clears all marks | `test_lk_marks`, `test_lk_one_message` |
| `/` | help: Filter | filter bar (Tab changes the scope); `Enter` commits, `Esc` clears | `test_tui_list_search_filter`, `test_lk_filter_without_match` |
| `d`, `Delete` | `d trash` | asks first naming the message, No is the default; Trash: `del` | `test_lk_delete_asks`, `test_tui_list_bulk_delete_confirm` |
| `*` `+` `-` `!` | `* star`, `+/- read`, `! junk` | act at once on the marked rows or the cursor row | `test_keys_registry_routine`, `test_keys_junk_toggle_imap` |
| `:` | `: actions` | the action menu (star, read, unread, junk, delete, sync, move, `$Done`) | `test_tui_list_bulk_star_marked`, `test_tui_list_sync_and_refresh` |
| `?` | `? help` | overlay from the key table; scrolls with `j`, `End`; `Esc`/`q` close only it | `test_lk_help_overlay` |
| `n f s m D h R a t u v g G N`, `Left`, `Right`, `Tab` | | change nothing, show nothing | `test_lk_unbound_keys` |

0 messages (the folder is empty): the status line offers only `Esc/q/BS`, `:`
(the sync menu), `c`, `U`, `l` and `?`; those work, every other key is
ignored (`test_lk_empty_folder`).  1 message: every cursor key stays on it,
`Space` marks without stepping (`test_lk_one_message`).  30 messages:
`test_lk_navigation`.

Findings of the study, all fixed with EMAIL-143: EMAIL-184 (an empty filter
result: `Enter` opened a hidden message, `r A F` replied to it), EMAIL-185
(the title range counted 18 rows, the table shows 19), EMAIL-186 (the cursor
went to the first message after `c r A F` and `l`), EMAIL-187 (the empty
folder offered `? help` and ignored it), EMAIL-188 (the empty folder offered
`j/k`, `Enter`, `d`, `r`), EMAIL-189 (the empty folder's rule row was 81
columns wide; the no-cache hint named the dead key `s`).  Already right:
`Esc`/`q`/`Backspace` back, `ctrl-c`, `j k` and the arrows, `PgUp`/`PgDn`
clamping, `Space`/`x`/`Esc` marks, the delete confirmation with No as the
default, `h` and the old keys doing nothing, `U`, `l`, the reader round trip.

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
