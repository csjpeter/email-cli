# User Story: Attachment Management

## Summary
As a user reading an email with attachments, I want to list the attachments and save
one or all of them to disk.

## Trigger
The interactive show view is active for a message that contains MIME attachments.

## Behaviour
1. When the message has attachments, the status bar shows `a=save  A=save-all(N)` where
   N is the number of attachments.
2. Pressing `a` opens the attachment picker, listing all attachments with their names
   and sizes.
3. In the attachment picker:
   - ↑ / ↓ navigate the list.
   - Enter selects the highlighted attachment.
   - A prompt `Save as: <path>` appears with the default save directory pre-filled.
   - Enter accepts the path; ESC cancels.
   - On success, `Saved: <path>` is shown briefly on the status bar.
   - Backspace returns to the show view without saving.
4. Pressing `A` from the show view opens a prompt `Save all to: <dir>` for batch saving.
   - Enter saves all attachments to the given directory.
   - ESC cancels without saving any file.
   - On success, `Saved N/N` is shown on the status bar.

## Save Directory
- Default save path is `~/Downloads/` if it exists, otherwise `~/`.

## Non-Interactive Mode
In batch mode (`email-cli show <uid>` without TTY), attachment names and sizes are
listed as part of the output but cannot be saved interactively.

## Examples (key sequences)
```
email-cli           # open TUI
Enter               # open message
a                   # open attachment picker
↓ / Enter           # select second attachment
Enter               # confirm default save path → file saved
```
