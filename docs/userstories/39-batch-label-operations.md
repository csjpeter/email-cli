# US-39: Batch Label-on-Message Operations

## Summary

As a Gmail user, I want to add or remove a label on a specific message from the command line, so that I can organize messages programmatically without the TUI.

## Commands

- `email-cli add-label <uid> <label>` — add a Gmail label to a message
- `email-cli remove-label <uid> <label>` — remove a Gmail label from a message

## Acceptance Criteria

1. The command contacts the Gmail API and modifies the message's label list.
2. The local label index is updated: `label_idx_add` or `label_idx_remove`.
3. IMAP accounts return an error: "label operations require Gmail mode."
4. The commands are blocked in `email-cli-ro` with a clear error message.
5. `<label>` is the Gmail label ID (e.g. `Label_12345`, `STARRED`, `INBOX`).

## Implementation Notes

- Implemented via `email_service_set_label()` which calls `mail_client_modify_label()`.
- Uses `gmail_modify_labels()` with either add or remove array.
