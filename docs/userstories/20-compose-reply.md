# User Story: Compose and Reply with External Editor

## Summary
As a user, I want to compose new messages and reply to existing ones using my
preferred terminal editor (`$EDITOR`), so that I have full editing capabilities
including navigation, backspace, multi-line editing, search, and undo.

## Invocation

### From the TUI
- `c` key in the message list → compose a new message
- `r` key in the message list → reply to the selected message
- `r` key in the message reader → reply to the currently open message

### From the CLI
```
email-tui compose
email-tui reply <uid>
```

## Preconditions
- SMTP credentials are configured (`SMTP_HOST`, `SMTP_USER`, `SMTP_PASS` in
  `~/.config/email-cli/config.ini`).
- If SMTP is not yet configured, `email-tui` runs `setup_wizard_smtp`
  interactively before opening the editor.

## Flow

### Compose (new message)
1. `email-tui` shows the compose form (To, Cc, Bcc, Subject, attachments).
2. The body is written in the built-in editor ([US-87](87-body-editor-autosave.md)):
   no external editor, no temporary file; the draft is autosaved.
3. Esc leaves the editor and shows the review screen ([US-85](85-post-compose-review.md)).
4. An empty `To:` is refused by the form.
5. Otherwise the message is built (MIME) and sent via SMTP.
6. On success: `Message sent.` is printed; on failure: SMTP error to stderr.
7. After a successful send, a copy of the message is appended to the IMAP Sent
   folder (defaults to `Sent`; override with `EMAIL_SENT_FOLDER` in config).
   `Saving to Sent folder...` is printed to stdout before the IMAP APPEND.
   If the IMAP APPEND fails a warning is written to stderr but the overall
   exit code remains `0` (the send itself succeeded).

### Reply
1. The original message is loaded from local cache or fetched from the server.
2. Reply metadata is extracted:
   - `To:` ← original `From:` (or `Reply-To:` if present)
   - `Subject:` ← original subject prefixed with `Re: ` (if not already present)
   - `In-Reply-To:` ← original `Message-ID`
3. The original body is extracted (HTML rendered to plain text, or plain text
   directly) and formatted as a quoted block:
   ```
   On <date>, <from> wrote:
   > quoted line 1
   > quoted line 2
   ```
4. A temp draft is created with the reply headers pre-filled and the quoted
   block in the body (top-posting convention: user types above the quote).
5. The built-in editor opens with the quote; steps 3–6 of Compose apply.

## Draft file format

```
From: sender@example.com
To: recipient@example.com
Subject: Re: Something
In-Reply-To: <msg-id@server.example.com>

Your reply here...

On 2026-04-14, Alice <alice@example.com> wrote:
> Original line 1
> Original line 2
```

All header fields are editable.  Additional standard headers (e.g. `Cc:`,
`Bcc:`) can be added manually; they are passed through to the MIME builder.

## Navigation keys that trigger compose/reply

| Context | Key | Action |
|---------|-----|--------|
| Message list | `c` | Compose new message |
| Message list | `r` | Reply to selected message |
| Message reader | `r` | Reply to currently displayed message |

## Status bar hints

Message list:
```
  ↑↓=step  PgDn/PgUp=page  Enter=open  Esc/q/BS=folders
  c=compose  r=reply  n=new  f=flag  d=done  s=sync  R=refresh  [N/M]
```

Message reader:
```
-- [page/total] PgDn/↓=scroll  PgUp/↑=back  r=reply  Backspace/ESC/q=list --
```

## Exit codes
- `0` — message sent successfully
- `1` — aborted, SMTP error, or missing UID

## Notes
- `$EDITOR` is no longer used by `email-tui` (US-87).
- The autosaved draft is removed when the message is sent or discarded.
- SMTP credentials are never written to the draft.
