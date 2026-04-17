# US-33 — Non-Interactive Batch Send

**As a** script or automation user,
**I want to** send an email from the command line without an interactive editor,
**so that** I can integrate email sending into scripts, cron jobs, and CI pipelines.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | `email-cli send --to <addr> --subject <text> --body <text>` sends a message and exits. |
| 2 | All three flags (`--to`, `--subject`, `--body`) are required; missing any produces an error on stderr and exits with code 1. |
| 3 | The sender address is `SMTP_USER` if configured, otherwise `EMAIL_USER`. |
| 4 | The message is a valid RFC 2822 message with Date, Message-ID, MIME-Version, Content-Type, and Content-Transfer-Encoding headers. |
| 5 | Body line endings are converted from LF to CRLF per RFC 2822. |
| 6 | SMTP configuration must exist before sending; if missing, the error message directs the user to `email-cli config smtp`. |
| 7 | For Gmail accounts (`GMAIL_MODE=1`), the message is sent via the Gmail REST API instead of SMTP. No SMTP configuration is needed. |
| 8 | On success, the sent message is saved to the local Sent folder via `email_service_save_sent()`. |
| 9 | If saving to Sent fails, a warning is printed but the exit code remains 0 (the send itself succeeded). |
| 10 | `email-cli send --help` prints usage text and exits with code 0. |

---

## Usage

```bash
email-cli send --to friend@example.com --subject "Hello" --body "Hi there!"
```

### Output

```
Message sent.
Saved.
```

If Sent folder save fails:

```
Message sent.
(Could not save to Sent folder — check EMAIL_SENT_FOLDER in config.)
```

---

## Error cases

| Condition | stderr output | Exit code |
|-----------|--------------|-----------|
| Missing `--to` | `Error: --to, --subject, and --body are all required.` | 1 |
| Missing `--subject` | (same) | 1 |
| Missing `--body` | (same) | 1 |
| No SMTP config (IMAP account) | `smtp_send: refused to send...` | 1 |
| SMTP connection failure | `smtp_send: <curl error>` | 1 |
| Gmail API send failure | `gmail_send: <error>` | 1 |

---

## Implementation notes

* `main.c` parses `--to`, `--subject`, `--body` from argv.
* `compose_build_message()` (libwrite) builds the RFC 2822 message.
* Send dispatch: `smtp_send()` for IMAP accounts, `gmail_send_message()` for Gmail.
* No interactive editor, no $EDITOR, no TUI — purely flag-driven.

---

## Related

* Spec: `docs/spec/commands.md` section `send`
* US-20: Compose/Reply (interactive, email-tui only)
* US-32: Gmail Compose and Send (Gmail-specific send path)
