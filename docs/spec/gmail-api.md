# Gmail Native API Specification

## Overview

This document specifies how email-cli supports Gmail accounts via the Gmail
REST API (v1).  **Gmail accounts never use IMAP or SMTP** — not for detection,
not for any operation.  The string `imap.gmail.com` must not appear anywhere
in the source code.

Gmail's internal model is a document database where **everything is a label**.
INBOX, SENT, TRASH, STARRED, UNREAD are all labels — there are no folders,
no special flag mechanism.  Our TUI exposes this model natively rather than
emulating IMAP semantics or copying the Gmail web UI's "folder illusion".

---

## 1. Account Type Detection

Account type is determined **exclusively** by an explicit wizard question at
setup time:

```
Account type:
  [1] IMAP (standard e-mail server)
  [2] Gmail (Google account — uses Gmail API, not IMAP)
```

No hostname sniffing, no `@gmail.com` suffix detection.  The config file
stores `GMAIL_MODE=1` when Gmail is selected.

### Config Fields (Gmail-specific)

```ini
GMAIL_MODE=1
GMAIL_REFRESH_TOKEN=<long-lived OAuth2 refresh token>
GMAIL_CLIENT_ID=<optional override of built-in default>
GMAIL_CLIENT_SECRET=<optional override of built-in default>
```

Gmail accounts still store `EMAIL_USER=user@gmail.com` for display purposes.
`EMAIL_HOST`, `EMAIL_PASS`, `SMTP_*` fields are absent or ignored.

---

## 2. OAuth2 Authentication

### Device Authorization Grant (RFC 8628)

Used because email-cli is a CLI/TUI application without a guaranteed browser
redirect capability.

**Flow:**

1. POST `https://oauth2.googleapis.com/device/code`
   - Parameters: `client_id`, `scope=https://www.googleapis.com/auth/gmail`
   - Response: `device_code`, `user_code`, `verification_url`, `expires_in`, `interval`

2. Display to user:
   ```
   Go to: https://accounts.google.com/device
   Enter code: XXXX-XXXX
   (Waiting for authorization... ^C to cancel)
   ```

3. Poll `https://oauth2.googleapis.com/token` every `interval` seconds:
   - Parameters: `grant_type=urn:ietf:params:oauth:grant-type:device_code`,
     `client_id`, `device_code`
   - Until: `access_token` + `refresh_token` returned, or `expires_in` exceeded

4. Store `refresh_token` in `config.ini` (persisted, mode 0600).
   `access_token` is held in memory only (expires in ~3600s).

### Token Refresh (automatic)

On every `gmail_connect()` call:
1. POST `https://oauth2.googleapis.com/token`
   - Parameters: `grant_type=refresh_token`, `client_id`, `refresh_token`
2. Response: new `access_token`
3. All subsequent API calls use `Authorization: Bearer <access_token>` header

### Built-in Credentials

OAuth2 `client_id` and `client_secret` are compiled into the binary via CMake
defines (`-DGMAIL_DEFAULT_CLIENT_ID="..."` `-DGMAIL_DEFAULT_CLIENT_SECRET="..."`).
This follows standard practice for open-source native Gmail clients (aerc,
himalaya, etc.).  Users may override via `GMAIL_CLIENT_ID` / `GMAIL_CLIENT_SECRET`
in config.ini.

---

## 3. Label Model

### Everything Is a Label

| Traditional concept | Gmail reality |
|---------------------|--------------|
| Folder              | Does not exist — a label filter view |
| `\Seen` flag        | Absence of `UNREAD` label |
| `\Flagged` flag     | `STARRED` label |
| `\Deleted` flag     | `TRASH` label (via `messages.trash()` API) |
| `\Draft` flag       | `DRAFT` label |
| Archive             | Message with **no labels at all** |

### System Labels

| Label ID   | Display name | Behaviour |
|------------|-------------|-----------|
| `INBOX`    | INBOX       | Incoming mail; archiving removes this label |
| `SENT`     | Sent        | Auto-added on send |
| `DRAFT`    | Drafts      | Auto-added on draft creation |
| `TRASH`    | Trash       | `messages.trash()` removes all other labels, adds this; auto-deleted by Google after 30 days |
| `SPAM`     | Spam        | Marked via `messages.modify()` (no compound API) |
| `STARRED`  | Starred     | Star toggle, also a browsable view |
| `UNREAD`   | Unread      | Inverse of "read"; also a browsable view |

### User Labels

