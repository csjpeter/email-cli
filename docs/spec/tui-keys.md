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

## Message reader, IMAP account (EMAIL-146)

`Enter` on a message of the list opens it; an empty folder has no message, so
there is no reader with 0 items (`test_lk_empty_folder`).  At 80x24 the header
block is rows 0-8, the body rows 9-22 (14 lines), the status line row 23; the
status line ends with `[page/pages]`, the last page being the last 14 lines
(not a page that is mostly blank).  The status hints are cut by priority at 80
columns (`r`, `F`, `*`, `v` are not shown; the keys work).  The help (`?`) is
made from the key table views `read-imap` / `read-gmail`
(`libemail/src/core/key_table.c`).  Tests are in `tests/pty/test_pty_views.c`,
named `test_rk_*` (80x24; the single 25-line mock message is two pages, with
`MOCK_IMAP_COUNT=N` the body is one line).  The mock serves one connection at
a time, so a test that needs a quit or a server action starts a fresh session.

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Down`, `j` / `Up`, `k` | `j/k scroll` | one line, stops at the top and at the last full page | `test_rk_scroll` |
| `PgDn` / `PgUp` | help: Page down / up | one page, never past the last full page | `test_rk_scroll` |
| `Home`, `g` / `End`, `G` | help: Top / Bottom | first line / last full page, `[1/2]` / `[2/2]` | `test_rk_scroll` |
| any scroll key, message of one page | | changes nothing, `[1/1]` | `test_rk_short_message` (1 item) |
| `/` | `/ search` | prompt `/text_` on the status line; `Enter` goes to the first match, `Esc` cancels, `Backspace` edits, a miss says `No match: text`, letters are text | `test_rk_search` |
| `n` / `N` | help: next / previous match | the search wraps round at the ends; nothing before a search | `test_rk_search` |
| `ctrl-c` in the prompt | | quits at once | `test_rk_search` |
| `*` `+` `-` `!` | `* star` ... | act at once and say so (`Starred`, `Marked as read`...); a second press undoes; `Already read` / `Already unread` | `test_rk_state_keys` |
| `v` | `v rendered` | toggles rendered / raw source, back to the top | `test_rk_state_keys` |
| `:` | help | the actions menu, `Esc` closes it | `test_rk_state_keys` |
| `?` | help | overlay; `Esc` or `q` close it, other keys do nothing | `test_rk_state_keys` |
| `d`, `Delete` | `d delete` | asks naming the message, No is the default (`Enter`, `Esc`, `n`: `Not deleted`); `y` moves it to the Trash and returns to the list | `test_rk_delete` |
| `r` / `A` / `F` | `r reply` ... | reply / reply-all / forward dialog for this message, back to the list afterwards | `test_rk_reply_forward` |
| `a` | `a save(n)` | two or more attachments: the picker (EMAIL-147); none: `No attachments` | `test_rk_attachments` |
| `Esc`, `q`, `Backspace`, `Left`, `h` | `Esc/q/BS back` | back to the list, cursor kept | `test_rk_back_keys` |
| `ctrl-c` | help: Quit | the program quits at once | `test_rk_ctrl_c` |
| any other key | | changes nothing, shows nothing | `test_rk_unbound_keys` |

Findings of the study, fixed with EMAIL-146 (no separate tickets): `g` / `G`
did nothing; `h` and `Left` did not go back (the lists do); `ctrl-c` in the
`/` prompt did nothing; `PgDn` jumped past the last full page (2 of 14 rows on
a blank screen); `End` left the indicator at `[1/2]`; `j` / `Down` scrolled
until the last line stood alone.  Already right: the other keys of the table
above.  Not changed: the help overlay does not list `Backspace`, `Left`, `h`.

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
| `Home`, `g` / `End`, `G` | help: First / Last message | first / last message (`g` `G` since EMAIL-201) | `test_lk_navigation` |
| `Enter` | `Enter open` | the reader on the cursor message; `Esc`, `q`, `Backspace` there return with the cursor kept | `test_lk_enter_opens_reader` |
| `Esc`, `q`, `Backspace`, `Left`, `h` | `Esc/q/BS folders` | back to the folder browser (`Esc` first drops marks, then the filter; `Left` and `h` since EMAIL-202) | `test_lk_back_keys`, `test_lk_marks` |
| `ctrl-c` | help: Quit | the program quits at once | `test_lk_back_keys` |
| `c` | `c compose` | new message dialog; after it the list returns with the cursor kept | `test_lk_compose_reply_forward` |
| `r` / `A` / `F` | `r reply`, `A r-all`, `F fwd` | the dialog for the cursor message (`Re: ...`, `Fwd: ...`); not on an empty filter result | `test_lk_compose_reply_forward`, `test_lk_filter_without_match` |
| `U` | `U refresh` | lists the folder again, the cursor stays (EMAIL-203) | `test_lk_refresh_and_rules` |
| `l` | `l rules` | the rules editor; `Esc` returns to the list | `test_lk_refresh_and_rules` |
| `Space` / `x` | `Space mark` | marks the row and steps down / clears all marks | `test_lk_marks`, `test_lk_one_message` |
| `/` | help: Filter | filter bar (Tab changes the scope); `Enter` commits, `Esc` clears | `test_tui_list_search_filter`, `test_lk_filter_without_match` |
| `d`, `Delete` | `d trash` | asks first naming the message, No is the default; Trash: `del` | `test_lk_delete_asks`, `test_tui_list_bulk_delete_confirm` |
| `*` `+` `-` `!` | `* star`, `+/- read`, `! junk` | act at once on the marked rows or the cursor row | `test_keys_registry_routine`, `test_keys_junk_toggle_imap` |
| `:` | `: actions` | the action menu (star, read, unread, junk, delete, sync, move, `$Done`) | `test_tui_list_bulk_star_marked`, `test_tui_list_sync_and_refresh` |
| `?` | `? help` | overlay from the key table; scrolls with `j`, `End`; `Esc`/`q` close only it | `test_lk_help_overlay` |
| `n f s m D R a t u v N`, `Right`, `Tab` | | change nothing, show nothing | `test_lk_unbound_keys` |

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
default, the old keys doing nothing, `U`, `l`, the reader round trip.

## Message list, Gmail account (EMAIL-144)

The list opens with `Enter` on a label of the label browser (a label, Inbox,
Unread, Archive, Trash, a search result).  The code of the list is the one of
the IMAP list (above); this section is what Gmail adds.  The table has 19 data
rows at 80x24, the title bar names the range (`a-b of N message(s)`, or
`Showing n of N [filter:...]` with a filter) and the status line ends with
`[cursor/rows]`.  The help (`?`) and the `:` menu are made from the key table
views of `libemail/src/core/key_table.c`: `list-gmail` (a label), `list-gmail-virtual`
(a search result), `list-gmail-archive`, `list-gmail-trash`.  Tests are in
`tests/pty/test_pty_gmail_tui.c`, named `test_gk_*` (80x24; the mock with 45
messages: 1-40 unread, listed first, newest first, then 41-45 read; 41-45 also
carry the label Work, so Work has 5).

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Down`, `j` / `Up`, `k` | `j/k step` | cursor one row, stops at the last / first | `test_gk_navigation` |
| `PgDn` / `PgUp` | `PgDn/PgUp page` | one page (19 rows), clamped | `test_gk_navigation` |
| `Home`, `g` / `End`, `G` | help: First / Last message (also g / also G) | first / last message | `test_gk_navigation`, `test_gk_filter`, `test_gk_one_message` |
| `Enter` | `Enter open` | the reader on the cursor message; `Esc`, `q`, `Backspace` there return with the cursor kept | `test_gk_enter_opens_reader` |
| `Esc`, `q`, `Backspace`, `Left`, `h` | `Esc/q/BS labels` | back to the label browser; `Esc`, `q`, `Left`, `h` drop the marks, then the filter, first; `Backspace` leaves at once | `test_gk_back_keys`, `test_gk_empty_views` |
| `ctrl-c` | help: Quit | the program quits at once | `test_gk_back_keys` |
| `Space` / `x` | `Space mark` | marks the row and steps down (on the last row it stays) / clears all marks | `test_gk_marks`, `test_gk_one_message` |
| `/` | help: Filter | filter bar; the status line counts the rows shown; `Enter` commits, `Esc` clears; with no match nothing can be opened, answered, deleted or marked; `g G h` type text | `test_gk_filter` |
| `c` `r` `A` `F` | `c compose`, `r reply`, `A r-all`, `F fwd` | the dialog for the cursor message; the list returns with the cursor kept | `test_gk_compose_refresh_rules`, `test_gk_filter` |
| `U` | `U refresh` | lists again, the cursor stays | `test_gk_compose_refresh_rules` |
| `l` | `l rules` | the rules editor; `Esc` returns with the cursor kept | `test_gk_compose_refresh_rules` |
| `*` `+` `-` `!` | `* star`, `+/- read`, `! junk` | act at once on the marked rows or the cursor row; the status column and the feedback line follow; a second press undoes | `test_gk_registry_keys`, `test_gk_marks`, `test_keys_junk_toggle_gmail` |
| `d`, `Delete` | `d trash` (`d del` in the Trash) | asks first naming the message ("Move ... to Trash?", in the Trash "Delete ... for good?"), No is the default (`Enter`, `Esc`, `n`); `y` moves it, the row stays marked `D` until the next list; marked rows: one question for all | `test_gk_delete`, `test_gk_trash_view`, `test_keys_d_asks_default_no` |
| `t` | `t labels` | the label picker of the message (studied in EMAIL-145); `Esc` closes it and changes nothing | `test_gk_t_picker`, `test_feedback_t_label_added` |
| `:` | `: actions` | the action menu of what applies in the view (below) | `test_gk_menu_by_view`, `test_keys_gmail_menu` |
| `?` | `? help` | overlay `Message list shortcuts (Gmail)`; scrolls with `j`, `End`; `Esc`, `q`, `Backspace` close only it; other keys change nothing | `test_gk_help` |
| `D a u f n m s v R N e i p o`, `Right`, `Tab`, `Insert`, `F1` | | change nothing, show nothing | `test_gk_unbound_keys` |

