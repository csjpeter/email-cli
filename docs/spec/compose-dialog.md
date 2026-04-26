# Compose Dialog — Specification

Pre-compose dialog for new messages, replies, reply-all, and forwards.
Replaces the raw vim header editing with a small interactive TUI form that
collects recipients and subject **before** opening the editor, so the user
lands in vim with a ready-to-type body.

---

## Concepts

### Pre-compose dialog

A full-screen (or centered floating) TUI form with four editable fields:

```
┌─ New Message ─────────────────────────────────────────────────────────┐
│                                                                        │
│  To:      alice@example.com; bob@                                      │
│           ╔══════════════════════════════════════════╗                 │
│           ║ ▶ bob.smith@acme.com (Bob Smith)         ║                 │
│           ║   bob.jones@example.org                  ║                 │
│           ╚══════════════════════════════════════════╝                 │
│  Cc:                                                                   │
│  Bcc:                                                                  │
│  Subject: ___________________________________________                  │
│                                                                        │
│  Tab=next field  Shift-Tab=prev  Enter=open editor  Esc=cancel        │
└────────────────────────────────────────────────────────────────────────┘
```

**Field navigation**: Tab / Shift-Tab cycle through To → Cc → Bcc → Subject.
Pressing Enter on the Subject field (or from any field when no autocomplete
dropdown is open) confirms and opens the editor.

**Recipient fields**: Each field accepts one or more RFC 5322 addresses
separated by `;` (semicolon).  After pressing `;` or Tab the current token
is "committed" and a new address can be typed.  Backspace on an empty token
removes the last committed address.

**Autocomplete dropdown**: While the user types a partial address in any
recipient field a drop-down of up to 8 matching candidates appears below the
active field.  The dropdown is dismissed when the field becomes empty, when
Esc is pressed, or when a selection is made.

```
  To:      john_
           ┌───────────────────────────────────────────┐
           │ ▶ john.doe@example.com                    │
           │   john.smith@company.org                  │
           │   johnathan.green@corp.org                │
           └───────────────────────────────────────────┘
```

### Contact suggestions source

Suggestions are built from:
1. **From addresses** in all manifest TSV files (fast, always available).
2. **To / Cc addresses** in `.hdr` cache files (scanned lazily, results
   cached in `contacts.tsv` per account, refreshed each sync).

Each contact entry is: `address\tDisplay Name\tfrequency` (tab-separated).
Entries are sorted by descending frequency (most-used first), then
alphabetically.  Matching is case-insensitive substring on both the address
and the display name.

File location: `~/.local/share/email-cli/accounts/<user>/contacts.tsv`

### Key bindings (all dialog types)

| Key | Effect |
|-----|--------|
| Tab | Confirm current address token; move to next field |
| Shift-Tab | Move to previous field |
| ↑ / ↓ | Navigate autocomplete dropdown (if open) |
| Enter (dropdown open) | Select highlighted suggestion |
| Enter (dropdown closed) | Confirm all fields; open editor |
| Esc (dropdown open) | Dismiss dropdown; stay in field |
| Esc (dialog) | Cancel — return to message list |
| `;` | Commit current token and start new address in same field |
| Backspace | Delete last char; if field is empty, remove last address |

### Editor integration

After the dialog is confirmed the editor is opened with a temp file that
already has all headers pre-filled:

```
From: me@example.com
To: alice@example.com; bob.smith@acme.com
Cc: carol@example.org
Bcc: hidden@example.com
Subject: Meeting tomorrow
In-Reply-To: <msgid>   ← only for replies

<cursor here>
```

The user writes the body and saves.  Bcc is stripped before sending.

### Empty-To guard

If the user presses Enter / confirms with an empty To field, the dialog
shows an inline error message and keeps focus on the To field.

### No-subject warning

If the Subject field is empty when the user confirms, the dialog shows a
one-line confirmation prompt:

```
  Subject is empty — send anyway? (y/n)
```

`y` or Enter proceeds; any other key returns focus to the Subject field.

---

## User Stories

### US-CD-1 — Pre-compose dialog for new messages

**As a user** composing a new message I want an interactive dialog that
lets me set To, Cc, Bcc, and Subject **before** vim opens, so I land in
the editor ready to type the body.

**Acceptance criteria:**
- Pressing `c` in the message list opens the pre-compose dialog (not vim).
- The dialog shows four labelled fields: To, Cc, Bcc, Subject.
- A status line at the bottom shows the active key bindings.
- Pressing Esc returns to the message list without opening vim.
- After filling To and Subject and pressing Enter, vim opens with those
  values pre-filled in the header section of the temp file.
- The From header in the temp file matches `cfg->user` (or `cfg->smtp_user`).

---

### US-CD-2 — Contact autocomplete