Created by the user, fully mutable (rename, delete).  A message can have
any number of user labels simultaneously.  Nested labels use `/` separator
(e.g. `Projects/Work`).

---

## 4. TUI — Label List View

The label list view replaces the folder browser for Gmail accounts.

### Layout (three sections)

```
  Labels — user@gmail.com  (12)

  INBOX                          3
  Starred
  Unread                         8
  Sent
  Drafts                         1
  ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
  Work                           5
  Personal
  Project-X
  ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄
  Archive                        (no labels)
  Spam                           2
  Trash                          (auto-delete: 30 days)
```

**Section 1 — System views:** INBOX, Starred, Unread, Sent, Drafts.
Daily-use entry points.

**Section 2 — User labels:** Work, Personal, etc.
Created by user; displayed alphabetically.

**Section 3 — Special views:**
- **Archive** — messages with no labels at all (non-overlapping with any label view)
- **Spam** — messages with SPAM label
- **Trash** — messages with TRASH label, with warning `(auto-delete: 30 days)`

### Archive View Semantics

A message is in the Archive view **if and only if it has no labels** — no INBOX,
no user labels, no STARRED, nothing.  This is intentionally non-overlapping:
a message is either in one or more label views, or in Archive, never both.

### Label List Navigation

| Key       | Action |
|-----------|--------|
| ↑ / ↓    | Move cursor |
| Enter     | Open: show messages with this label |
| Backspace | Return to accounts screen |
| ESC / q   | Quit |

---

## 5. TUI — Message List (Gmail)

### Layout

```
  Labels — user@gmail.com > INBOX  (3 unread)

  Date              Labels         Subject              From
  ════════════════  ═════════════  ═══════════════════  ════════════
▶ 2026-04-16       ★ Work         Quarterly review     Boss
  2026-04-15                      Hello there          Alice
  2026-04-14       Personal       Invoice              Bank
```

- **No UID column** — the 16-char hex ID is meaningless to users; shown only
  in the message reader view (optional).
- **Labels column** — shows all labels on the message (not just the active filter).
  System labels abbreviated: `★` for STARRED.  User labels by name, truncated
  to column width.
- **Unread messages** rendered in bold (or reverse video) per existing convention.

### Key Bindings (Gmail-specific)

| Key | Operation         | API call                           | Effect |
|-----|-------------------|------------------------------------|--------|
| `r` | Remove label      | `messages.modify(removeLabelIds)`  | Removes the currently-filtered label from the message; message disappears from this view but survives elsewhere |
| `d` | Trash             | `messages.trash()`                 | All labels removed, TRASH added; auto-deleted by Google after 30 days |
| `a` | Archive           | `messages.modify(remove: INBOX)`   | INBOX label removed (relevant only in INBOX view) |
| `f` | Star toggle       | `messages.modify(STARRED)`         | Add or remove STARRED label |
| `n` | Unread toggle     | `messages.modify(UNREAD)`          | Add or remove UNREAD label |
| `t` | Label picker      | `messages.modify()`                | Popup checkbox list of all user labels; toggle on/off |
| `c` | Compose           | `messages.send()`                  | Compose new message; sent via Gmail API (not SMTP) |
| `s` | Sync              | History API                        | Start background incremental sync |

### Important: `r` (Remove label) vs `d` (Trash)

This distinction is the core of Gmail-native behaviour and the key difference
from IMAP.  In IMAP, "delete from folder" means delete the message.  In Gmail:

- **`r` (remove label)** = the label comes off, the message stays alive with
  its other labels.  If the message has no other labels, it moves to Archive.
- **`d` (trash)** = `messages.trash()` — a compound operation that strips
  **all** labels and adds TRASH.  Message is permanently deleted by Google
  after 30 days.

### Trash Restore Caveat

`messages.untrash()` removes the TRASH label but does **not** restore the
original labels.  To support "undo trash", the client must locally remember
the labels that were present before trashing.

### Auto-sorting (server-side Gmail filters)

Gmail filters can:
1. Remove INBOX label + add a custom label → message appears only in that label view
2. Keep INBOX + add a custom label → message appears in both INBOX and the label view

The Labels column in the message list makes this visible: a message in INBOX
that also has `Work` label shows `Work` in the Labels column.

---

## 6. Unified 16-Character UID

All message identifiers are stored as **fixed-width 16-character strings**
throughout the system.

| Backend | UID example        | Format |
|---------|--------------------|--------|
| IMAP    | `0000000000012345` | Decimal integer, zero-padded to 16 chars |
| Gmail   | `18c9b46d67a6123f` | Native Gmail hex message ID (always 16 hex chars) |

