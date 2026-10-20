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
