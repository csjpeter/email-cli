# Functional coverage map

Which functional test exercises which capability (EMAIL-124; policy
`testing-model.md`, *Every capability has a functional test*).  Line coverage
says which lines ran; this says which things the programs can do and which
test drives each from outside, against the mock IMAP, SMTP and Gmail servers.

Surveyed **2026-10-17** from the programs' own `--help` (`bin/<program> --help`).
`tests/functional/run_coverage_map.sh` re-reads those and fails when a command,
sub-command or option has no row here, when a row names a test that does not
exist, when a row has neither a test nor a reason, and when a row matches
nothing in a `--help` any more.  A change that adds, changes or removes a
capability changes its row in the same commit.

Test references: `F:<id>` is a check of `tests/functional/run_functional.sh`
(the id is the first word of its description), `P:<name>` a PTY test function in
`tests/pty/`, `S:<file>` a script in `tests/functional/`.  Where a check only
asserts that the command does not crash, the *Notes* column says so: that is
recorded weakness, not coverage of the promise.

## email-cli

| Capability | Tests | Notes |
|------------|-------|-------|
| `email-cli add-account` | F:76.1 F:116.41 | |
| `email-cli add-label` | F:124.22 | Gmail wire request asserted; IMAP has no labels |
| `email-cli config` | F:19.1 F:73.3 | show, imap and smtp sub-commands; `reauth` F:89.21 |
| `email-cli create-folder` | F:124.14 | IMAP `CREATE` asserted; Gmail refusal F:124.27 |
| `email-cli create-label` | F:122.24 | IMAP refusal F:124.26 |
| `email-cli delete-folder` | F:124.16 | |
| `email-cli delete-label` | F:122.25 | |
| `email-cli help` | F:1.2 F:85.7 | |
| `email-cli --help` | F:1.1 | |
| `email-cli --version` | F:74.1 | |
| `email-cli list` | F:1.4 F:1.6 | |
| `email-cli list --all-accounts` | F:84.26 | exercised through `email-cli-ro`, which shares the code |
| `email-cli list --folder` | F:58.1 F:59.1 | virtual folders and search; weak: asserts no crash |
| `email-cli list --from` | F:84.2 | through `email-cli-ro` |
| `email-cli list --json` | F:84.17 F:116.5 | through `email-cli-ro`; the other `--json` commands F:122.1 |
| `email-cli list --since/--before` | F:84.5 F:116.1 | |
| `email-cli list-accounts` | F:18.1 | |
| `email-cli list-attachments` | F:14.1 F:14.2 | |
| `email-cli list-folders` | F:25.13 F:1.14 | |
| `email-cli list-labels` | F:26.11 | |
| `email-cli mark-junk` | F:124.10 | |
| `email-cli mark-notjunk` | F:124.12 | |
| `email-cli mark-read` | F:124.2 | |
| `email-cli mark-starred` | F:124.6 | |
| `email-cli mark-unread` | F:124.4 | |
| `email-cli migrate-credentials` | F:122.26 | |
| `email-cli remove-account` | F:122.22 | |
| `email-cli remove-label` | F:124.24 | |
| `email-cli remove-starred` | F:124.8 | |
| `email-cli rules add` | F:37.1 | |
| `email-cli rules apply` | F:36.1 | `--dry-run` F:36.3 |
| `email-cli rules edit` | F:37.20 | `--new-name` F:37.29 |
| `email-cli rules list` | F:34.1 | `--json` F:122.16 |
| `email-cli rules remove` | F:37.12 | |
| `email-cli save-attachment` | F:15.2 | |
| `email-cli reply` | F:133.3 F:133.8 | `--all` F:133.7 |
| `email-cli trash` | F:132.2 F:132.6 | |
| `email-cli archive` | F:132.5 F:132.13 | |
| `email-cli send` | F:11.3 F:114.3 | SMTP mock receives the message; failure F:114.1 |
| `email-cli show` | F:1.10 F:1.11 | |
| `email-cli show --raw` | F:122.21 | |

## email-cli-ro