### Properties

- **Fixed width**: every UID is exactly 16 characters, enabling fixed-size
  records in index files and O(log n) binary search by byte offset.
- **String-sortable**: zero-padded decimal and hex strings both sort correctly
  via `strcmp()` / `memcmp()`.
- **Direct file naming**: `store/<d1>/<d2>/<uid>.eml` where d1 and d2 are
  derived from the UID string (last 2 chars for bucketing).
- **No mapping file needed**: Gmail message IDs are used directly as UIDs
  without any synthetic integer mapping.

### Refactor Scope

The existing `int uid` parameter type in `imap_client.h`, `email_service.c`,
and `local_store.c` changes to `char uid[17]` (16 chars + NUL) in the
`mail_client.h` abstraction layer.  IMAP integer UIDs are converted to
zero-padded strings at the `mail_client` boundary.

---

## 7. Local Store — Gmail Layout

### Directory Structure

```
~/.local/share/email-cli/accounts/<email>/
  store/<d1>/<d2>/<uid>.eml          # Full RFC 2822 message (flat, no folder level)
  headers/<d1>/<d2>/<uid>.hdr        # Display metadata: from, subject, date, labels
  labels/
    INBOX.idx                        # Sorted list of UIDs with INBOX label
    SENT.idx
    STARRED.idx
    UNREAD.idx
    DRAFTS.idx
    <UserLabel>.idx                  # One file per user label
    _nolabel.idx                     # Archive: UIDs with no labels at all
    _spam.idx
    _trash.idx
  gmail_history_id                   # Last synced historyId (single integer)
```

### Label Index Files (.idx)

Each `.idx` file contains one UID per line, sorted in ascending order.

**Record format:** 16-character UID + newline = **17 bytes per record** (fixed).

**Binary search:** For a file with N records:
- Total size = N × 17 bytes
- Record at position i starts at byte offset i × 17
- Binary search: `seek((lo + hi) / 2 * 17)`, read 16 chars, `memcmp()`, halve

**Lookup complexity:** O(log N) with a single file descriptor.

**A message appears in multiple .idx files** if it has multiple labels.
This is by design and reflects Gmail's label model.

### Header Cache Files (.hdr)

Per-message files containing display metadata (from, subject, date, label list).
Variable-length content (subjects and sender names vary).

For rendering one page of the message list (~30 messages), the client reads
~30 individual `.hdr` files.  On modern SSDs this is negligible (<1ms total).

### Archive View (`_nolabel.idx`)

Maintained during sync: when a label is removed from a message and no other
labels remain, the UID is added to `_nolabel.idx`.  When any label is added,
the UID is removed from `_nolabel.idx`.

**Query for archive** (server-side, used during full sync):
```
q=has:nouserlabels -label:inbox -label:sent -label:drafts -label:spam -label:trash -label:starred
```

---

## 8. Synchronisation

### Full Sync (first run)

1. `GET /gmail/v1/users/me/messages?maxResults=500` (paginated via `nextPageToken`)
   → collect all message IDs
2. For each message ID not yet in local store:
   - `GET /gmail/v1/users/me/messages/{id}?format=raw` → base64url decode → save as `.eml`
   - `GET /gmail/v1/users/me/messages/{id}?format=metadata&metadataHeaders=From&metadataHeaders=Subject&metadataHeaders=Date`
     → extract headers + `labelIds` → save as `.hdr`
   - Messages already cached (both `.eml` and `.hdr` present) are skipped.
3. **Rebuild all label index files** from locally cached `.hdr` files (see below).
   This runs unconditionally at the end of every full sync — even when 0 messages
   were downloaded — ensuring indexes are always consistent with the store.
4. Build `_nolabel.idx` from messages that have no labels
5. Save `historyId` from the most recent message to `gmail_history_id`

#### Label Index Rebuild (step 3)

Rebuilding is an O(N log N) in-memory operation:

1. Read all `.hdr` files in the local store.
2. Parse the labels field (4th tab-separated column, comma-separated label IDs).
3. Build a flat `(label_id, uid)` pair list.
4. Sort by `label_id` then `uid`.
5. Write one `.idx` file per unique label (17-byte records: 16-char UID + `\n`).

This replaces the previous O(N²) approach of appending UIDs one by one during the
fetch loop.  Because it reads from `.hdr` files rather than the API, it works
identically whether messages were freshly downloaded or already cached.