**As a user** entering a recipient address I want to see matching suggestions
from my email history as I type, so I can select the right address quickly.

**Acceptance criteria:**
- Typing ≥ 2 characters in any recipient field opens the autocomplete dropdown.
- Dropdown shows up to 8 matching candidates.
- Typing more characters narrows the dropdown in real time.
- Up / Down arrows move the highlighted entry.
- Enter selects the highlighted entry, appends it to the field, clears the
  input token, and closes the dropdown.
- Tab selects the top (highlighted) entry, commits it, and moves to the
  next field.
- Esc dismisses the dropdown without selecting; the typed text remains.
- Matching is case-insensitive on both address and display name.
- If no matches exist the dropdown is not shown (no empty dropdown).
- Suggestions come from both `From` addresses in manifests and `To`/`Cc`
  addresses extracted from `.hdr` cache files.

---

### US-CD-3 — Multiple recipients per field

**As a user** I want to add multiple addresses to the To, Cc, and Bcc fields
so I can send to several people at once.

**Acceptance criteria:**
- Pressing `;` after an address commits it and starts a new input token in
  the same field.
- The field displays committed addresses as `name1@a.com; name2@b.com; _`.
- Pressing Backspace on an empty token removes the last committed address.
- All committed addresses appear in the corresponding header in the vim file.
- Multiple To recipients are semicolon-separated in the `To:` header line.

---

### US-CD-4 — Tab / Shift-Tab navigation between fields

**As a user** I want Tab and Shift-Tab to move between To → Cc → Bcc →
Subject so I can fill all fields without using a mouse.

**Acceptance criteria:**
- Tab from To moves to Cc.
- Tab from Cc moves to Bcc.
- Tab from Bcc moves to Subject.
- Tab from Subject wraps to To.
- Shift-Tab moves in reverse.
- When autocomplete is open Tab first selects the top suggestion.

---

### US-CD-5 — Empty-To validation

**As a user** I want to be prevented from sending with an empty To field so I
don't accidentally send blank messages.

**Acceptance criteria:**
- Pressing Enter with an empty To field shows an inline error:
  `To: field cannot be empty`.
- Focus stays on the To field.
- The error clears as soon as the user starts typing.
- No vim is opened.

---

### US-CD-6 — No-subject warning

**As a user** I want a warning when I try to send without a subject, with an
option to proceed, so I can avoid accidentally subject-less messages.

**Acceptance criteria:**
- When Subject is empty and Enter is pressed the dialog shows:
  `Subject is empty — send anyway? (y/n)`.
- Pressing `y` or Enter proceeds to open the editor.
- Any other key (including `n` or Esc) returns focus to the Subject field.

---

### US-CD-7 — Reply pre-compose dialog

**As a user** replying to a message I want the pre-compose dialog to open
with the To field pre-filled with the original sender and the Subject
pre-filled with `Re: <original subject>`, and I want to be able to add Cc
recipients before the editor opens.

**Acceptance criteria:**
- Pressing `r` in the message list opens the pre-compose dialog (not vim).
- Dialog title reads "Reply".
- To field contains the sender's address (from Reply-To header if present,
  otherwise From header).
- Subject field contains `Re: <original subject>` (double "Re: Re:" collapsed
  to single "Re:").
- Cc and Bcc fields are empty and editable.
- Pressing Enter opens vim with the pre-filled headers and a quoted body block.
- Pressing Esc cancels and returns to the message list.
- Autocomplete works in the Cc field.

---

### US-CD-8 — Reply-All (`A` key)

**As a user** I want a reply-all shortcut that pre-fills both To (original
sender) and Cc (all other recipients from the original To and Cc), so I can
easily respond to group threads.

**Acceptance criteria:**
- Pressing `A` (capital A) in the message list opens the pre-compose dialog.
- Dialog title reads "Reply All".
- To field pre-filled with the original sender's address.
- Cc field pre-filled with all addresses from the original To and Cc headers,
  minus the user's own address (`cfg->user` / `cfg->smtp_user`).
- Subject pre-filled with `Re: <original subject>`.
- All fields remain editable before opening the editor.
- Pressing Esc cancels.
- `A` is listed in the message list help popup.

---

### US-CD-9 — Forward (`F` key)

**As a user** I want to forward a message by pressing `F` and then specifying
the destination recipients in the pre-compose dialog, before the editor opens
with the original body quoted.

**Acceptance criteria:**
- Pressing `F` (capital F) in the message list opens the pre-compose dialog.
- Dialog title reads "Forward".
- To, Cc, Bcc fields are empty (user must fill at least To).
- Subject field pre-filled with `Fwd: <original subject>`.
- Pressing Enter with empty To shows the empty-To error.
- After filling To and pressing Enter, vim opens with the `Fwd:` subject and
  the original message body appended below a `--- Forwarded message ---`
  separator line.
