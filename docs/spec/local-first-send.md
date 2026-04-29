# Local-First Send & Sync

## Overview

Sending a message never blocks on an IMAP connection. After a successful SMTP
send the message is written to the local Sent folder and queued for upload.
If SMTP fails the message is saved as a Draft instead. The next `email-sync`
run uploads all pending messages to the IMAP server.

---

## User Stories

### US-SL-01 — Successful send saves locally

**As** an email user,
**I want** my sent message saved immediately to my local Sent folder after
successful SMTP delivery,
**so that** I can see the sent message in the folder list without waiting for
an IMAP connection.

**Acceptance Criteria:**
- Composing and sending a new message calls SMTP only.
- On SMTP success `email_service_save_sent` stores the message in the local
  Sent folder (`.eml` + `.hdr` + manifest entry).
- The message is also added to `pending_appends.tsv`.
- No IMAP/IMAPS connection is opened during or after the send.
- The UI shows: `Saved locally (will upload on next sync).`

---

### US-SL-02 — Failed send saves to Drafts

**As** an email user,
**I want** an unsent message preserved in my Drafts folder when SMTP fails,
**so that** I don't lose the message and can retry later.

**Acceptance Criteria:**
- If SMTP returns a non-zero exit code the compose UI calls
  `email_service_save_draft`.
- The draft is written to the local Drafts folder with a temporary UID.
- The draft is queued in `pending_appends.tsv` so the next sync uploads it.
- The UI shows: `Saved to Drafts (will retry on next sync).`

---

### US-SL-03 — Sync uploads pending messages

**As** an email user,
**I want** my locally-saved sent messages and drafts automatically uploaded
to the IMAP server when `email-sync` runs,
**so that** they appear on all my devices and mail clients.

**Acceptance Criteria:**
- `email_service_sync` loads `pending_appends.tsv` at the start of every run.
- For each entry it issues `IMAP APPEND` to upload the raw message.
- On successful upload the local `.eml`/`.hdr` files and the manifest entry
  are removed, and the entry is deleted from `pending_appends.tsv`.
- On failure the entry is left in `pending_appends.tsv` for retry.
- Progress is printed: `→ Sent ... uploaded.` / `... failed (retry on next sync).`

---

### US-SL-04 — Pending queue survives restarts

**As** an email user,
**I want** my unsynchronised outgoing messages to survive application restarts,
**so that** they are not lost even if sync has not yet run.

**Acceptance Criteria:**
- `pending_appends.tsv` is a persistent file in the account's local store
  directory (same level as `pending_flags.tsv`).
- Messages saved via `local_save_outgoing` appear in `pending_appends.tsv`
  immediately and remain until successfully uploaded.
- Restarting the application and running sync again picks up any remaining
  entries.

---

## Data Format

`pending_appends.tsv` — one entry per line, tab-separated:

```
<folder>\t<uid>\n
```

Example:
```
Sent	t1714398012345
Drafts	t1714399000000
```

Temporary UIDs use the format `t<epoch_ms>` (14 characters, fits the 16-char
UID field).

---

## Error Handling

| Scenario | Behaviour |
|---|---|
| SMTP succeeds, local disk full | Warning printed; no crash |
| SMTP fails, local disk full | Warning printed; no crash |
| IMAP APPEND fails during sync | Entry kept in queue; retry on next sync |
| `.eml` file missing at upload time | Entry removed silently (already gone) |
