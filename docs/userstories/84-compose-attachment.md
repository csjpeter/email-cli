# US-84 — Compose: File Attachment Support

**As a** user composing an email,
**I want** to attach one or more local files to a message from within the
pre-compose dialog using Tab-completion and a file-browser dropdown,
**so that** I can send attachments without leaving the TUI and without having
to remember exact file paths.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | The pre-compose dialog has an `Attach:` field below Subject. |
| 2 | Typing a path prefix in the Attach field and pressing Tab cycles through matching files and directories (like path_complete). |
| 3 | A dropdown below the Attach field lists the files and subdirectories under the current partial path as the user types (similar to the contact-autocomplete dropdown). |
| 4 | Directories are shown with a trailing `/`; files without. |
| 5 | Pressing Enter in the Attach field when a complete file path is typed adds the file to the attachment list and clears the field for another entry. |
| 6 | Attempting to attach a directory (path ending with `/`) shows an inline error: `Not a file: <path>`. |
| 7 | Attempting to attach a non-existent path shows an inline error: `File not found: <path>`. |
| 8 | After adding a file the dialog shows a summary line below the Attach field: `  [1] report.pdf  [2] notes.txt`. |
| 9 | Pressing `d` while the Attach field is focused and a file is listed removes the last attachment. |
| 10 | Tab navigation: To → Cc → Bcc → Subject → Attach → To (wraps). |
| 11 | If at least one attachment is present, the pre-filled editor draft includes an `Attach:` header line listing the absolute paths, one per line. |
| 12 | `compose_build_message()` detects the `Attach:` header(s) in the draft file and constructs a `multipart/mixed` MIME message with the text body as the first part and each file as a separate `application/octet-stream` (or inferred MIME type) part with `Content-Disposition: attachment; filename="..."`. |
| 13 | The file content is encoded as `base64` in the MIME part. |
| 14 | The maximum total attachment size is 25 MB; exceeding it shows an error and aborts send. |
| 15 | Attachment filenames containing non-ASCII characters are encoded per RFC 2047 / RFC 5987 in the `Content-Disposition` header. |

---

## Dialog layout sketch

```
┌─ New Message ──────────────────────────────────────────────────────────┐
│                                                                         │
│  To:       alice@example.com                                            │
│  Cc:                                                                    │
│  Bcc:                                                                   │
│  Subject:  Meeting tomorrow                                             │
│  Attach:   ~/Documents/rep_                                             │
│            ┌───────────────────────────────────────────┐               │
│            │ ▶ report-2026-Q1.pdf                      │               │
│            │   report-2025-Q4.pdf                      │               │
│            │   reports/                                │               │
│            └───────────────────────────────────────────┘               │
│  [1] contract.pdf                                                       │
│                                                                         │
│  Tab=next  Shift-Tab=prev  Enter=attach file  d=remove last  Esc=cancel│
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Technical notes

### ComposeParams extension

```c
typedef struct {
    const char *from;
    const char *to;
    const char *cc;
    const char *bcc;
    const char *subject;
    const char *body;
    const char *reply_to_msg_id;
    const char **attachments;   /* NULL-terminated array of absolute paths */
    int          attach_count;
} ComposeParams;
```

### MIME structure (multipart/mixed)

```
MIME-Version: 1.0
Content-Type: multipart/mixed; boundary="=_boundary_<pid>_<time>"

--=_boundary_<pid>_<time>
Content-Type: text/plain; charset=UTF-8
Content-Transfer-Encoding: quoted-printable

<body text>

--=_boundary_<pid>_<time>
Content-Type: application/octet-stream
Content-Transfer-Encoding: base64
Content-Disposition: attachment; filename="report.pdf"

<base64 encoded data>
--=_boundary_<pid>_<time>--
```

When there are no attachments the message structure is unchanged
(`text/plain` only, no `multipart/mixed` wrapper).

### Attach: header in editor draft

```
From: me@example.com
To: alice@example.com
Subject: Meeting tomorrow
Attach: /home/peter/Documents/report.pdf
Attach: /home/peter/notes.txt

Body text here.
```

Multiple `Attach:` lines are supported. The header is stripped from the
visible body in the editor (parsed separately by `cmd_compose_interactive`).
Lines after the blank separator are the body regardless of `Attach:` headers.

### Files changed / created

| File | Change |
|------|--------|
| `libwrite/src/compose_service.h` | Add `attachments`, `attach_count` to `ComposeParams` |
| `libwrite/src/compose_service.c` | `compose_build_message()` generates `multipart/mixed` when attachments present |
| `src/main_tui.c` | Attach field in `compose_dialog()`; parse `Attach:` from draft |
| `libemail/src/core/path_complete.c` | Reuse or extend for file browser dropdown in compose |

---

## Related user stories

- US-CD-1 … US-CD-12: Pre-compose dialog (base)
- US-85: Post-compose review screen
