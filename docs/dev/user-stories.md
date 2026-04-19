# User Stories

This document captures user stories that drove feature decisions in email-cli.
Each story links to the relevant spec section and, where applicable, the
implementation commit.

---

## Gmail label index rebuild

### Background

Gmail accounts store message metadata locally in `.hdr` files and maintain
per-label index files (`.idx`) that map label IDs to the UIDs of all messages
carrying that label.  These indexes drive the "Total" count column in
`list-folders` and allow O(log N) lookup when opening a label view.

---

### US-1 — Counts show 0 after re-sync

> **As a user** whose Gmail account has 25 000 cached messages,
> **when** I run `email-sync` and see "0 fetched, 25 000 already cached",
> **I want** the label counts in `list-folders` to reflect my actual messages,
> **so that** I know how many messages are in INBOX, Work, Personal, etc.

**Problem:** Before this feature, label index files were only updated for
newly downloaded messages.  On a warm cache (all messages already stored),
the sync loop skipped every message and the `.idx` files were never written,
leaving all counts at 0.

**Solution:** `gmail_sync_full` now rebuilds all label index files from
locally cached `.hdr` files at the end of every full sync, regardless of how
many messages were downloaded.

**Spec:** `docs/spec/gmail-api.md` § 8 — Full Sync, step 3.

---

### US-2 — Repair indexes without re-downloading

> **As a user** whose label index files are missing or corrupted
> (e.g. after restoring a backup, upgrading from an older version, or
> accidental deletion),
> **I want** to run `email-sync --rebuild-index` and have all `.idx` files
> rebuilt from the locally cached `.hdr` files,
> **so that** I can fix label counts without waiting for a full re-download
> of tens of thousands of messages.

**Solution:** `email-sync --rebuild-index [--account <email>]` is a new
subcommand that calls `gmail_sync_rebuild_indexes()` for each Gmail account.
It is entirely offline — no Gmail API contact required.

**Spec:** `docs/spec/email-sync.md` → `--rebuild-index`.

---

### US-3 — Repair a single account

> **As a user** with multiple Gmail accounts configured,
> **when** only one account's label indexes are wrong,
> **I want** to run `email-sync --rebuild-index --account user@gmail.com`
> to fix only that account,
> **so that** the rebuild is fast and does not touch unaffected accounts.

**Solution:** The `--account` flag accepted by `email-sync` applies to
`--rebuild-index` as well as to normal sync.

**Spec:** `docs/spec/email-sync.md` → `--rebuild-index` → Options table.

---

### US-4 — Automatic repair on first sync

> **As a new user** running `email-sync` for the first time with a large Gmail
> account,
> **I want** label counts to be accurate after the first sync completes,
> **without** having to run any extra repair commands.

**Solution:** The automatic rebuild (US-1) happens at the end of every full
sync, so the first run always produces correct indexes.

---

---

## Gmail hex UID acceptance

### Background

Gmail message IDs are 64-bit integers rendered as 16-character lowercase
hexadecimal strings (e.g. `19d9c3c967c0cc2a`).  The `list` command displays
this format in the UID column.  All commands that accept a UID as a CLI
argument must accept the same format — otherwise the user cannot use the
output of `list` as input to `mark-read`, `mark-unread`, `mark-starred`, etc.

IMAP UIDs are positive decimal integers (e.g. `42`).  Both formats must be
accepted simultaneously; neither may break the other.

---

### US-6 — mark-read rejects the UID shown by list

> **As a Gmail user** who runs `email-cli list` and sees a hex UID such as
> `19d9c3c967c0cc2a` in the output,
> **when** I copy that UID and run `email-cli mark-read 19d9c3c967c0cc2a`,
> **I want** the command to succeed (or fail for a network reason),
> **so that** I can act on messages without manually converting UIDs.

**Problem:** `parse_uid()` in `main.c` only accepted positive decimal
integers.  Hex UIDs from the `list` output were rejected with
`"Error: UID must be a positive integer"`.  `main_ro.c` had already been
fixed to accept hex UIDs but the fix was not carried over to `main.c`.

**Affected commands:** `mark-read`, `mark-unread`, `mark-starred`,
`remove-starred`, `add-label`, `remove-label`, `show`, `list-attachments`,
`save-attachment`.

**Solution:** `parse_uid()` in `main.c` now first checks whether the input
is a 16-character all-hex-digit string and accepts it as-is.  Decimal IMAP
UIDs continue to be accepted and zero-padded to 16 characters.  Garbage
input is still rejected.

