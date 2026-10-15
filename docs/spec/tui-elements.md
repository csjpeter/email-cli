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
runs are read as `#`.

## Views

| View (test) | Reached by | Elements | Keys named on the screen |
|---|---|---|---|
| Accounts (`accounts`) | start | header bar with count; column heads Unread, Flagged, Account, Type, Server, Send via; ruler; one row per account, `>` and the dark blue selection on the cursor row; status bar | ↑↓ Enter n d i e Esc |
| Accounts help (`accounts_help`) | `h` on accounts | boxed overlay, title, key/description rows, footer "Press any key to close" | any key |
| Delete asked (`account_delete_ask`) | `d` on accounts | information line `Delete account NAME? (y/N)`, the list unchanged | y, any other key |
| Account deleted (`account_delete`) | `d`, `y` on accounts | "No accounts configured." | n |
| Wizard type (`wizard_type`) | `n` on accounts | plain prompt: account type menu, `Choice [1]:` | 1, 2 |
| Folder browser (`folders`, `folders_flat`) | Enter on an account | header bar with count; heads Unread, Flagged, Folder, Total; "Tags / Flags" section (dim, 6 virtual rows) and "Folders" tree; cursor row selected (dark blue, `>`); status bar with `[n/N]` | ↑↓ PgUp/PgDn Enter t / h Esc q BS |
| Folder help (`folders_help`) | `h` | overlay | any key |
| Folder search (`folders_search`) | `/` | filter prompt line | text, Enter, Esc |
| Message list (`list`, `list_second_row`, `list_narrow`) | Enter on INBOX | header bar "1-3 of 3 message(s) in INBOX (3 unread) [account]."; heads Date, Sts, Subject, From; ruler; rows with status column (`N` green = unread); status bar; marked rows carry `*` and the header adds `[N marked]` (US-86) | ↑↓ PgUp/PgDn Enter c r F A n f j d D s U l / h Esc q BS |
| Bulk action prompt (US-86) | `m` on the message list | information line `N [marked ]message(s): r=read u=unread f=star F=unstar m=move D=trash  Esc=cancel`; Gmail adds `l=+label L=-label`; `D` asks `(y/N)` | r u f F l L m D Esc |
| Empty folder (`list_empty`) | Enter on an empty folder | header "0 of 0 message(s) …", "(empty)" | |
| List help (`list_help`) | `h` | overlay with key rows and the Sts legend | any key |
| List search (`list_search`) | `/` | filter line "Filter [Subject|From|To|Body]: _  Showing n of N" | text, Tab, Enter, Esc |
| Message reader (`reader`, `reader_source`, `reader_search`) | Enter on a message | header block From, Subject, Date, UID, File, DMARC; rule; body; status line "-- [1/1] …" with the keys | ↑↓ PgUp/PgDn Home End r F f n d D a A v / h Esc q BS |
| Reader help (`reader_help`) | `h` | overlay | any key |
| Rules editor (`rules_empty`, `rules_list`, `rules_second`) | `l` on the list | title "Rules for account (n rules)"; "(no rules …)" or one row per rule, `>` and the selection on the cursor; hint line | j k ↑↓ Enter a d Esc q |
| Rule detail (`rule_detail`) | Enter on a rule | "Rule:", "Account:", `when:` and the `then-*` rows; hint line | e d Esc q |
| Rule edit (`rule_edit_form`) | `e` on a rule | condition list editor step 1/2 prefilled | j k Enter e a d q Esc |
| Rule add, conditions (`rule_add_when`) | `a` in the rules editor | step 1/2 title, hint, "(empty — press 'a' …)" | a q Esc |
| Rule add, actions (`rule_add_actions`) | `q` in step 1 | step 2/2 title; Name; add-label 1–3; rm-label 1–3; then-move-folder; then-forward-to | Enter Esc |
| Compose dialog (`compose_dialog`, `compose_typed`, `compose_empty_to`) | `c` | "New Message"; To Cc Bcc Subject Attach fields; guard line "To: is empty …" (red); hint | Tab Shift-Tab ; Enter Esc |
| Gmail label browser (`gmail_labels`, `gmail_label_user`) | Enter on a Gmail account | header "Labels — account (N)"; heads Count, Label; "Tags / Flags" (Unread, Flagged, user labels) and "Folders" (Inbox, Sent, Drafts, Archive, Spam, Trash with "auto-delete: 30 days"); selected row dark blue with `>`; counts of what is synced | ↑↓ Enter c d h Esc |
| Gmail label views (`gmail_unread`, `gmail_flagged`, `gmail_inbox`, `gmail_inbox_second`, `gmail_archive`, `gmail_trash`, `gmail_starred_filled`, `gmail_trash_filled`, `gmail_archive_filled`, `gmail_user_label`) | Enter on a label | header "N-M of T message(s) in LABEL (U unread) [account]."; Trash adds "⚠ auto-delete: 30 days" and the keys `D=del u=restore`; empty label shows "(empty)"; star shown as ★ in Sts | ↑↓ Enter d D a u f n c r Esc |
| Gmail pending marks (`gmail_inbox_d`, `gmail_inbox_trash_mark`, `gmail_inbox_archive_mark`, `*_off`) | `d`, `D`, `a` on a row | mark `d` (bold yellow, whole row yellow when the cursor is elsewhere), `D` (red row), feedback line "Label removed: INBOX" / "Moved to Trash" / "Archived"; archive also clears the unread `N` | d D a |
| Bulk-action states (`list_marked`, `list_marked_two`, `list_bulk_prompt`, `list_bulk_cursor_prompt`, `list_move_prompt`, `list_delete_ask`, `list_bulk_done`) | `Space`, `m` on the message list (US-86) | `*` in the mark column, `[N marked]` in the header; the prompt line `N [marked ]message(s): r=read …  Esc=cancel`; `Move to folder: <text>`; `Move to Trash N message(s)? (y/N)`; the result line `N message(s) starred` | Space x m r u f F D Esc |
| Gmail label picker (`gmail_picker`, `gmail_picker_second`, `gmail_picker_toggled`, `gmail_picker_done`) | `t` on a Gmail message | boxed popup "Toggle Labels" (reverse video, cursor row bold), rows `[x]`/`[ ]` per label, dim footer `↑↓=move  Enter=toggle  ESC=done`; after Esc the feedback line names the change (`Label removed: INBOX`) | ↑↓ Enter Space Esc |
| Reader with attachments (`att_reader`) | Enter on the multipart message | the reader plus `Attach:  2 files: notes.txt, data.bin` and `a=save  A=save-all(2)` in the status bar; the rendered HTML body keeps its bold/italic/underline/strike/colour |  a A |
| Attachment picker (`att_picker`, `att_picker_second`) | `a` in the reader | "Attachments (N):", one row per file (name, type, size), selected row dark blue with `>`; hint line `↑↓=select  Enter=choose  Esc/q/BS=back` | ↑↓ Enter Esc |
| Save prompts and result (`att_save_prompt`, `att_saved`, `att_save_all`) | Enter in the picker, `A` | `Save as: <path>` (line editor, scrolls), `Saved: <path>`, `Save all to: <dir>`; the golden pictures compare these rows by their start only (paths differ per machine) | Enter Esc |
| Review states (`review_full`, `review_long_to`, `review_attach_ask`, `review_attach_removed`, `review_send_failed`) | `s`, `a`, `d` in the review | Cc, Bcc and Attach rows when present (`Attach:  (none)` otherwise); a long To list wraps at the terminal edge; `Add Attachment` / `Path:` prompt; a failed send appends `Sending...`, the SMTP error, `Saved to Drafts (will retry on next sync).` below the review | s e b a d q |
| Address drop-down (`dd_many`, `dd_one`, `dd_second`, `dd_none`, `dd_wide`, `dd_accepted`) | typing 2+ characters in To/Cc/Bcc | up to five candidate rows under the field, each `▶ address   name` (cyan marker, the chosen row in reverse); none when nothing matches; Down moves, Enter takes the address | text ↓ ↑ Enter ; Tab |
| Path completion (`path_many`, `path_one`) | typing a path in Attach | the typed path scrolls in the field; candidate file names below with the same marker; hint line `Tab=complete … Enter=add  d=remove last` | Tab Enter d |
| Connection errors (`err_connect`, `err_login`, `err_tls`) | Enter on an account whose server cannot be used | the specific error line (`ERROR: connect to …`, `localhost rejected the login for …` with the server's words, `SSL handshake failed …`), then the generic `Connection failed.` causes and `Press any key to return to accounts...` | any key |
| Sync and feedback (`list_syncing`, `list_new_mail`, `list_star_feedback`) | `s` and `f` on the message list | the header suffix `↻ syncing...`, then `✉ New mail may have arrived!  U=refresh`; a one-word feedback line (`Starred`) above the key bar | s U f |
| Setup wizard steps (`wiz_imap_host`, `wiz_imap_port`, `wiz_imap_user`, `wiz_imap_pass`, `wiz_imap_masked`, `wiz_imap_folder`, `wiz_smtp_host`, `wiz_smtp_port`, `wiz_gmail`, `wiz_bad_type`, `wiz_imap_done`) | `n` on accounts, then the answers | plain prompts one under the other (IMAP host with `→ using imaps://…`, port, username, password echoed as `*` with `→ password updated (N characters)`, folder, SMTP host/port); the Gmail path asks for the address; `wiz_imap_done` is the accounts list with the new account selected | text, Enter |
| Rules editor help (`rules_help`, `rule_detail_help`, `rule_when_help`) | `h` or `?` in the rules list, a rule, the condition list (EMAIL-95) | the same boxed overlay as the other screens: title ("Rules shortcuts", "Rule shortcuts", "Conditions shortcuts"), key/description rows, "Press any key to close" | any key |
| Rules editor states (`rules_delete_ask`, `rules_deleted`, `rule_detail_delete_ask`, `rule_when_typed`, `rule_when_added`, `rule_when_bad`, `rule_step2_field`) | `d`, `a` in the rule views | the delete question `Delete "NAME"? (y/N)` on the last row; the typed condition on the `new:` line; a condition that does not parse is refused with `Not a valid condition: … (e.g. from:*@example.org)` on the last row and not added; step 2 shows the typed field | d y a Enter q |
| Body editor (`body_editor`, `body_editor_typed`) | Enter on the compose dialog (US-87) | header bar "Message body — To: … Subject: …"; the text (tui_textedit, scrolls both ways, cursor at the text cursor); dim bottom line `Esc=save and continue  Ctrl-S=save`, `saved`/`autosaved` after a save | text, Enter, arrows, Esc, Ctrl-S |
| Review (`review`, `review_discard_ask`) | Esc in the body editor | header "Review: Message"; dim labels From, To, Subject, Body (line count), Attach; key line `[s] Send … [q] Discard`; `q` asks `Discard draft? To: …, Subject: …, N line(s), M attachment(s) [y/n]` | s e b a d q |

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
