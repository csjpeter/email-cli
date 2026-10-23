# Operation history and revert (EMAIL-226)

Specification of the history of state-changing operations, and of reverting
them, in `email-cli` and `email-tui`.  It implements the workspace policy
`undo-model.md`.  Code: `libemail/src/infrastructure/op_history.c` (the store),
`libemail/src/domain/history_service.c` (recording and reverting mail
operations), `libemail/src/domain/history_view.c` (the TUI page, libtuikit's
`tui_histview`), and the `history` / `revert` commands in `src/main.c`.
Tests: `tests/unit/test_history.c`, phase 140 of
`tests/functional/run_functional.sh`, `tests/pty/test_pty_history.c`.

## Where the history lives

A mail server keeps no history of who changed what, so the programs keep their
own: one append-only file, `<data dir>/email-cli/history.tsv` (mode 0600,
`$XDG_DATA_HOME` or `~/.local/share`), one tab-separated line per step.  A line
is written under `flock(2)` right after the change it records succeeded, by the
function that made the change; the history is therefore in **the tool's own
store**, written in the same call as the change (no transaction spans a mail
server and a file, so a change the server refused is never recorded, and a crash
between the two loses at most the record of one step, never the change).  The
file is never rewritten.

Each step records: its number, **when** (UTC, ISO 8601), **who** (the operating
system user), the account, the **thing** that changed (object name and field),
the **old** and the **new** value, the operation name, the step it reverts (if
any) and its classification.  An operation that changes nothing (a flag set to
the value it had, a move to the folder the mail is in) records nothing.  A bulk
operation writes one step per object.

**Secrets** (a password, a token, a client secret) are recorded as the word
`changed`, never with their value, in the old and the new column alike.

Object names: `mail FOLDER/UID` (IMAP), `mail UID` (Gmail), `folder NAME`,
`label NAME`, `rule NAME`, `account` (the fields of one account), `settings`,
`sent mail`.

## Operations and their classification

| Operation | Class | Why |
|-----------|-------|-----|
| Read / unread, star, answered and other flags (IMAP `STORE`, Gmail `UNREAD`, `STARRED`) | revertable | the inverse is the same flag set the other way on the server |
| Move a message (IMAP), also trash and archive of IMAP | revertable | the inverse is a move back, found again by its Message-ID (the UID changes with the move) |
| Gmail: add / remove a label, move, trash, archive | revertable | the inverse is the label change, in the other direction |
| Mark junk / not junk | revertable | the inverse is the other mark |
| Rule add, edit, delete | revertable | the rule file is the program's own; the old rule is in the step |
| Account fields that are not secrets (host, user, folder, ...) | revertable | the old value is in the step |
| Setting: credential obfuscation | revertable | the old value is in the step |
| Create a folder or label | revertable | the inverse is a delete of the empty, just created folder |
| Add an account | compensable | removing it is a counter-step, but not the exact inverse: the first sync and the stored credentials are not undone, and a removal is itself irreversible |
| Remove an account | irreversible | its credentials and local store are gone; the secrets are not recorded |
| Permanent delete (a message deleted from the Trash) | irreversible | the server has no undelete |
| Send / forward a mail | irreversible | a sent mail cannot be recalled |
| Delete a folder or a Gmail label | irreversible | the messages in it are gone with it |
| Change of a secret (password, token, client secret) | irreversible | the old value is not recorded, so there is nothing to restore |

## Reverting

A revert performs the inverse of the recorded step on the server (a move or a
flag change is another move or flag change, not a local edit) and then writes
**a new step** that names the step it reverts.  Nothing is rewritten.  The
revert is an ordinary step: it can be reverted, which restores the original
change.  A step already reverted shows `reverted by N` and is refused a second
time; the revert shows `revert of N`.

A revert is refused, in words, when:

* the step is not revertable (irreversible), or already reverted;
* **it was overtaken**: the thing is not in the state the step left it in (the
  message was flagged again, moved elsewhere, the rule edited); the reason names
  what is there now.  Where the server refuses (the folder no longer exists, the
  message is gone), the refusal is the **server's own words**;
* the local store lacks the mail (a revert needs the mail's current flags, and
  for IMAP its Message-ID): the refusal says `the local store has not the mail
  ...`;
* the step belongs to another user and the caller is not an administrator (root).
  `history` lists a user's own steps; an administrator sees everyone's.

A Gmail trash and archive are reverted by the label state they left, so the
revert of such a revert needs no memory of its own.

## Interfaces

**email-cli:**

```
email-cli [<account>] history [<object>] [--limit N]
email-cli revert <id> [-q]
```

`history` prints `ID WHEN WHO OP WHAT`, newest first, each line ending in
`[revertable]`, `[compensable]`, `[not revertable]`, `[reverted by N]` or
`[revert of N]` (a line longer than the terminal wraps).  With a colour terminal
the header is bold and a step that is not revertable is dimmed.  Without
`<object>` the recent changes are listed; with it every step of that object.
`revert` shows the step and asks `Continue? [y/N]` (the default, and the end of
input, is No: `Not done.`); `-q` / `--quiet` answers yes.  `-q` also answers the
confirmation of the irreversible operations below.  `email-cli-ro` offers
neither command: it reads mail only.

**Irreversible operations confirm before they are done**, default No, and the
question says so (`This cannot be undone and is not revertable. Continue?
[y/N]`): `delete-folder`, `delete-label`, `remove-account`, the permanent delete
(`trash` in the trash folder), and sending (`forward`, `reply`, `send`).

**email-tui:** `H` in the message list opens the recent changes; `H` in the
reader opens the history of that message (key table, `libemail/src/core/key_table.c`).
The page is libtuikit's `tui_histview`: the keys of every list, `:` for the menu
with **Revert**, `Enter` on a step opens the confirmation naming the step, the
thing and what it will become (default No), a step marked not revertable shows a
notice and no dialog, a refusal is shown in one line at the bottom, `?` opens
the help, `Esc`, `q`, `Backspace` go back.  Tests: `tests/pty/test_pty_history.c`
at 80x24 with the colours checked.  Parity: `docs/interface-parity.md`.