The same logic is exposed as `email-sync --rebuild-index` for offline repair.
See `docs/spec/email-sync.md` → `--rebuild-index`.

### Incremental Sync (subsequent runs)

Uses the Gmail History API for O(changes) efficiency instead of O(total messages).

1. `GET /gmail/v1/users/me/history?startHistoryId=<saved>&historyTypes=messageAdded,messageDeleted,labelAdded,labelRemoved`

2. Process delta records:

   | Delta type       | Local action |
   |-----------------|-------------|
   | `messagesAdded`  | Download `.eml` + `.hdr`; insert UID into relevant `.idx` files |
   | `messagesDeleted`| Delete `.eml` + `.hdr`; remove UID from all `.idx` files |
   | `labelsAdded`    | Insert UID into the label's `.idx`; remove from `_nolabel.idx` if present |
   | `labelsRemoved`  | Remove UID from the label's `.idx`; if no labels remain → add to `_nolabel.idx` |

3. Re-sort any modified `.idx` files
4. Save new `historyId`

### "No labels remain" Check

When processing a `labelsRemoved` event, determine if the message still has
any labels by checking if the UID exists in any `.idx` file (excluding
`_nolabel.idx`, `_spam.idx`, `_trash.idx`).  If not found in any, add to
`_nolabel.idx`.

---

## 9. Gmail API Endpoints Used

### Compound Operations (server manages label consistency)

| Operation   | Endpoint                    | Behaviour |
|-------------|-----------------------------|-----------|
| Trash       | `POST messages/{id}/trash`  | Removes all labels, adds TRASH |
| Untrash     | `POST messages/{id}/untrash`| Removes TRASH only (does NOT restore previous labels) |
| Send        | `POST messages/send`        | Creates message with SENT label |
| Send draft  | `POST drafts/{id}/send`     | Removes DRAFT, adds SENT |
| Delete      | `DELETE messages/{id}`       | Permanent deletion — **not exposed in TUI** |

### Label Modification (client manages consistency)

| Operation        | Endpoint                     | Request body |
|-----------------|------------------------------|-------------|
| Remove label     | `POST messages/{id}/modify`  | `{removeLabelIds: ["<label>"]}` |
| Add label        | `POST messages/{id}/modify`  | `{addLabelIds: ["<label>"]}` |
| Mark as spam     | `POST messages/{id}/modify`  | `{addLabelIds: ["SPAM"], removeLabelIds: ["INBOX"]}` |
| Toggle STARRED   | `POST messages/{id}/modify`  | `{addLabelIds: ["STARRED"]}` or `{removeLabelIds: ["STARRED"]}` |
| Toggle UNREAD    | `POST messages/{id}/modify`  | `{addLabelIds: ["UNREAD"]}` or `{removeLabelIds: ["UNREAD"]}` |
| Batch modify     | `POST messages/batchModify`  | `{ids: [...], addLabelIds: [...], removeLabelIds: [...]}` (max 1000) |

### Read Operations

| Operation        | Endpoint                                  | Notes |
|-----------------|-------------------------------------------|-------|
| List messages    | `GET messages?labelIds=X&maxResults=500`  | Paginated via `nextPageToken` |
| Get raw message  | `GET messages/{id}?format=raw`            | Returns base64url RFC 2822 |
| Get metadata     | `GET messages/{id}?format=metadata`       | Returns headers + labelIds |
| List labels      | `GET labels`                              | All system + user labels |
| Create label     | `POST labels`                             | `{name: "...", ...}` |
| Delete label     | `DELETE labels/{id}`                       | User labels only |
| Sync history     | `GET history?startHistoryId=X`            | Incremental delta |

### Message Retrieval

Messages are fetched with `format=raw`, which returns the complete RFC 2822
message as a base64url-encoded string.  After decoding, this is fully
compatible with the existing `mime_util` and `html_render` code — no changes
needed to the message display pipeline.

---

## 10. Abstraction Layer (mail_client.h)

A thin dispatch layer sits between `email_service.c` and the backend-specific
clients (`imap_client.h`, `gmail_client.h`).

### Semantic Search Criteria

The abstraction uses an enum instead of IMAP-style search strings:

```c
typedef enum {
    MAIL_SEARCH_ALL,
    MAIL_SEARCH_UNREAD,
    MAIL_SEARCH_FLAGGED,   /* IMAP: \Flagged, Gmail: STARRED */
    MAIL_SEARCH_DONE,
} MailSearchCriteria;
```

### Dispatch Logic