The `:` menu of each view (table order): `* + - ! d Delete`, `Start a sync`,
`Move to...`, `t Labels`, then `Archive`, `Restore from Trash`, `Remove this
label`, `Add a label...`, `Remove a label...` as far as they apply:

| View | Archive | Restore from Trash | Remove this label |
|---|---|---|---|
| a label (Inbox, Sent, Work, Unread...) | yes | no | yes (`Label removed: Work`; the second time `Undo: Work restored`) |
| search result | yes | no | no |
| Archive | no | no | no |
| Trash | yes | yes (`Restored to Inbox`, the row is marked `u`) | no |

Tests: `test_gk_menu_by_view`, `test_gk_label_and_archive_views`,
`test_gk_trash_view`, the key table in `tests/unit/test_key_table.c`.

Trash header at 80 columns: `1-1 of 1 message(s) in Trash (1 unread).  auto-delete: 30 days`
(the account in brackets is dropped from the title when it does not fit;
`test_gk_trash_view`).  Empty Trash, Archive or other list: `(empty)` and one
line saying what fills it (Trash: `d on a message moves it here.`; Archive: the
Archive action of the `:` menu; others: `U lists again after a sync.`); the status
line offers only `Esc/q/BS`, `:`, `c`, `U`, `l`, `?`, and every other key does
nothing (`test_gk_empty_views`).  1 message: every cursor key stays on it,
`Space` marks without stepping (`test_gk_one_message`).