- Autocomplete works in all recipient fields.
- Pressing Esc cancels.
- `F` is listed in the message list help popup.

---

### US-CD-10 — Compose from folder browser

**As a user** I want to be able to start a new message from the folder browser
screen, not only from the message list, so I don't have to open a folder first.

**Acceptance criteria:**
- Pressing `c` in the folder/label browser opens the pre-compose dialog.
- Behaviour is identical to composing from the message list.

---

### US-CD-11 — Cc / Bcc written to editor temp file

**As a user** I want the Cc and Bcc addresses I entered in the dialog to be
present in the editor temp file so I can review and edit them before sending.

**Acceptance criteria:**
- `Cc:` header written to temp file if the Cc field is non-empty.
- `Bcc:` header written to temp file if the Bcc field is non-empty.
- `compose_build_message()` generates a `Cc:` header from the Cc field.
- Bcc is **not** included in the final RFC 2822 message (stripped before
  send); the SMTP envelope still includes Bcc recipients.
- The `ComposeParams` struct is extended with `cc` and `bcc` string fields.

---

### US-CD-12 — Contacts file rebuilt on sync

**As a user** I want the contact suggestions to stay up to date so that newly
received senders appear in autocomplete after a sync.

**Acceptance criteria:**
- `email-sync` (or `email-tui` background sync) rebuilds `contacts.tsv` after
  each successful sync of an account.
- If `contacts.tsv` does not exist the first autocomplete query scans all
  `.hdr` files and writes the file.
- The file is written atomically (write to `.tmp`, then rename).

---

## Implementation Notes

### New files

| File | Purpose |
|------|---------|
| `libemail/src/core/compose_dialog.c/.h` | Pre-compose TUI dialog |
| `libemail/src/infrastructure/contact_store.c/.h` | Contact suggestions cache |

### Changed files

| File | Change |
|------|--------|
| `libwrite/src/compose_service.h` | Add `cc`, `bcc` to `ComposeParams` |
| `libwrite/src/compose_service.c` | Generate `Cc:` header; strip `Bcc:` from wire |
| `src/main_tui.c` | Route 'c'/'r'/'F'/'A' through compose dialog; add `cmd_forward()`, `cmd_reply_all()` |
| `libemail/src/domain/email_service.c` | Return codes 4=reply-all, 5=forward from list TUI; add 'F'/'A' key handlers; update help panel |
| `libemail/src/domain/email_service.h` | Document new return codes 4 and 5 |

### Return codes from `email_service_list()`

Extend the existing return value set:

| Value | Meaning |
|-------|---------|
| 0 | User quit normally |
| 1 | Backspace (go to folder list) |
| 2 | `c` — compose new message |
| 3 | `r` — reply; `opts->action_uid` holds the UID |
| 4 | `A` — reply-all; `opts->action_uid` holds the UID |
| 5 | `F` — forward; `opts->action_uid` holds the UID |
| -1 | Error |

---

## Test Plan (PTY)

All tests live in `tests/pty/test_pty_compose_dialog.c`.
They use the existing mock IMAP + mock SMTP servers.

### Compose dialog — basic flow

| ID | Test | Expected |
|----|------|---------|
| TC-CD-01 | Press `c` in list view | Pre-compose dialog opens (shows "New Message") |
| TC-CD-02 | Esc in compose dialog | Returns to message list; no vim invoked |
| TC-CD-03 | Enter with empty To | Inline error "cannot be empty"; dialog stays open |
| TC-CD-04 | Fill To + Subject, Enter | vim opens; draft file contains `To:` and `Subject:` |
| TC-CD-05 | Fill To only, Enter (no subject) | Warning "Subject is empty"; dialog stays open |
| TC-CD-06 | Warning prompt: `y` | vim opens despite empty subject |
| TC-CD-07 | Warning prompt: `n` | Returns to Subject field; dialog stays open |
| TC-CD-08 | Dialog title shows "New Message" | Title text visible on screen |

### Field navigation

| ID | Test | Expected |
|----|------|---------|
| TC-CD-09 | Tab from To field | Cursor moves to Cc field |
| TC-CD-10 | Tab from Cc → Bcc → Subject → To (wrap) | Correct cycle |
| TC-CD-11 | Shift-Tab from Subject → Bcc → Cc → To | Reverse cycle |
| TC-CD-12 | Tab from Subject confirms dialog | Editor opens |

### Recipient input