```c
MailClient *mail_client_connect(const Config *cfg);
// → if cfg->gmail_mode: gmail_connect(cfg)
// → else:               imap_connect(cfg->host, cfg->user, cfg->pass, ...)
```

`mail_client_uses_labels(c)` returns 1 for Gmail, 0 for IMAP.
`email_service.c` uses this to select "Labels" vs "Folders" UI text and
to enable/disable Gmail-specific key bindings (`r`, `a`, `t`).

---

## 11. Setup Wizard — Gmail Flow

### Step 1: Account Type

```
Account type:
  [1] IMAP (standard e-mail server)
  [2] Gmail (Google account — uses Gmail API, not IMAP)
Choice [1]:
```

### Step 2 (Gmail): Email Address

```
Email address: user@gmail.com
```

### Step 3 (Gmail): OAuth2 Device Flow

```
Opening Gmail authorization...
  Go to: https://accounts.google.com/device
  Enter code: XXXX-XXXX
  (Waiting for authorization... ^C to cancel)

✓ Authorization successful. Configuration saved.
```

On success:
- `GMAIL_MODE=1` saved to config.ini
- `EMAIL_USER=user@gmail.com` saved
- `GMAIL_REFRESH_TOKEN=<token>` saved
- No `EMAIL_HOST`, `EMAIL_PASS`, or `SMTP_*` fields

### Reauthorization

If the refresh token expires or is revoked, `gmail_connect()` detects the
`invalid_grant` error and prompts for reauthorization using the same device
flow.

---

## 12. TUI — Account List (Gmail Accounts)

Gmail accounts appear alongside IMAP accounts in the unified account list.
The **Type** column distinguishes them:

```
  Accounts  (2)

  Email                    Type    Status
  ═══════════════════════  ══════  ═══════════
  user@example.com         IMAP    3 unread
  user@gmail.com           Gmail   8 unread
```

- **Type column**: displays `IMAP` or `Gmail` — replaces any server hostname
  info (Gmail accounts have no IMAP host/port to display).
- **No SMTP columns**: Gmail send uses the REST API, not SMTP.
- **Status**: aggregate unread count, identical to IMAP accounts.
- **Navigation**: Enter on a Gmail account opens the **Label List** (§4);
  Enter on an IMAP account opens the folder browser (existing behaviour).

---

## 13. TUI — Label Picker (`t` key)

When `t` is pressed on a message in the Gmail message list, an overlay popup
appears:

```
  ┌─ Labels ─────────────┐
  │ [x] INBOX            │
  │ [ ] Starred          │
  │ [x] Work             │
  │ [ ] Personal         │
  │ [ ] Project-X        │
  │                      │
  │ Enter=toggle  q=done │
  └──────────────────────┘
```

### Behaviour

- **Checkbox list** overlaid on the message list, centred horizontally.
- Shows toggleable labels: system labels (INBOX, STARRED, UNREAD) and all
  user labels.  **Excluded** from the picker: TRASH, SPAM, DRAFTS, SENT —
  these are managed exclusively by compound API calls.
- `[x]` = message currently has this label; `[ ]` = does not.
- **Enter** = toggle the selected label immediately via `messages.modify()`.
  The checkbox updates in-place; the API call fires asynchronously.
- **↑ / ↓** = navigate the picker list.
- **q / ESC** = close the picker and return to the message list.
- If the toggle results in the message having **no labels at all**, it
  migrates to `_nolabel.idx` (Archive).

### Width and Scrolling

- Popup width: max(label name lengths) + checkbox + padding, capped at 40
  columns.
- If the label count exceeds the available popup height, the list scrolls
  vertically.

---

## 14. TUI — Status Bar (Gmail)

The status bar (bottom line of the terminal) adapts its content for Gmail
accounts:

| View | Status bar text |
|------|----------------|
| Label list | `↑↓=navigate  Enter=open  Backspace=accounts  q=quit` |
| Message list (INBOX) | `r=remove  d=trash  a=archive  f=star  t=labels  c=compose  q=back` |
| Message list (other label) | `r=remove  d=trash  f=star  t=labels  c=compose  q=back` |
| Message reader | `r=remove  d=trash  f=star  n=unread  t=labels  q=back` |
| Label picker | `Enter=toggle  ↑↓=navigate  q=done` |

**Notes:**
- The `a` (archive) key hint appears **only** in the INBOX view — archiving
  is defined as removing the INBOX label.