Findings of the study, all fixed with EMAIL-144: EMAIL-201 (`g`, `G` did nothing,
IMAP list too), EMAIL-202 (`Left` and `h` did not go back, also in the empty
list), EMAIL-203 (`U` threw the cursor to the first message, IMAP list too),
EMAIL-204 (with a filter the position counted the unfiltered list, `[1/45]` for
14 rows and for none), EMAIL-205 (the Trash header note was cut at 80 columns),
EMAIL-206 (the menu offered Restore outside the Trash, Archive in the Archive,
Remove this label where there is none), EMAIL-207 (an empty list did not say
what fills it).  Already right: `j k Down Up`, `PgUp`/`PgDn` clamping by 19,
`Home`/`End`, `Enter` and the reader round trip, `Esc`/`q`/`Backspace` back and
the marks first, `ctrl-c`, `Space`/`x`/`Esc` marks, `*` `+` `-` `!` with their
status column and feedback, the delete dialog with No as the default and the
named message, the Trash dialog `for good`, `c r A F l` keeping the cursor, the
filter and its no-match case, the old letters `D a u f n` doing nothing, the
help overlay and its closing keys, the Archive messages (`Archived`), `Label
removed` and `Undo`.  Not in this view's scope: the label picker (`t`) does not
close with `q` or `Backspace` and shows no status line (EMAIL-145), the help
overlay has no description or search (EMAIL-181, TUI-59), does not list
`Backspace` (EMAIL-183) and takes no `ctrl-c` (EMAIL-153).

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

## Folder browser, IMAP account, tree and flat (EMAIL-142)

