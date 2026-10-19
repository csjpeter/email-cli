# Interface parity: email-cli, email-cli-ro, email-tui

Survey of **2026-10-17**, read from the code at commit `8bc9837487d1` (the
operations of the reference interface come from `email-cli --help` and
`src/main.c`; the TUI side from the key handlers in
`libemail/src/domain/email_service.c` and `src/main_tui.c`, not from the earlier
tables).  Policy: `interface-parity-model.md` (workspace policies).  Kept with
the code: a change that adds or removes an operation changes this table.

## Interfaces

| Interface | Program | Notes |
|-----------|---------|-------|
| CLI | `email-cli` (read/write), `email-cli-ro` (read only, same read commands) | the reference; scripts and agents |
| TUI | `email-tui` | full-screen; also `email-tui send/reply/compose` one-shot forms |
| Web UI | none, not planned | |
| Sync | `email-sync` | background program (cron), not an interface of the owner's operations; it runs the same sync, rules and cron as the others |

The CLI-only forms of a command (`--json`, `--batch`, flags for scripts) are
medium, not gaps: a screen is not a script.

## Operations

`yes` needs a test reference: `F:<id>` is a check of `tests/functional/run_functional.sh`
(its first word), `P:<name>` a PTY test (a function in `tests/pty/`).  The
parity test `tests/functional/run_parity.sh` checks that every `yes` has
existing references and every `no` has a reason.

| Operation | CLI | CLI test | TUI | TUI test | Reason for a `no` / ticket |
|-----------|-----|----------|-----|----------|----------------------------|
| List messages (unread/all, paging) | yes | F:1.4 | yes | P:test_screen_list | |
| Filter the list by sender/subject/to/body text | yes (`--from`, `__search__:`) | F:84.2 | yes (`/`) | P:test_tui_list_search_filter | |
| Filter the list by date (`--since`, `--before`) | yes | F:84.5 | yes (`/`, Tab to Since / Before) | P:test_tui_list_date_filter | |
| Search cached mail | yes (`--folder __search__:scope:text`) | F:59.1 | yes (`/`) | P:test_screen_list_search | |
| Virtual folders (unread, flagged, junk, answered, forwarded) | yes | F:58.1 | yes | P:test_virtual_junk_list_shows_messages | |
| Read a message | yes (`show`) | F:1.10 | yes | P:test_screen_reader | |
| Raw message source | yes (`show --raw`) | F:122.21 | yes (`v`) | P:test_interactive_show_source_toggle | |
| List IMAP folders (counts, tree) | yes | F:25.13 | yes | P:test_screen_folders | |
| List Gmail labels | yes | F:26.11 | yes | P:test_screen_gmail_labels | |
| Create / delete an IMAP folder | yes | F:51.2 | no | | not built in the TUI; ticket to be opened by the owner |
| Create a Gmail label | yes | F:122.24 | yes (`c` in the label browser) | P:test_label_browser_create_label_confirmed | |
| Delete a Gmail label | yes | F:122.25 | no | | the label browser has a `d` key, but it passes the label name as the id (`TODO` in `email_service_labels_interactive`; Gmail needs the id) and no test deletes a label there: a gap until fixed and tested; ticket to be opened by the owner |
| Mark read / unread | yes | F:16.1 | yes (`n`) | P:test_feedback_n_read | |
| Star / unstar | yes | F:17.1 | yes (`f`) | P:test_feedback_f_star | |
| Add / remove a Gmail label on a message | yes | F:60.1 | yes (`t` picker) | P:test_feedback_t_label_added | |
| Mark junk / not junk | yes | F:52.2 | yes | P:test_keys_junk_toggle_imap | the `!` key (registry); the PTY tests check the server requests (IMAP STORE; Gmail: test_keys_junk_toggle_gmail) |
| Move a message to a folder | yes (`move`) | F:124.29 | yes (`m`) | P:test_screen_list_move_prompt | |
| Delete (trash) / archive a message | yes (`trash`, `archive`) | F:132.2 | yes (`d`, `D`, `a`) | P:test_tui_list_bulk_delete_confirm | |
| Mark several messages at once | no (one UID per call; a script loops) | | yes (`Space`, `m`) | P:test_tui_list_bulk_star_marked | medium: a script loops; not a gap |
| List attachments | yes | F:14.1 | yes | P:test_show_attachment_picker | |
| Save an attachment / all attachments | yes | F:15.2 | yes (`a`, `A`) | P:test_show_save_single | |
| Compose and send a new message | yes (`send`) | F:11.3 | yes (`c`) | P:test_review_s_sends_message | |
| Reply, reply to all | yes (`reply`, `reply --all`) | F:133.3 | yes (`r`, `R`) | P:test_reply_all_send | the answered mark is local until the next sync |
| Forward | yes (`forward`) | F:134.1 | yes (`F`) | P:test_forward_fill_to_opens_editor | |
| List accounts | yes | F:18.1 | yes | P:test_screen_accounts | |
| Add an account (wizard) | yes | F:76.1 | yes (`n`) | P:test_wizard_complete | |
| Remove an account | yes | F:122.22 | yes (`d`) | P:test_screen_account_delete | |
| Edit account settings (IMAP, SMTP) | yes (`config imap`, `config smtp`) | F:73.3 | yes (`e`, the wizard) | P:test_wizard_with_smtp | |
| Renew a Gmail authorization | yes (`config reauth`) | F:89.21 | yes (`:` menu on the accounts screen) | P:test_reauth_paste_renews_the_token | browser, address only or paste, as the CLI; `--import-token` stays CLI-only |
| Re-encrypt stored credentials | yes (`migrate-credentials`) | F:122.26 | no | | maintenance, run by hand; `n/a` in a screen, not a gap |
| List sorting rules | yes | F:34.1 | yes (`l`) | P:test_screen_rules_list | |
| Add a sorting rule | yes | F:37.1 | yes | P:test_tui_rules_add_form_saves_new_rule | |
| Remove a sorting rule | yes | F:37.12 | yes | P:test_screen_rules_deleted | |
| Edit a sorting rule | yes (`rules edit`) | F:37.20 | yes (`e`) | P:test_screen_rule_edit_form | |
| Apply rules now (also `--dry-run`) | yes | F:36.1 | yes (`A` asks first, `p` previews) | P:test_tui_rules_preview_and_apply | |
| Sync now | yes (`email-sync`) | F:20.1 | yes (`s`) | P:test_screen_list_syncing | |
| Set up / remove / show the sync cron entry | yes (`email-sync cron`) | F:54.2 | no | | scheduling is the background program's; `n/a` in a screen, not a gap |

## Fields

The data of a message (From, To, Cc, Bcc, Subject, Date, UID, labels, flags,
DMARC, attachments, body) is shown by both: the CLI `show` and `list --json`
carry every field; the TUI reader shows From, To, Subject, Date, UID, File,
DMARC, attachments and body, and the list the status flags.  Cc is shown in the
TUI compose and review dialogs and in the reader headers when present.

## No interface holds a business rule the others do not

Both programs call `email_service_*` and `libemail-client` (`emc_*`); rule
matching, flag changes and sending are in those layers.  The one place a screen
carries its own behaviour is the label deletion by name noted above.

## Gaps for the owner

The `no` rows above are the gaps.  Opening a ticket for each takes the owner's
permission; this file is the recorded list until then.
