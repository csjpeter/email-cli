# US-40: Label/Folder Management

## Summary

As a user, I want to list, create, and delete labels (Gmail) or folders (IMAP) from the command line and from the TUI label browser.

## Commands

- `email-cli list-labels` — list all labels (Gmail) or folders (IMAP)
- `email-cli create-label <name>` — create a new label/folder
- `email-cli delete-label <id>` — delete a label/folder

## Acceptance Criteria

### list-labels

1. For Gmail: prints a two-column table with "Label" and "ID" headers.
2. For IMAP: prints one folder name per line.
3. Available in `email-cli-ro` (read-only operation).

### create-label

1. For Gmail: calls `gmail_create_label()` via the REST API; prints the new label ID.
2. For IMAP: issues the IMAP `CREATE` command.
3. Blocked in `email-cli-ro`.

### delete-label

1. For Gmail: calls `gmail_delete_label()` with the label ID; prints confirmation.
2. For IMAP: issues the IMAP `DELETE` command.
3. System labels (INBOX, TRASH, etc.) cannot be deleted — the server returns an error.
4. Blocked in `email-cli-ro`.

## TUI Label Browser

The interactive label browser (`email_service_list_labels_interactive`) has two new keys:

- `c` — prompts for a new label name (uses `input_line_run`), calls `email_service_create_label`, refreshes the display.
- `d` — calls `email_service_delete_label` with the currently selected label name as the ID, refreshes the display.

Note: For Gmail, the displayed name is used as the deletion ID, which may differ from the actual label ID. For precise deletion use `email-cli delete-label <id>` with the ID from `list-labels`.

The status bar shows: `c=create  d=delete`.