Opens with `Enter` on an account.  Title `Folders - USER  (N)` (N counts the
table rows: eight virtual rows plus the folders; in the flat view the rows of
the level), the table has `rows - 5` data rows (19 at 80x24), the status line
ends with `[cursor/total]`.  The cursor starts on `Unread` (or on the folder
last left); the two section headings are never selectable, and the `Tags /
Flags` heading is on the screen whenever `Unread` is the top row.  The help
(`?`) is made from the key table view `folders`
(`libemail/src/core/key_table.c`).  Tests are in `tests/pty/test_pty_views.c`,
named `test_fk_*` (80x24; the mock lists INBOX and 7 subfolders, or with
`MOCK_IMAP_FOLDERS=N` INBOX and N-1 subfolders `F01`...).  The cell-exact
screens are `folders_help` and `folders_search` in `test_pty_screens.c`.

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Down`, `j` / `Up`, `k` | `up/down step` | cursor one row, stops at the last / first; skips the `Folders` heading | `test_fk_navigation` |
| `PgDn` / `PgUp` | `PgDn/PgUp page` (dropped first at 80 columns) | one page (19 rows), clamped; on the top row the `Tags / Flags` heading is shown | `test_fk_navigation` |
| `Home`, `g` / `End`, `G` | help: First row / Last row | first selectable row (`Unread`) / last row | `test_fk_navigation`, `test_fk_flat` |
| `Enter` | `Enter open/select` | tree: the folder or virtual list under the cursor; flat: a folder with subfolders steps into it (its own row first), a leaf opens; `Esc`, `q`, `Backspace` in the list return to the same row | `test_fk_enter`, `test_fk_flat` |
| `l`, `Right` | help: Open | flat view: as `Enter` | `test_fk_flat` |
| `h`, `Left` | help: Up | flat view: as `Esc` | `test_fk_flat` |
| `t` | `t flat` / `t tree` (the view it switches to) | switches tree / flat, kept in the stored preference; also in the `:` menu as `Tree / flat` | `test_fk_flat`, `test_fk_menu` |
| `Esc`, `q`, `Backspace` | `Esc/q/BS back` (flat, inside a folder: `Esc/q/BS up`) | tree and flat root: back to the accounts; flat inside a folder: one level up | `test_fk_back_keys`, `test_fk_flat` |
| `ctrl-c` | help: none (the help overlay does not take it) | the program quits at once, status 0 | `test_fk_ctrl_c`, `test_fk_search` |
| `/` | help: Search | prompt `Search [Subject]: _`; text (also `q j t`), `Backspace` deletes, `Tab` Subject/From/To/Body, `Enter` searches (empty: cancels), `Esc` cancels, `ctrl-c` quits; the status line shows `Enter search  Tab scope  Esc cancel`; a result returns to the row the search started from | `test_fk_search`, screen `folders_search` |
| `c` | help: Compose | the new message dialog; the browser comes back on the same row (and flat level), also when it was reached from a message list | `test_fk_compose_keeps_row` |
| `:` | `: actions` | menu: `Create a folder...`, `Delete the folder`, `Tree / flat` | `test_fk_menu`, `test_folder_create`, `test_folder_delete_asks` |
| `?` | `? help` | overlay `Folder browser shortcuts`; `Esc`, `q`, `Backspace` close it, other keys change nothing | `test_fk_help`, screen `folders_help` |
| any other key | | changes nothing, shows nothing | `test_fk_unbound_keys` |

0 folders (the server answers `LIST` with nothing): the browser opens with the
virtual rows and says `No folders in this account.  Create one in the : menu.`
(`test_fk_zero_folders`).  1 folder: every cursor key stays on it, `t` and
`Enter` work (`test_fk_one_folder`).  40 folders: `test_fk_navigation`.

Findings of the study: EMAIL-190 (ctrl-c printed `Connection failed`), EMAIL-191
(ctrl-c in the search prompt only closed it), EMAIL-192 (the status line kept the
browser's hints in the prompt), EMAIL-193 (an empty folder list was reported as
a failed connection), EMAIL-194 (the cursor was lost after `c`; from a list the
browser did not come back at all), EMAIL-195 (the cursor was lost after a search
result), EMAIL-196 (the `Tags / Flags` heading scrolled off), EMAIL-197 (`h`,
`l`, `Left`, `Right` in the flat view), EMAIL-198 (`g`, `G`), EMAIL-199 (the help
had no row for `t`) were fixed with EMAIL-142.  EMAIL-200 (the tree is open with no
`+`/`-` markers and `l`/`h` do not open or close) waits for the libtuikit tree
widget (TUI-60); until then `h`, `l`, `Left`, `Right` do nothing in the tree.
Already right: `Enter` on folders and virtual rows, `Esc`/`q`/`Backspace`
back, `j k` and the arrows stopping at the ends, `PgUp`/`PgDn` by 19 rows,
`Home`/`End`, `t` both ways with the hint following, the search editing keys,
`?` and its closing keys, the `:` menu, unbound keys.  Not in this view's
scope: the help overlay takes no `ctrl-c` (EMAIL-153), has no description or
search (EMAIL-181, TUI-59) and does not list `Backspace` (EMAIL-183).

## Gmail label browser and label picker (EMAIL-145)

Opens with `Enter` on a Gmail account.  Title `Labels - USER  (N)`: N counts the
table rows, the labels and the two headings (`Tags / Flags`, `Folders`); the
mock has 12 labels and so 14 rows, 10 without a label of the user's own.  The
table has `rows - 5` data rows (19 at 80x24); the status line ends with
`[cursor/rows]`.  The cursor starts on the label last left (Inbox), the
headings are never selectable, the user's labels are sorted by name, the
system labels (Unread, Flagged, Inbox, Sent, Drafts, the categories, Archive,
Spam, Trash) come in fixed places.  The help (`?`) and the `:` menu are made
from the key table views `labels` (a label of the user's own) and
`labels-system` (any other row) of `libemail/src/core/key_table.c`.  Tests are
in `tests/pty/test_pty_gmail_tui.c`, named `test_lb_*` (browser) and `test_lp_*`
(picker), at 80x24 with the mock's 45 messages (`lb_add_labels` adds 36 more
labels for the paging tests).

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Down`, `j` / `Up`, `k` | `j/k select` | cursor one selectable row, stops at the last / first, skips the headings | `test_lb_navigation` |
| `PgDn` / `PgUp` | help: Page down / Page up | one page (19 rows), clamped; on the first row the `Tags / Flags` heading is on the screen | `test_lb_navigation` (14 and 50 rows) |
| `Home`, `g` / `End`, `G` | help: First label (also g) / Last label (also G) | first selectable row (Unread) / last row (Trash) | `test_lb_navigation`, `test_lb_no_user_labels` |
| `Enter` | `Enter open` | the message list of the label (`Work`: `1-5 of 5 message(s) in Work`); `Esc` there returns to the same row | `test_lb_navigation` |
| `Esc`, `q`, `Backspace`, `Left`, `h` | `Esc/q/BS accounts` | back to the accounts | `test_lb_back_keys` |
| `ctrl-c` | help: none | the program quits at once, status 0, nothing printed | `test_lb_back_keys`, `test_lb_search`, `test_lb_menu` |
| `/` | help: Search | prompt `Search [Subject]: _`; text (also `q j t g`), `Backspace` deletes, `Tab` Subject/From/To/Body, `Enter` searches (empty: cancels), `Esc` cancels, `ctrl-c` quits; the status line shows `Enter search  Tab scope  Esc cancel`; a result returns to the row the search started from | `test_lb_search` |
| `:` | `: actions` | menu: `Create a label...` on every row, `Delete the label` on a label of the user's own only; `j k g G Home End` move, `Enter` chooses, `Esc`, `q`, `Backspace` close; status line `j/k select  Enter choose  Esc/q/BS close [n/N]` | `test_lb_menu` |
| `Create a label...` | prompt `New label name:` | status line `Enter create  Esc cancel`; `Enter` creates, `Esc` cancels, `ctrl-c` quits; `q` is text | `test_lb_menu`, `test_label_browser_create_*` |
| `Delete the label` | `Delete the label X on the server? ...  [Yes] [No]` | No is the default; `y` deletes by id, `n`, `Esc`, `q`, `Backspace` refuse; status line `y yes  n no  Esc/q/BS cancel`; `ctrl-c` quits, nothing deleted | `test_lb_menu`, `test_label_browser_delete_label_uses_id` |
| `?` | `? help` | overlay `Label browser shortcuts`; `Esc`, `q`, `Backspace` close it, other keys change nothing | `test_lb_help_and_status` |
| resize | | redrawn at once, the status line on the new last row | `test_lb_help_and_status` |
| `Space c d t x n r e i D l a u U 1 -`, `Right`, `Tab`, `Insert`, `Delete`, `F1` | | change nothing, show nothing (`c` and `d` are in the `:` menu) | `test_lb_unbound_keys` |