| Capability | Tests | Notes |
|------------|-------|-------|
| `email-cli-ro help` | F:77.1 | |
| `email-cli-ro --help` | F:77.1 | |
| `email-cli-ro --version` | F:74.20 | |
| `email-cli-ro list` | F:84.2 F:77.9 | |
| `email-cli-ro list --all-accounts` | F:84.26 | |
| `email-cli-ro list --folder` | F:77.4 P:test_ro_list_folder | |
| `email-cli-ro list --from` | F:84.2 | |
| `email-cli-ro list --json` | F:84.17 | |
| `email-cli-ro list --since/--before` | F:84.5 | |
| `email-cli-ro list-accounts` | F:66.7 | |
| `email-cli-ro list-attachments` | F:57.2 P:test_ro_attachments | |
| `email-cli-ro list-folders` | F:57.1 | |
| `email-cli-ro list-labels` | F:66.5 | |
| `email-cli-ro save-attachment` | P:test_ro_save_attachment | |
| `email-cli-ro show` | F:66.1 F:66.2 | |
| `email-cli-ro show --raw` | F:122.21 | |

## email-sync

| Capability | Tests | Notes |
|------------|-------|-------|
| `email-sync --account` | P:test_sync_account_filter_known F:74.37 | |
| `email-sync --apply-rules` | F:47.3 F:36.1 | |
| `email-sync cron remove` | F:54.2 F:116.55 | |
| `email-sync cron setup` | F:116.51 F:116.49 | |
| `email-sync cron status` | F:116.53 | |
| `email-sync --help` | P:test_sync_help | |
| `email-sync --rebuild-contacts` | F:55.2 | |
| `email-sync --rebuild-index` | F:27.3 F:27.6 | |
| `email-sync --reconcile` | F:30.9 | |
| `email-sync --verbose` | F:35.3 F:35.4 | |

## email-import-rules

| Capability | Tests | Notes |
|------------|-------|-------|
| `email-import-rules --account` | F:116.60 F:116.65 | |
| `email-import-rules --dry-run` | F:69.2 | weak: asserts no crash |
| `email-import-rules --help` | F:38.10 F:69.3 | |
| `email-import-rules --output` | F:116.60 F:116.62 | |
| `email-import-rules --thunderbird-path` | F:116.58 F:116.64 | |
| `email-import-rules --version` | F:116.69 | |

## Other capabilities (not named in a `--help` list)

| Capability | Tests | Notes |
|------------|-------|-------|
| `email-tui` interactive screens | P:test_screen_list P:test_screen_reader P:test_screen_folders P:test_screen_accounts | the per-operation list with tests is `docs/interface-parity.md`, checked by `run_parity.sh`; element-level checks are `docs/spec/tui-elements.md` |
| `email-tui` compose, reply, forward | P:test_review_s_sends_message P:test_reply_all_send P:test_forward_fill_to_opens_editor | |
| `email-tui send` | F:114.3 | |
| `email-tui reply` | F:114.7 | refusal without a UID; the reply itself P:test_review_reply_uses_review_screen |
| `email-tui` rules editor | P:test_tui_rules_add_form_saves_new_rule P:test_screen_rules_deleted P:test_tui_rules_preview_and_apply P:test_screen_rules_preview | |
| `email-tui` Gmail labels and flags | P:test_feedback_t_label_added P:test_feedback_f_star | |
| TUI mark junk (`j`) | | gap: no PTY test; also listed in `docs/interface-parity.md`; ticket to be opened by the owner |
| TUI delete label (`d` in the label browser) | | gap: no PTY test, and it deletes by name not id; ticket to be opened by the owner |
| libemail-client IMAP, Gmail, SMTP over verified TLS | S:run_static_binaries.sh | the static binaries run against the mock servers with certificate checking in an empty root; protocol detail in the unit tests |
| libemail-client Gmail authorization (loopback flow, revoked grant) | F:113.24 F:89.25 | |
| `--json` on the read commands | F:122.1 F:122.5 F:122.9 F:122.10 F:122.13 F:122.16 | both binaries |
| `TERM` handling of the programs | S:run_term.sh | |
| Packaging and install | S:run_package_dir.sh S:run_package_policy.sh | |
