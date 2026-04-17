# US-32 — Gmail Compose and Send

**As a** Gmail user,
**I want to** compose and send emails via the Gmail REST API,
**so that** I don't need SMTP configuration for my Gmail account.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | Pressing **c** in the Gmail message list opens the compose editor (same InputLine-based flow as IMAP). |
| 2 | The user fills in To, Subject, and Body fields. |
| 3 | On send, the RFC 2822 message is base64url-encoded and sent via `POST /gmail/v1/users/me/messages/send`. |
| 4 | Gmail server auto-adds the SENT label; no client-side label management is needed. |
| 5 | No SMTP configuration is stored or required for Gmail accounts. |
| 6 | `email_service.c` dispatches to `gmail_send_message()` for Gmail or `smtp_send()` for IMAP based on `mail_client_uses_labels()`. |
| 7 | If the user abandons composition (ESC / ^C), the message is discarded with a confirmation prompt. |
| 8 | The **e** key (SMTP wizard) on the accounts screen is hidden or disabled for Gmail accounts. |

---

## Send dispatch

```c
if (mail_client_uses_labels(client)) {
    // Gmail: send via REST API
    gmail_send_message(client, raw_rfc2822, len);
} else {
    // IMAP: send via SMTP (existing libwrite path)
    smtp_send(smtp_cfg, raw_rfc2822, len);
}
```

---

## Error handling

| Error | Behaviour |
|-------|-----------|
| HTTP 401 | Refresh access token and retry once; if still 401, trigger reauthorization |
| HTTP 403 | Display: `"Gmail API access denied."` |
| Network error | Display: `"Send failed — check internet connection."` |

---

## Implementation notes

* `gmail_client.c` provides `gmail_send_message()` which encodes the raw
  RFC 2822 message to base64url and POSTs to the Gmail send endpoint.
* The compose UI is shared between IMAP and Gmail; only the final send
  step differs.
* Reply and forward pre-populate headers (In-Reply-To, References) and
  include `threadId` from the original message for Gmail threading.

---

## Related

* Spec: `docs/spec/gmail-api.md` section 16
* GML milestones: GML-11