Without a label of the user's own the browser has 10 rows and works the same
(`test_lb_no_user_labels`); there is no 0-row or 1-row browser, the system
labels are always listed.

### Label picker (`t` in a Gmail list or reader)

A modal `Toggle Labels` over the list: a checklist of UNREAD, INBOX, STARRED,
SENT, DRAFTS, then the user's labels sorted by name; the labels the message
has are ticked.  The status line is the picker's own while it is open.

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Down`, `j` / `Up`, `k` | `j/k select` | cursor one row, stops at the ends; `[n/N]` at the right | `test_lp_keys` |
| `Home`, `g` / `End`, `G`, `PgUp`, `PgDn` | | first / last row, a page; the long list scrolls inside the box | `test_lp_keys`, `test_lp_order` |
| `Space` | `Space tick` | ticks / unticks the row, changes nothing yet | `test_lp_keys` |
| `Enter` | `Enter apply` | applies what was ticked (`Label removed: INBOX`, `Labels updated`), nothing when nothing changed | `test_lp_keys`, `test_feedback_t_label_added` |
| `Esc`, `q`, `Backspace` | `Esc/q/BS cancel` | close and discard the ticks | `test_lp_keys`, `test_lp_reader` |
| `ctrl-c` | | quits at once, applies nothing | `test_lp_keys` |
| any other key | | changes nothing | `test_lp_keys` |

The same `q`, `Backspace`, `g`, `G`, `ctrl-c` and status line serve the `:`
menu and its confirmation in every view (`tui_frame_modal_key`,
`tui_frame_modal_status`; `terminal_request_quit()` makes the view underneath
quit on its next read).  A picker with 0 or 1 labels does not exist: the five
system labels are always listed.

Findings of the study, fixed with EMAIL-145: EMAIL-208 (`g`, `G` did nothing),
EMAIL-209 (`Left`, `h` did not go back), EMAIL-210 (the `:` menu and its
confirmation did not close with `q` or `Backspace`), EMAIL-211 (the picker did
not close with `q` or `Backspace`), EMAIL-212 (`g`, `G` in the picker and the
menu), EMAIL-213 (the picker and the menu had no status line), EMAIL-214
(ctrl-c in the search prompt only closed it), EMAIL-215 (the prompts had no
status line), EMAIL-216 (a search result lost the row), EMAIL-217 (the picker
listed the labels in directory order), EMAIL-218 (`Delete the label` was offered
on system labels), EMAIL-219 (ctrl-c in a modal only closed it), EMAIL-220
(ctrl-c in the label browser printed `Connection failed`).  Already right:
`j k Down Up` stopping at the ends and skipping the headings, `PgUp`/`PgDn` by 19
rows and clamped, `Home`/`End`, `Enter` and the return to the same row, `Esc`,
`q`, `Backspace` back, the search prompt's editing keys and scopes, the empty
search, create (prompt, failure notice) and delete (asks first, by id, No the
default, failure notice), `?` and its closing keys, the cursor row marked with
`>`, unbound keys, a resize.  Not in this view's scope: the help overlay has no
description or search (EMAIL-181, TUI-59), does not list `Backspace` (EMAIL-183)
and takes no `ctrl-c` (EMAIL-153); the status line of the browser omits `/` and
the page keys (EMAIL-163).

## Rules list, rule detail and condition editor (EMAIL-148)

Reached with `l` on the message list.  Tested at 80x24 against the mock server
(`tests/pty/test_pty_rules_keys.c`, `./manage.sh pty rules-keys`), each key also
with 0 and 1 rules / conditions.

### Rules list

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Down`, `j` / `Up`, `k` | `j/k navigate` | cursor one row, stops at the ends | `test_rk_list_navigation`, `test_rk_list_zero_and_one` |
| `Home`, `g` / `End`, `G`, `PgUp`, `PgDn` | | first / last rule, a page (rows - 4) | `test_rk_list_navigation` |
| `Enter`, `Right`, `l` | `Enter open` | opens the rule detail; the cursor row is kept on return | `test_rk_list_open_keys` |
| `a` | `a add` | the condition editor, step 1/2 | `test_rk_cond_zero_and_one` |
| `d` | `d delete` | asks with the rule's name, default No | `test_rk_list_delete` |
| `p`, `A` | `p preview`, `A apply now` | preview report; apply asks first | `test_rk_list_preview_apply` |
| `Esc`, `q`, `Backspace`, `Left`, `h` | `Esc/q/BS back` | back to the message list | `test_rk_list_back_keys` |
| `?` | `? help` | overlay; closes with `Esc`, `q`, `Backspace` | `test_rk_list_help_and_status` |
| `ctrl-c` | | quits at once with status 0, also at the questions | `test_rk_list_back_keys`, `test_rk_list_delete` |
| any other key | | changes nothing | `test_rk_list_unbound_keys` |

