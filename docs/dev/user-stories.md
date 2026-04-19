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

### US-5 — IMAP accounts not affected

> **As a user** with both IMAP and Gmail accounts configured,
> **when** I run `email-sync --rebuild-index`,
> **I want** IMAP accounts to be silently skipped,
> **so that** the command does not fail on accounts that do not use label
> indexes.

**Solution:** `email_service_rebuild_indexes()` checks `cfg->gmail_mode`
and skips accounts where it is 0.  A message is printed for each skipped
account in verbose/debug mode.

**Spec:** `docs/spec/email-sync.md` → `--rebuild-index` → Behaviour.
