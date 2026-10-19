# email-tui: element inventory

Every view of `email-tui` with its elements, keys and the test that compares
it cell for cell (EMAIL-34).  The tests are in `tests/pty/test_pty_screens.c`;
the expected pictures and style maps are in `tests/pty/screens_expected.h`.  A
terminal of 100x24, `TZ=UTC`, a mock IMAP server with three messages and a
fresh home per view are fixed, so every picture is reproducible.

Style legend of the maps: `H` bold white on blue (header bar), `r` reverse
(status bar), `B` bold+reverse, `b` bold, `d` dim, `g` green, `G` reverse
green, `R` red, space default.  The selection of a table (EMAIL-79, libtuikit
`tui_table`) is a dark blue background (palette 17) with a `>` in column 0:
`S` bright white text on it, `A` the bold `>`, `n` a green cell kept as bright
green and bold.  A golden row starting with `~` holds a port number: its digit
runs are read as `#`.  The expectations do not depend on where the work tree
is (EMAIL-174): a row starting with `%` is compared by its start, and the byte
`\001` in it stands for any run of characters (the start of a path); a row
`==` stands for any number of blank rows, the rows after it being compared
from the bottom of the screen (a status bar); a golden cursor column of `-1`
is the end of the text of its row.

## Views

| View (test) | Reached by | Elements | Keys named on the screen |
|---|---|---|---|
| Accounts (`accounts`) | start | header bar with count; column heads Unread, Flagged, Account, Type, Server, Send via; ruler; one row per account, `>` and the dark blue selection on the cursor row; status bar | ↑↓ Enter n d i e Esc |
| Accounts help (`accounts_help`) | `h` on accounts | boxed overlay (libtuikit `tui_dialog_help`, EMAIL-110) over the view: title, key in bold and description rows; Esc, q and Backspace close it, the movement keys scroll it, any other key changes nothing | Esc q BS |
| Delete asked (`account_delete_ask`) | `d` on accounts | information line `Delete account NAME? (y/N)`, the list unchanged | y, any other key |
| Account deleted (`account_delete`) | `d`, `y` on accounts | "No accounts configured." | n |
| Wizard type (`wizard_type`) | `n` on accounts | plain prompt: account type menu, `Choice [1]:` | 1, 2 |
| Folder browser (`folders`, `folders_flat`) | Enter on an account | header bar with count; heads Unread, Flagged, Folder, Total; "Tags / Flags" section (dim, 6 virtual rows) and "Folders" tree; cursor row selected (dark blue, `>`); status bar with `[n/N]` | ↑↓ PgUp/PgDn Enter t / h : Esc q BS; `:` menu: create a folder (name asked), delete the folder (asks, No default) |
| Folder help (`folders_help`) | `h` | overlay (as the accounts help) | Esc q BS |
| Folder search (`folders_search`) | `/` | filter prompt line | text, Enter, Esc |
| Message list (`list`, `list_second_row`, `list_narrow`) | Enter on INBOX | header bar "1-3 of 3 message(s) in INBOX (3 unread) [account]."; heads Date, Sts, Subject, From; ruler; rows with status column (`N` green = unread); status bar; marked rows carry `*` and the header adds `[N marked]` (US-86) | ↑↓ PgUp/PgDn Enter c r F A n f j d D s U l / h Esc q BS |
| Bulk action prompt (US-86) | `m` on the message list | information line `N [marked ]message(s): r=read u=unread f=star F=unstar m=move D=trash  Esc=cancel`; Gmail adds `l=+label L=-label`; `D` asks `(y/N)` | r u f F l L m D Esc |
| Empty folder (`list_empty`) | Enter on an empty folder | header "0 of 0 message(s) …", "(empty)" | |
| List help (`list_help`) | `h` | overlay with key rows and the Sts legend | Esc q BS |
| List search (`list_search`) | `/` | filter line "Filter [Subject|From|To|Body|Since|Before]: _  Showing n of N" | text (Since/Before take YYYY-MM-DD, as the CLI --since/--before), Tab, Enter, Esc |
| Message reader (`reader`, `reader_source`, `reader_search`) | Enter on a message | header block From, Subject, Date, UID, File, DMARC; rule; body; status line "-- [1/1] …" with the keys | ↑↓ PgUp/PgDn Home End r F f n d D a A v / h Esc q BS |
| Reader help (`reader_help`) | `h` | overlay (as the accounts help) | Esc q BS |
| Rules editor (`rules_empty`, `rules_list`, `rules_second`) | `l` on the list | title "Rules for account (n rules)"; "(no rules …)" or one row per rule, `>` and the selection on the cursor; status line (`j/k navigate  Enter open  a add  d delete  p preview  A apply now  Esc/q back  h help`) | j k ↑↓ Enter a d Esc q |
| Rule detail (`rule_detail`) | Enter on a rule | "Rule:", "Account:", `when:` and the `then-*` rows; status line (`e edit  d delete  Esc/q back  h help`) | e d Esc q |
| Rule edit (`rule_edit_form`) | `e` on a rule | condition list editor step 1/2 prefilled | j k Enter e a d q Esc |
| Rule add, conditions (`rule_add_when`) | `a` in the rules editor | step 1/2 title, status line, "(empty — press 'a' …)" | a q Esc |
| Rule add, actions (`rule_add_actions`) | `q` in step 1 | step 2/2 title; Name; add-label 1–3; rm-label 1–3; then-move-folder; then-forward-to | Enter Esc |
| Compose dialog (`compose_dialog`, `compose_typed`, `compose_empty_to`) | `c` | "New Message"; To Cc Bcc Subject Attach fields; guard line "To: is empty …" (red); hint | Tab Shift-Tab ; Enter Esc |
| Gmail label browser (`gmail_labels`, `gmail_label_user`) | Enter on a Gmail account | header "Labels — account (N)"; heads Count, Label; "Tags / Flags" (Unread, Flagged, user labels) and "Folders" (Inbox, Sent, Drafts, Archive, Spam, Trash with "auto-delete: 30 days"); selected row dark blue with `>`; counts of what is synced | ↑↓ Enter c d h Esc |
| Gmail label views (`gmail_unread`, `gmail_flagged`, `gmail_inbox`, `gmail_inbox_second`, `gmail_archive`, `gmail_trash`, `gmail_starred_filled`, `gmail_trash_filled`, `gmail_archive_filled`, `gmail_user_label`) | Enter on a label | header "N-M of T message(s) in LABEL (U unread) [account]."; Trash adds "⚠ auto-delete: 30 days" and the keys `D=del u=restore`; empty label shows "(empty)"; star shown as ★ in Sts | ↑↓ Enter d D a u f n c r Esc |
| Gmail pending marks (`gmail_inbox_d`, `gmail_inbox_trash_mark`, `gmail_inbox_archive_mark`, `*_off`) | `d`, `D`, `a` on a row | mark `d` (bold yellow, whole row yellow when the cursor is elsewhere), `D` (red row), feedback line "Label removed: INBOX" / "Moved to Trash" / "Archived"; archive also clears the unread `N` | d D a |
| Bulk-action states (`list_marked`, `list_marked_two`, `list_bulk_prompt`, `list_bulk_cursor_prompt`, `list_move_prompt`, `list_delete_ask`, `list_bulk_done`) | `Space`, `m` on the message list (US-86) | `*` in the mark column, `[N marked]` in the header; the prompt line `N [marked ]message(s): r=read …  Esc=cancel`; `Move to folder: <text>`; `Move to Trash N message(s)? (y/N)`; the result line `N message(s) starred` | Space x m r u f F D Esc |
| Gmail label picker (`gmail_picker`, `gmail_picker_second`, `gmail_picker_toggled`, `gmail_picker_done`) | `t` on a Gmail message | libtuikit checklist "Toggle Labels" as an overlay (EMAIL-110): rows `[x]`/`[ ]` per label; Space ticks, Enter applies what was ticked, Esc leaves everything as it was; the feedback line names the change (`Label removed: INBOX`) | ↑↓ Space Enter Esc |
| Reader with attachments (`att_reader`) | Enter on the multipart message | the reader plus `Attach:  2 files: notes.txt, data.bin` and `a save(2)` in the status line; the rendered HTML body keeps its bold/italic/underline/strike/colour |  a A |
| Attachment picker (`att_picker`, `att_picker_second`) | `a` in the reader | "Attachments (N):", one row per file (name, type, size), selected row dark blue with `>`; status line `↑↓ select  Enter choose  Esc/q/BS back  [n/N]` | ↑↓ Enter Esc |
| Save prompts and result (`att_save_prompt`, `att_saved`, `att_save_all`) | Enter in the picker, `:` menu | `Save as: <path>` (line editor, scrolls), `Saved: <path>`, `Save all to: <dir>`; the golden pictures compare these rows by their start only (paths differ per machine) | Enter Esc |
| Review states (`review_full`, `review_long_to`, `review_attach_ask`, `review_attach_removed`, `review_send_failed`) | `s`, `a`, `d` in the review | Cc, Bcc and Attach rows when present (`Attach:  (none)` otherwise); a To, Cc or Bcc list that does not fit the width is broken between addresses (after the `;` or `,`), the continuation line indented to the value column, never inside an address (EMAIL-107; `w80_review_long_to`, `w140_review_long_to`); `Add Attachment` / `Path:` prompt; a failed send appends `Sending...`, the SMTP error, `Saved to Drafts (will retry on next sync).` below the review | s e b a d q |
| Address drop-down (`dd_many`, `dd_one`, `dd_second`, `dd_none`, `dd_wide`, `dd_accepted`) | typing 2+ characters in To/Cc/Bcc | up to five candidate rows under the field, each a `tui_table` row of address (cyan) and name, drawn as an overlay (EMAIL-110), `>` and the selection style on the chosen row; none when nothing matches; Down moves, Enter takes the address | text ↓ ↑ Enter ; Tab |
| Path completion (`path_many`, `path_one`) | typing a path in Attach | the typed path scrolls in the field; candidate file names below as table rows (EMAIL-110); status line `Tab complete  Enter add  Enter (empty) ok  d remove last  Esc cancel` | Tab Enter d |
| Connection errors (`err_connect`, `err_login`, `err_tls`) | Enter on an account whose server cannot be used | the specific error line (`ERROR: connect to …`, `localhost rejected the login for …` with the server's words, `SSL handshake failed …`), and `Press any key to return to accounts...`; the generic `Connection failed.` causes block is printed only when no specific error line was written (EMAIL-105) | any key |
| Sync and feedback (`list_syncing`, `list_new_mail`, `list_star_feedback`) | `s` and `f` on the message list | the header suffix `↻ syncing...`, then `✉ New mail may have arrived!  U=refresh`; a one-word feedback line (`Starred`) above the key bar | s U f |
| Setup wizard steps (`wiz_imap_host`, `wiz_imap_port`, `wiz_imap_user`, `wiz_imap_pass`, `wiz_imap_masked`, `wiz_imap_folder`, `wiz_smtp_host`, `wiz_smtp_port`, `wiz_gmail`, `wiz_bad_type`, `wiz_imap_done`) | `n` on accounts, then the answers | plain prompts one under the other (IMAP host with `→ using imaps://…`, port, username, password echoed as `*` with `→ password updated (N characters)`, folder, SMTP host/port); the account type question takes `1` (or Enter) for IMAP and `2` for Gmail, any other answer is refused with `Unknown account type '…'` and asked again (EMAIL-106); the Gmail path asks for the address; `wiz_imap_done` is the accounts list with the new account selected | text, Enter |
| Rules editor help (`rules_help`, `rule_detail_help`, `rule_when_help`) | `h` or `?` in the rules list, a rule, the condition list (EMAIL-95) | the same boxed overlay as the other screens: title ("Rules shortcuts", "Rule shortcuts", "Conditions shortcuts"), key/description rows | Esc q BS |
| Rules run (`rules_apply_ask`, `rules_preview`, `rules_applied`) (EMAIL-126) | `A` (asks `Apply the rules to the cached messages now? (y/N)`) and `p` in the rules editor | a screen of its own: header bar `Rules preview (dry run, nothing changed): ACCOUNT` or `Rules applied: ACCOUNT`, then the report of `rules apply --dry-run --verbose` (a `[dry-run]` / `[rule]` line per match, the count line); status line `any key back` | A y p, any key |
| Rules editor states (`rules_delete_ask`, `rules_deleted`, `rule_detail_delete_ask`, `rule_when_typed`, `rule_when_added`, `rule_when_bad`, `rule_step2_field`) | `d`, `a` in the rule views | the delete question `Delete "NAME"? (y/N)` on the last row; the typed condition on the `new:` line; a condition that does not parse is refused with `Not a valid condition: … (e.g. from:*@example.org)` on the last row and not added; step 2 shows the typed field | d y a Enter q |
| Body editor (`body_editor`, `body_editor_typed`) | Enter on the compose dialog (US-87) | header bar "Message body — To: … Subject: …"; the text (tui_textedit, scrolls both ways, cursor at the text cursor); status line `Esc save and continue  Ctrl-S save`, `saved`/`autosaved` in its place after a save | text, Enter, arrows, Esc, Ctrl-S |
| Review (`review`, `review_discard_ask`) | Esc in the body editor | header "Review: Message"; dim labels From, To, Subject, Body (line count), Attach; status line `s send  e edit headers  b edit body  a add attachment  d remove last  q discard`; `q` asks `Discard draft? To: …, Subject: …, N line(s), M attachment(s) [y/n]` | s e b a d q |

## The status line (EMAIL-98)

Every full-screen view ends in one reverse-video row, drawn by libtuikit's
`tui_statusline` (`libemail/src/core/status_line.c`); there is no information
row above it any more.

- **Hints** are `key label` pairs (`Enter open`, `Esc/q/BS folders`).  When
  the terminal is too narrow whole hints are dropped, lowest priority first;
  the way back (`Esc/q/BS …`), the open key and the position stay.
- **Position** `[n/N]` is right-aligned and always kept; with no items it is
  not shown.  The reader shows its page `[page/pages]`.
- **Help**: `?` help (`h help` in the label browser and the accounts list)
  is the last hint.
- **Message**: an action's feedback (`Moved to Trash`, `Label removed: …`),
  a search miss in the reader, a prompt (`Delete account NAME? (y/N)`, the
  bulk menu and its confirmations) and a flash notice replace the hints on
  the same row until the next key; then the hints are back.
- The compose dialog and its attachment prompt, the body editor, the review
  and the rules editor draw the same status line on the last row (EMAIL-111):
  hints with priorities that drop out on a narrow terminal, a message in their
  place (the condition list's refusal, the editor's `saved`).  The delete
  question of the rules editor replaces it for the moment.

## Not covered by a whole-screen comparison yet

The Gmail label browser and label views (`test-pty-gmail-tui`), the attachment
picker, the post-compose review screen, the pre-compose dialog's address
drop-down, the sync and refresh status messages and every error dialog keep
their element-level assertions in the other PTY suites.

## Findings recorded while writing the pictures

Behaviour as it is today is what the pictures encode; each item below is a
defect or inconsistency the pictures make visible, left for the owner to ticket.

1. Accounts: the Server column cut the address after the colon, the port was
   never visible.  Fixed with EMAIL-80: the Server and Send via cells are cut
   at their start (`…aps://localhost:41145`), so the port stays.
2. Folder browser, tree: an empty row followed the entries with wide
   characters (`中`, `😀`); the rows below were shifted.  Gone since the
   folders are a `tui_table` view (EMAIL-79); EMAIL-81 pins it with a test.
3. Folder browser, flat view (`t`): only `INBOX/` is listed, and inside it
   only the subfolders, so INBOX's own messages could not be opened.  Fixed
   with EMAIL-82: inside a folder the folder itself is the first row.
4. Header bar: the rules editor, rule detail, rule add/edit steps and the
   compose dialog did not use the bold white-on-blue header bar of
   `policies/tui-interface-model.md`; the compose dialog's top row was reverse
   video, the others plain.  Fixed with EMAIL-83: these screens, the review
   screen and the attachment picker draw the bar (`H` in the style maps).
5. Accounts: `d` deleted the account at once, with no confirmation.  Fixed
   with EMAIL-84: the information line asks `Delete account NAME? (y/N)`;
   only `y` or `Y` deletes (`account_delete_ask`).
6. Message list: the From column was cut in the middle of the address
   (`<sender3@example.c`) at 100 columns.  Fixed with EMAIL-79/85: the column
   is a table cell cut with an ellipsis, and where it is too narrow for
   `Name <address>` it shows the name alone (`list_narrow`).
7. Help overlays (fixed with EMAIL-34): rows were padded by bytes, so rows
   with `↑ / ↓` were misaligned and a multi-byte cut was possible; the
   key rows still said `ESC / q  Quit` and `Backspace` after EMAIL-37.
8. Narrow terminal (80x24, `w80_*`): the bottom key bars of the accounts
   screen and the message list are cut mid-hint (`(*keeps loc`, `F=f`)
   instead of being shortened.  Not changed; the pictures pin the present state.
9. Eight-colour terminal (`TERM=linux`, `*_8colour`): the programs treat
   every `TERM` but unset, empty and `dumb` as ANSI (EMAIL-108); the
   selection is still the dark-blue palette-17 row with `>`, so on a
   terminal without it the `>` alone marks the row.

### TERM (EMAIL-108)

`TERM` unset, empty or `dumb` means a terminal without escape sequences.
`email-tui` (full-screen interface) then refuses to start: exit status 1 and a
plain message on stderr that names the need and points to `email-cli`; its
`send` subcommand, `--help` and `--version` work.  The other programs
(`email-cli`, `email-cli-ro`, `email-sync`, `email-import-rules`) drop the CSI
escape sequences (colour, cursor, erase-line) from stdout and stderr, so the
text of the output is the same without the codes.  Any other value, known or
not, is taken as an ANSI terminal.  Checked by `tests/functional/run_term.sh`
and the unit test of `terminal_strip_csi()` / `terminal_plain_output_if_needed()`.

## State matrix (EMAIL-96)

`w80_*` and `w140_*` pictures: accounts, folders, list, reader, list help,
compose form, body editor, review and rules list at 80 and 140 columns (the
default is 100); `list_third_row`, `folders_second_row` (cursor on a later
row); `list_wide_subject`, `reader_wide_subject` (wide characters),
`list_long_subject` (cut with an ellipsis); `accounts_8colour`,
`folders_8colour`, `list_8colour`, `list_8colour_third`, `reader_8colour`.

## Resize and the minimum size (EMAIL-97)

`terminal_read_key()` (platform/posix/terminal.c) waits for input or
`SIGWINCH` with `ppoll()`: a resize returns "ignored", so every screen redraws
at the new size at once, without a key.  A resize that arrives while a screen
is being drawn is kept in a flag and is not lost.  The reader lays the message
out again at the new width and keeps its position; the list, folder browser and
the other screens re-read the size on every pass.  The body editor
(`body_editor.c`) and the single-line editor (`input_line.c`) do the same
through `key_events_next()` (`core/key_events.c`), which reports SIGWINCH as
`TUI_EV_RESIZE`.

Below 80x24 the screen is replaced by `Terminal too small (WxH).` /
`Needs 80x24. Resize, or Esc to leave.`; keys other than Esc and Ctrl-C are
swallowed until the window is large enough again, and nothing is edited while
nothing is drawn.  Pictures: no cell-exact golden (the notice is two lines);
the behaviour is in `test_resize_*`, `test_start_below_minimum_shows_notice`
and `test_body_editor_resize_and_too_small`.  The single-line editor is
drawn again at once at the new width (EMAIL-109: the text scrolls so that the
cursor stays visible; `test_resize_redraws_the_line`).

## Keys that change state (EMAIL-175)

Policy: `policies/tui-interface-model.md`, "Actions that change state: the
action menu".  The keys of the message list, the message reader, the Gmail
label browser and the review screen are rows of one libtuikit key table
(`libemail/src/core/key_table.c`).  The key handler asks the table
(`ekeys_find`), the help overlay is made from it (`ekeys_help`) and `:` opens
the menu of the view (`ekeys_menu`); the table is validated when `email-tui`
starts and the program refuses to start with a table that breaks the policy.

| Key | Action | Class |
|---|---|---|
| `*` | toggle the star | routine, at once |
| `+` / `-` | mark read / unread | routine, at once |
| `!` | toggle junk | routine, at once |
| `d`, `Delete` | move to Trash (for good in Trash) | serious: a dialog names sender and subject, default No |
| `t` | Gmail label picker | routine |
| `:` | the action menu | lists every state-changing action |

These act on the marked rows, or on the row under the cursor.  In the `:` menu
only: start a sync (a dialog names the account and folder), send (review
screen; names the recipients), delete a label (names it), create a label,
archive, restore from Trash, remove a label, add/remove a named label, move,
toggle Done, save all attachments.  `j`/`k` move on every list; `h` is not a
help key; Esc in the compose dialog asks before it discards typed text.