An empty list shows `(no rules -- press 'a' to add one)`; `d`, `Enter`, `p`
and `A` do nothing there (`test_rk_list_zero_and_one`).

### Rule detail

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `e` | `e edit` | the condition editor prefilled | `test_rk_detail_view` |
| `d` | `d delete` | asks, default No; Yes returns to the list | `test_rk_detail_delete` |
| `Esc`, `q`, `Backspace`, `Left`, `h` | `Esc/q/BS back` | back to the list | `test_rk_detail_back_keys` |
| `?`, `ctrl-c`, other keys | `? help` | as in the list | `test_rk_detail_back_keys`, `test_rk_detail_view` |

### Condition editor (step 1/2) and the actions form (step 2/2)

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Down`, `j` / `Up`, `k`, `Home`, `End`, `g`, `G`, `PgUp`, `PgDn` | `j/k move` | cursor, stops at the ends | `test_rk_cond_navigation` |
| `Enter`, `e` | `Enter/e edit` | edit prompt of the condition; Esc keeps it | `test_rk_cond_edit_keys` |
| `a` | `a add` | prompt for a new condition | `test_rk_cond_zero_and_one`, `test_rk_cond_edit_keys` |
| `d` | `d delete` | removes the condition | `test_rk_cond_zero_and_one` |
| `q` | `q confirm` | goes on to step 2/2 (EMAIL-221) | `test_rk_cond_leave_keys`, `test_rk_actions_form` |
| `Esc`, `Backspace`, `Left`, `h` | `Esc cancel` | cancels the rule | `test_rk_cond_leave_keys` |
| `?` | `? help` | overlay (11 rows) | `test_rk_cond_help_and_unbound` |
| `ctrl-c` | | quits at once with status 0, also in the prompts | `test_rk_cond_leave_keys` |
| any other key | | changes nothing | `test_rk_cond_help_and_unbound` |

A resize redraws all three views (`test_rk_resize`).

Findings of the study, fixed with EMAIL-148 (no ticket): `Backspace`, `Left` and
`h` did not go back in the list or the detail, nor cancel in the condition
editor; `g`, `G` were unbound in all three; the help overlays of the list and
the detail ran without raw mode and could not be closed; ctrl-c at the delete,
apply and Save? questions and in the input lines only answered or closed
instead of quitting; the status lines and help texts did not name `Backspace`
or `h`.  Open: EMAIL-221 (`q` in the condition editor confirms, the policy
says `q` goes back).  Already right: `j k Down Up` stopping at the ends,
`Home`/`End`/pages, `Enter` and the kept row, the empty list, the default-No
questions, the reports, unbound keys, a resize.

## Rule actions form: add step 2/2 and the rule edit (EMAIL-149)

Reached with `q` in the condition editor, for a new rule (`a` in the rules list)
or for the edit of a rule (`e` in the rule detail; the fields are prefilled).
Nine one-line fields, one after another: `Name`, `add-label [1..3]`,
`rm-label [1..3]`, `then-move-folder`, `then-forward-to`.  Tested at 80x24
against the mock server (`tests/pty/test_pty_rules_keys.c`).

| Key | Label | Behaviour | Test |
|---|---|---|---|
| printable keys, `Backspace`, `Left`, `Right`, `Home`, `End` | | edit the field in the input line | `test_rk_form_field_keys`, `test_rk_form_layout_and_edit` |
| `Enter` | `Enter next field` | keeps the field, goes to the next; after the ninth asks `Save? (y/N)` | `test_rk_actions_form`, `test_rk_form_layout_and_edit` |
| `Esc` | `Esc cancel` | in any field: cancels the rule, nothing is written; back to the list (add) or the detail (edit) | `test_rk_actions_form`, `test_rk_form_esc_and_save_keys` |
| `ctrl-c` | | quits at once with status 0, also at `Save?` | `test_rk_actions_form` |
| `Up`, `Down`, `Tab`, `PgUp`, `PgDn`, `F1` | | change nothing; the form has no way back to an earlier field | `test_rk_form_field_keys` |
| `y`, `Y` at `Save?` | | saves the rule; an edit returns to the detail | `test_rk_form_layout_and_edit` |
| any other key at `Save?` (`n`, `N`, `Esc`, `Enter`, ...) | | No: nothing is saved | `test_rk_form_esc_and_save_keys` |
| any key at `Rule name is required` | | returns to the list, nothing is saved | `test_rk_form_name_required_and_resize` |

A resize keeps the form on screen (`test_rk_form_name_required_and_resize`).
Findings of the study: no defect that this ticket fixes; the form has no help
overlay (`?` is a typed character in a field) and no step back to an earlier
field, which the status line (`Enter next field`, `Esc cancel`) states.

## Body editor and review screen (EMAIL-151)

Reached with `c` (new message), `r`, `A`, `F`: the compose dialog (separate
study), then the body editor, then the review screen.  Tested at 80x24 against
the mock servers (`tests/pty/test_pty_review_keys.c`, suite `review-keys`).

### Body editor

Title bar `Message body - To: ... Subject: ...`, status line `Esc save and
continue  Ctrl-S save`.  The editor is libtuikit's `tui_textedit`; a letter is
text here, so there is no `?` help, no `:` menu and no `q`.  Below 80x24 only
`Terminal too small` is shown and nothing is edited, `Esc` still leaves.

| Key | Label | Behaviour | Test |
|---|---|---|---|
| printable keys, `Enter`, `Tab`, paste | | insert text (bracketed paste, wide and accented characters) | `test_ed_unbound_keys_are_text`, `test_body_editor_paste_and_wide` |
| `Left` `Right` `Up` `Down` `Home` `End` `Backspace` `Delete` | | move and delete | `test_ed_cursor_and_delete_keys` |
| `PgUp` / `PgDn` | | one page: the cursor goes to the top / bottom of the view, the next press scrolls | `test_ed_many_lines_paging` |
| `Esc` | `Esc save and continue` | saves the draft and goes to the review screen; with an empty body too | `test_ed_esc_saves_and_leaves`, `test_ed_empty_body_esc` |
| `Ctrl-S` | `Ctrl-S save` | saves the draft, stays; the status line says `saved` until the next key | `test_ed_ctrl_s_note` |
| `ctrl-c` | | saves the draft and quits the program at once | `test_ed_ctrl_c` |
| idle with unsaved changes | | autosave, status line `autosaved` | `test_body_editor_autosave_while_typing` |

0 lines: `test_ed_empty_body_esc`; 1 line: `test_ed_esc_saves_and_leaves`;
60 lines (paging): `test_ed_many_lines_paging`; resize: `test_ed_resize`.

### Review screen

Status line `: actions (send)  e edit headers  b edit body  a add attachment
? help  q discard` (what fits at the width).  Sending is in the `:` menu
(`Send`, then `Send the message to ...?` Yes/No); there is no `s` and no `d`.

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `:` | `actions (send)` | menu `Send` / `Edit headers` / `Remove an attachment`; `Esc` closes only the menu; Remove with nothing attached does nothing | `test_rv_colon_menu` |
| `Send` in the menu | | asks naming the recipients; Yes sends, removes the draft; `Esc` or No returns to the review | `test_rv_send_asks_and_declines`, `test_rv_send_yes` |
| `e` | `edit headers` | the dialog `Edit Message`, prefilled; `Esc` asks `Discard what was typed?`, Yes returns to the review | `test_rv_e_edit_headers` |
| `b` | `edit body` | the body editor with the text kept; the line count follows | `test_rv_b_edit_body` |
| `a` | `add attachment` | prompt `Add Attachment`: `Enter` adds, `Esc` or an empty `Enter` cancels, `Tab` completes, an error (`File not found`) clears when the path is edited | `test_rv_a_attach_prompt`, `test_rv_a_tab_completes` |
| `?` | `help` | overlay `Review shortcuts` from the key table; `Esc` closes only the overlay | `test_rv_help` |
| `q`, `Esc` | `q discard` | asks `Discard draft? To: .., Subject: .., N line(s), N attachment(s) [y/n]`; `y`/`Y` discards and removes the draft, anything else stays | `test_rv_discard_keys` |
| `ctrl-c` | | quits the program at once; the draft stays | `test_rv_ctrl_c` |
| any other key (`s`, `d`, `x`, arrows, `Enter`, `Tab`, space) | | changes nothing | `test_rv_unbound_keys` |

Layout (From, To, Subject, Body count, Attach) at 80x24 and a resize:
`test_rv_layout`, `test_rv_resize`.

Findings of the study, fixed in this ticket (no separate tickets, EMAIL-140
rule): `ctrl-c` in the review screen asked `Discard draft?` like `q` instead of
quitting (now quits, the draft stays); `ctrl-c` in the body editor was ignored
(now saves and quits); after a compose, reply or forward the pause `[Press any
key to return to inbox]` swallowed a pending quit (now skipped).

## Setup wizard and bulk-action prompt (EMAIL-152)

Tested at 80x24 against the mock server (`tests/pty/test_pty_views.c`, suite
`views`).

### Setup wizard

Reached with `n` (new), `i` (IMAP settings), `e` on the accounts screen, and at
the first start without any account.  It is not a key-driven screen: the
terminal is cooked, one line per answer, so a key is text and `Enter` ends the
line.  Prompts: `Account type` (`Choice [1]:`), `IMAP Host`, `IMAP Port [993]`,
`Email Username`, `Email Password` (hidden), `Default Folder [INBOX]`,
`SMTP Host [Enter=skip]`.

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Enter` | `[1]`, `[993]`, `[INBOX]`, `Enter=skip` | takes the default shown in the prompt; at SMTP it skips; an empty password is said to be empty | `test_wk_wizard_imap_flow` |
| `Space`, `m` `r` `u` `f` `F` `l` `L` `D` | | text: at `Account type` each is refused (`Unknown account type 'x'`) and the question is asked again | `test_wk_wizard_type_prompt` |
| `Esc` | | text, not cancel (EMAIL-223) | `test_wk_wizard_type_prompt`, `test_wk_wizard_imap_flow` |
| `ctrl-c` | | text, echoed `^C`, not quit (EMAIL-223) | `test_wk_wizard_ctrl_c` |
| `Ctrl-D` | | end of input: `Wizard aborted.`, back to the unchanged accounts list (at the first start: `Configuration aborted. Exiting.`, exit status 1); also at the hidden password | `test_wk_wizard_type_prompt`, `test_wk_wizard_imap_flow`, `test_wk_wizard_first_run` |

