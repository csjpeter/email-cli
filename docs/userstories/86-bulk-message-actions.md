# US-86 — Mark Messages and Act on Them Together

**As a** user with a long message list,
**I want** to mark several messages and change a property of all of them at once,
**so that** I do not repeat the same key for every message.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | In the message list `Space` marks the row under the cursor (a `*` in the mark column) and moves the cursor down; on a marked row it unmarks. |
| 2 | While rows are marked the header line shows `[N marked]`. |
| 3 | `x` clears all marks. `Esc` clears the marks first; a further `Esc` leaves the list as before. |
| 4 | `m` opens a one-key action prompt that acts on the marked messages, or on the row under the cursor when none is marked: `r` read, `u` unread, `f` star, `F` unstar, `m` move (asks for the folder or label), `D` move to Trash / delete in Trash (asks `y/N` with the count). |
| 5 | On Gmail accounts the prompt also offers `l` add label and `L` remove label. |
| 6 | `Esc` at the prompt cancels and keeps the marks. |
| 7 | After the action the feedback line reads `N message(s) <done>` (with `, M failed` when some failed) and the marks are cleared. |
| 8 | Marks follow the messages when the list is re-sorted or filtered (marks are kept per message, not per row position). |
