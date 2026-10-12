# email-tui: element inventory

Every view of `email-tui` with its elements, keys and the test that compares
it cell for cell (EMAIL-34).  The tests are in `tests/pty/test_pty_screens.c`;
the expected pictures and style maps are in `tests/pty/screens_expected.h`.  A
terminal of 100x24, `TZ=UTC`, a mock IMAP server with three messages and a
fresh home per view are fixed, so every picture is reproducible.

Style legend of the maps: `H` bold white on blue (header bar), `r` reverse
(status bar, selection), `B` bold+reverse, `b` bold, `d` dim, `g` green,
`G` reverse green, `R` red, space default.

## Views

| View (test) | Reached by | Elements | Keys named on the screen |
|---|---|---|---|
| Accounts (`accounts`) | start | header bar with count; column heads Unread, Flagged, Account, Type, Server, Send via; ruler; one row per account, `→` on the cursor row, bold; status bar | ↑↓ Enter n d i e Esc |
| Accounts help (`accounts_help`) | `h` on accounts | boxed overlay, title, key/description rows, footer "Press any key to close" | any key |
| Account deleted (`account_delete`) | `d` on accounts | "No accounts configured." | n |
| Wizard type (`wizard_type`) | `n` on accounts | plain prompt: account type menu, `Choice [1]:` | 1, 2 |
| Folder browser (`folders`, `folders_flat`) | Enter on an account | header bar with count; heads Unread, Flagged, Folder, Total; "Tags / Flags" section (dim, 6 virtual rows) and "Folders" tree; cursor row reverse; status bar with `[n/N]` | ↑↓ PgUp/PgDn Enter t / h Esc q BS |
| Folder help (`folders_help`) | `h` | overlay | any key |
| Folder search (`folders_search`) | `/` | filter prompt line | text, Enter, Esc |
| Message list (`list`, `list_second_row`, `list_narrow`) | Enter on INBOX | header bar "1-3 of 3 message(s) in INBOX (3 unread) [account]."; heads Date, Sts, Subject, From; ruler; rows with status column (`N` green = unread); status bar | ↑↓ PgUp/PgDn Enter c r F A n f j d D s U l / h Esc q BS |
| Empty folder (`list_empty`) | Enter on an empty folder | header "0 of 0 message(s) …", "(empty)" | |
| List help (`list_help`) | `h` | overlay with key rows and the Sts legend | any key |
| List search (`list_search`) | `/` | filter line "Filter [Subject|From|To|Body]: _  Showing n of N" | text, Tab, Enter, Esc |
| Message reader (`reader`, `reader_source`, `reader_search`) | Enter on a message | header block From, Subject, Date, UID, File, DMARC; rule; body; status line "-- [1/1] …" with the keys | ↑↓ PgUp/PgDn Home End r F f n d D a A v / h Esc q BS |
| Reader help (`reader_help`) | `h` | overlay | any key |
| Rules editor (`rules_empty`, `rules_list`, `rules_second`) | `l` on the list | title "Rules for account (n rules)"; "(no rules …)" or one row per rule, reverse on the cursor; hint line | j k ↑↓ Enter a d Esc q |
| Rule detail (`rule_detail`) | Enter on a rule | "Rule:", "Account:", `when:` and the `then-*` rows; hint line | e d Esc q |
| Rule edit (`rule_edit_form`) | `e` on a rule | condition list editor step 1/2 prefilled | j k Enter e a d q Esc |
| Rule add, conditions (`rule_add_when`) | `a` in the rules editor | step 1/2 title, hint, "(empty — press 'a' …)" | a q Esc |
| Rule add, actions (`rule_add_actions`) | `q` in step 1 | step 2/2 title; Name; add-label 1–3; rm-label 1–3; then-move-folder; then-forward-to | Enter Esc |
| Compose dialog (`compose_dialog`, `compose_typed`, `compose_empty_to`) | `c` | "New Message"; To Cc Bcc Subject Attach fields; guard line "To: is empty …" (red); hint | Tab Shift-Tab ; Enter Esc |

## Not covered by a whole-screen comparison yet

The Gmail label browser and label views (`test-pty-gmail-tui`), the attachment
picker, the post-compose review screen, the pre-compose dialog's address
drop-down, the sync and refresh status messages and every error dialog keep
their element-level assertions in the other PTY suites.

## Findings recorded while writing the pictures

Behaviour as it is today is what the pictures encode; each item below is a
defect or inconsistency the pictures make visible, left for the owner to ticket.

1. Accounts: the Server column cuts the address after the colon
   (`imaps://localhost:`), the port is never visible.
2. Folder browser, tree: an empty row follows the entries with wide
   characters (`中`, `😀`); the rows below are shifted.
3. Folder browser, flat view (`t`): only `INBOX/` is listed, not its subfolders.
4. Header bar: the rules editor, rule detail, rule add/edit steps and the
   compose dialog do not use the bold white-on-blue header bar of
   `policies/tui-interface-model.md`; the compose dialog's top row is reverse
   video, the others plain.
5. Accounts: `d` deletes the account at once, with no confirmation.
6. Message list: the From column is cut in the middle of the address
   (`<sender3@example.c`) at 100 columns.
7. Help overlays (fixed with EMAIL-34): rows were padded by bytes, so rows
   with `↑ / ↓` were misaligned and a multi-byte cut was possible; the
   key rows still said `ESC / q  Quit` and `Backspace` after EMAIL-37.