0 accounts: `test_wk_wizard_first_run`; 1 account: all the others.

### Bulk-action prompt (`d` on a list with marks)

`Space` marks the row under the cursor (`[N marked]` in the title; the cursor
does not move for one message).  `d` then asks once for all marked messages:
`Move N marked messages to Trash?` with `[ Yes ]  [ No ]`, status line `y yes
n no  Esc/q/BS cancel`.  With one message the question names it, not a count.
With no marks `d` acts on the row under the cursor.

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `y` | `y yes` | does it (`Moved to Trash`), the marks go | `test_bk_prompt_yes` |
| `n`, `Esc`, `q`, `Backspace` | `n no`, `Esc/q/BS cancel` | `Not deleted`, marks and cursor stay | `test_bk_prompt_keys` |
| `Enter` | | takes the button in focus; No at first (`Not deleted`) | `test_bk_prompt_keys`, `test_bk_prompt_buttons` |
| `Left` `Right` `Tab` | not on the status line | move the focus between Yes and No | `test_bk_prompt_buttons` |
| `ctrl-c` | | cancels and quits the program at once | `test_bk_prompt_keys` |
| any other key (`Space` `m` `r` `u` `f` `F` `l` `L` `D` `j` `x` `d`, arrows, PgUp/PgDn, Home/End, F1) | | ignored, the question stays | `test_bk_prompt_keys` |

Before the prompt, with rows marked, `m` `u` `f` `D` `L` do nothing, `Space`
on a marked row unmarks it, and `Esc` drops the marks first
(`test_bk_marks_and_old_letters`).  1 message: `test_bk_prompt_one_item`; 0
messages: `Space`, `d` and the letters do nothing (`test_bk_prompt_no_item`).

Findings of the study.  Fixed in this ticket (no separate ticket): quitting
with ctrl-c after the server had dropped an idle connection killed the program
with SIGPIPE (the LOGOUT write went to a closed socket); IMAP writes now block
SIGPIPE and fail instead (`test_bk_quit_after_server_dropped`).  Ticketed:
EMAIL-223 (wizard prompts: `Esc` and ctrl-c are text, and after one given-up
wizard every later wizard of the session aborts at once).  Document only: the
button keys `Left` `Right` `Tab` are not on the prompt's status line.