**Regression guard:** Functional test Phase 28 (28.1–28.11) verifies all
nine affected commands accept a hex UID, that garbage is rejected, and that
decimal IMAP UIDs continue to work.

---

### US-5 — IMAP accounts not affected

> **As a user** with both IMAP and Gmail accounts configured,
> **when** I run `email-sync --rebuild-index`,
> **I want** IMAP accounts to be silently skipped,
> **so that** the command does not fail on accounts that do not use label
> indexes.

**Solution:** `email_service_rebuild_indexes()` checks `cfg->gmail_mode`
and skips accounts where it is 0.  A message is printed for each skipped
account in verbose/debug mode.

---

## Gmail .hdr consistency — labels CSV and flags integer

### Background

Each cached Gmail message is stored in a `.hdr` file with five
tab-separated fields:

```
from\tsubject\tdate\tINBOX,UNREAD,CATEGORY_SOCIAL\t1
                     ──────────────────────────────  ─
                         labels CSV  (field 4)      flags int (field 5)
```

The **labels CSV** is the source of truth: `rebuild_label_indexes` reads
it to reconstruct all `.idx` files.  The **flags integer** is a derived
cache: the list view reads it to display `N` (unread) and `★` (starred)
without parsing the CSV.

Both fields must be updated atomically whenever a label changes, or the
next full sync's `rebuild_label_indexes` step will silently undo the
mutation.

---

### US-7 — mark-read reverts after sync

> **As a Gmail user** who marks hundreds of messages as read,
> **when** I run `email-sync` afterwards,
> **I want** those messages to stay read,
> **so that** my read-state changes are not silently discarded.

**Problem:** `email_service_set_flag` (mark-read / mark-unread) updated
the `.idx` and the flags integer, but not the labels CSV.
`rebuild_label_indexes` (called on every full sync) read the stale
labels CSV, found `UNREAD` still present, and re-inserted the UID into
`UNREAD.idx` — making the message appear unread again.

**Solution:** `email_service_set_flag` now also calls
`local_hdr_update_labels` to add/remove `"UNREAD"` / `"STARRED"` from
the labels CSV.  `local_hdr_update_labels` itself was also fixed to
recompute the flags integer from the resulting label set, keeping both
fields in sync in a single write.

**Regression guard:** Phase 29 tests 29.1–29.2 (mark-read, mark-unread).

---

### US-8 — n/f key changes revert after sync

> **As a Gmail user** in the TUI message list,
> **when** I press `n` to toggle read/unread or `f` to toggle starred,
> **I want** the change to survive the next sync,
> **so that** interactive flag changes are not silently lost.

**Problem:** The TUI `n`/`f` key handler called `label_idx_add/remove`
and `local_hdr_update_flags` but not `local_hdr_update_labels`, leaving
the labels CSV stale.

**Solution:** The `n`/`f` handler now calls `local_hdr_update_labels`
(which updates both CSV and flags integer) instead of the separate calls.

**Regression guard:** Phase 29 tests 29.3–29.4 (mark-starred, remove-starred).

---

### US-9 — add-label / remove-label changes revert after sync

> **As a Gmail user** who runs `email-cli add-label <uid> UNREAD` or
> uses the label picker (`t` key) to toggle a label,
> **I want** the change to survive the next sync,
> **so that** label edits are not silently discarded.

**Problem:** `email_service_set_label` (CLI `add-label`/`remove-label`)
and the TUI label picker both updated `.idx` but not `.hdr` labels CSV.

**Solution:** Both now call `local_hdr_update_labels` after updating `.idx`.

**Regression guard:** Phase 29 tests 29.5–29.6 (add-label UNREAD, remove-label UNREAD).

---

### US-10 — archive / remove-label operations revert after sync

> **As a Gmail user** who presses `a` (archive) or `d` (remove current
> label) in the TUI,
> **I want** the label change to survive the next sync,
> **so that** archived messages don't reappear in INBOX.

**Problem:** The `a` and `d` key handlers called `label_idx_remove` but
not `local_hdr_update_labels`, leaving the labels CSV stale.

**Solution:** Both handlers now call `local_hdr_update_labels` to remove
the relevant label from the CSV (and recompute the flags integer).

**Regression guard:** Covered indirectly by Phase 29 (same code path in
`local_hdr_update_labels`).

**Spec:** `docs/spec/email-sync.md` → `--rebuild-index` → Behaviour.