- The `n` (unread toggle) key hint appears in the **reader** view — in the
  list it's less useful because opening a message auto-clears UNREAD.
- IMAP accounts continue to show the existing IMAP-specific status bar text.

---

## 15. TUI — Message Reader (Gmail)

The message reader for Gmail adds a **Labels** line to the header block:

```
  From:    Boss <boss@example.com>
  Date:    2026-04-16 09:30
  To:      user@gmail.com
  Subject: Quarterly review
  Labels:  INBOX, ★ Starred, Work
  ────────────────────────────────────

  Hi, please find the quarterly review attached...
```

### Details

- **Labels line**: shown only for Gmail accounts.  IMAP messages do not
  display this line.
- **System label display**: `★` prefix for STARRED; other system labels by
  name (INBOX, SENT, DRAFTS).
- **UNREAD not shown**: the user is currently reading the message, so
  displaying UNREAD would be misleading (it gets removed on open).
- **TRASH / SPAM**: shown if the message is in Trash or Spam view (the user
  navigated there intentionally).
- **Key bindings in reader**: `r` (remove label), `d` (trash), `f` (star
  toggle), `n` (unread toggle), `t` (label picker) — all function
  identically to the message list.

---

## 16. Compose / Reply / Forward (Gmail)

### Compose (`c` key)

1. Opens the existing compose editor (same InputLine-based flow as IMAP).
2. User fills in **To**, **Subject**, **Body**.
3. On send: the composed RFC 2822 message is base64url-encoded and sent via
   `POST /gmail/v1/users/me/messages/send` with `{raw: "<base64url>"}`.
4. Gmail server auto-adds `SENT` label — no client-side label management
   needed.

### Reply (`R` key) / Forward (`F` key)

Same compose flow with pre-populated headers (In-Reply-To, References, quoted
body).  The `messages.send()` call includes `threadId` from the original
message to maintain Gmail's threading model.

### Draft Save

If the user abandons composition (ESC / ^C):
- **Option A** (simple, Phase 1): message is discarded with confirmation
  prompt.
- **Option B** (future): `POST /gmail/v1/users/me/drafts` with the partial
  message → saved to Drafts label.

### Dispatch in `email_service.c`

```c
if (mail_client_uses_labels(client)) {
    // Gmail: send via REST API
    gmail_send_message(client, raw_rfc2822, len);
} else {
    // IMAP: send via SMTP (existing libwrite path)
    smtp_send(smtp_cfg, raw_rfc2822, len);
}
```

No SMTP configuration is stored or needed for Gmail accounts.

---

## 17. Error Handling

### OAuth2 Errors

| Error | Handling |
|-------|----------|
| `invalid_grant` (expired/revoked refresh token) | Auto-trigger device flow reauthorization; display the device code prompt |
| `invalid_client` | Display: `"OAuth2 client credentials are invalid. Check GMAIL_CLIENT_ID/SECRET in config.ini."` |
| Network error during device flow poll | Retry silently up to `expires_in` timeout; then display: `"Authorization timed out."` |
| User cancels (^C during device flow) | Return to account setup; no partial config saved |

### API Request Errors

| HTTP Status | Handling |
|-------------|---------|
| 401 Unauthorized | Refresh access token and retry once; if still 401 → trigger reauthorization |
| 403 Forbidden | Display: `"Gmail API access denied. Verify OAuth2 scope and account permissions."` |
| 404 Not Found (message) | Skip message silently during sync (permanently deleted server-side) |
| 429 Too Many Requests | Exponential backoff: 1s → 2s → 4s → 8s → 16s → 30s cap; retry up to 6 times |
| 5xx Server Error | Retry 3 times with exponential backoff; then display: `"Gmail API temporarily unavailable."` |

### Sync Error Recovery

If incremental sync fails with `historyId` not found (HTTP 404 on the
`/history` endpoint):

1. Log warning: `"History expired — performing full resync"`
2. Delete all local `.idx` files for this account
3. Perform full sync from scratch (§8)
4. Save new `historyId`

This is expected to happen only when the client hasn't synced for an extended
period (>30 days) or when Google's internal history is pruned.

### Network Connectivity

All Gmail API calls go through a single `gmail_request()` helper that handles:
- Connection timeouts: 10 seconds (libcurl `CURLOPT_CONNECTTIMEOUT`)
- Transfer timeouts: 60 seconds (libcurl `CURLOPT_TIMEOUT`)
- On timeout: display `"Network error — check internet connection"` in the
  status bar; operation is not retried automatically (user can press `s` to
  retry sync).