| ID | Test | Expected |
|----|------|---------|
| TC-CD-13 | Type complete address in To | Address accepted |
| TC-CD-14 | Type address, `;`, type second address | Both appear in field; both in temp file |
| TC-CD-15 | Backspace on empty token | Removes last committed address |
| TC-CD-16 | Cc address written to temp file header | `Cc:` header present |
| TC-CD-17 | Bcc address written to temp file header | `Bcc:` header present |

### Autocomplete

| ID | Test | Expected |
|----|------|---------|
| TC-CD-18 | Typing ≥2 matching chars opens dropdown | Dropdown visible with ≤8 entries |
| TC-CD-19 | Typing more chars narrows dropdown | Fewer entries shown |
| TC-CD-20 | No matches → no dropdown | Dropdown absent |
| TC-CD-21 | 1 char typed → no dropdown yet | Dropdown absent (threshold=2) |
| TC-CD-22 | Down arrow highlights next entry | Second entry highlighted |
| TC-CD-23 | Enter selects highlighted entry | Address appended; dropdown closed |
| TC-CD-24 | Tab selects top entry and advances field | Top entry added; cursor in Cc |
| TC-CD-25 | Esc dismisses dropdown | Dropdown gone; typed text stays |
| TC-CD-26 | Case-insensitive match: "JOHN" matches "john@" | Match shown |
| TC-CD-27 | Display name match: "Smith" matches "bob.smith@" | Match shown |

### Reply dialog

| ID | Test | Expected |
|----|------|---------|
| TC-CD-28 | Press `r` on a message | Reply dialog opens with title "Reply" |
| TC-CD-29 | To field pre-filled | Contains original sender's address |
| TC-CD-30 | Subject pre-filled | Shows "Re: <original subject>" |
| TC-CD-31 | Reply-To header respected | To uses Reply-To address if present |
| TC-CD-32 | Re: not doubled | "Re: Re: X" collapsed to "Re: X" |
| TC-CD-33 | Can add Cc before opening editor | Cc address appears in temp file |
| TC-CD-34 | Esc cancels reply | Returns to message list |
| TC-CD-35 | Enter opens editor | vim opens with quoted body |
| TC-CD-36 | Quoted body present | `> ` prefixed lines visible after editing |

### Reply-All dialog

| ID | Test | Expected |
|----|------|---------|
| TC-CD-37 | Press `A` on a message with multiple recipients | Reply-All dialog opens |
| TC-CD-38 | Dialog title "Reply All" | Visible on screen |
| TC-CD-39 | To has original sender | Pre-filled correctly |
| TC-CD-40 | Cc has other original recipients | Pre-filled with remaining addresses |
| TC-CD-41 | Own address excluded from Cc | cfg->user not in Cc field |
| TC-CD-42 | Esc cancels | Returns to list |
| TC-CD-43 | `A` listed in help panel | Visible when `h` pressed |

### Forward dialog

| ID | Test | Expected |
|----|------|---------|
| TC-CD-44 | Press `F` on a message | Forward dialog opens |
| TC-CD-45 | Dialog title "Forward" | Visible on screen |
| TC-CD-46 | To field empty | No pre-fill; cursor in To |
| TC-CD-47 | Subject pre-filled "Fwd: X" | Correct prefix |
| TC-CD-48 | Enter with empty To | Inline error shown |
| TC-CD-49 | Fill To, Enter | vim opens with `Fwd:` subject |
| TC-CD-50 | Forwarded body present | Original text under separator line |
| TC-CD-51 | Autocomplete works in Forward | Dropdown appears on partial address |
| TC-CD-52 | Esc cancels | Returns to list |
| TC-CD-53 | `F` listed in help panel | Visible when `h` pressed |

### Compose from folder browser

| ID | Test | Expected |
|----|------|---------|
| TC-CD-54 | Press `c` in folder browser | Pre-compose dialog opens |
| TC-CD-55 | Esc from dialog returns to folder browser | Folder list visible |
| TC-CD-56 | Fill To + Subject, open editor | Editor opens with headers |

### Contacts / autocomplete source

| ID | Test | Expected |
|----|------|---------|
| TC-CD-57 | Sender from received mail appears in suggestions | Shown in dropdown |
| TC-CD-58 | Recipient from sent mail appears in suggestions | Shown in dropdown |
| TC-CD-59 | After sync, new sender appears in suggestions | New contact in contacts.tsv |
| TC-CD-60 | contacts.tsv created on first autocomplete query | File exists after first query |

### ComposeParams Cc/Bcc

| ID | Test | Expected |
|----|------|---------|
| TC-CD-61 | Cc in dialog → `Cc:` header in sent message | Mock SMTP receives Cc header |
| TC-CD-62 | Bcc in dialog → Bcc address in SMTP envelope | SMTP DATA has no Bcc header |
| TC-CD-63 | Multiple Cc addresses → semicolon-separated Cc header | All addresses in Cc: line |
