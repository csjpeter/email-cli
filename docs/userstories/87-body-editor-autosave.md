# US-87 — Write the Message Body in the Built-in Editor, Draft Autosaved

**As a** user composing or replying in `email-tui`,
**I want** to write the body in the program's own editor, with the text saved
as I go,
**so that** I need no external editor and never lose a message I was writing.

---

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | After the header dialog the body is written in a full-screen editor (libtuikit `tui_textedit`): a title bar with To and Subject, the text, and a bottom line `Esc=save and continue  Ctrl-S=save`. No external editor and no temporary file is used. |
| 2 | Typing, pasting (bracketed paste keeps its newlines), wide and accented characters, Tab, Enter, Backspace/Delete, arrows, Home/End, PgUp/PgDn and Ctrl-K work; a long line scrolls horizontally. |
| 3 | There is no question when leaving: Esc saves the draft and goes on to the review screen. Ctrl-S saves and stays (`saved` is shown). |
| 4 | A changed text is also saved after 3 seconds without typing (`autosaved`), so a killed terminal loses at most the last few seconds. |
| 5 | The draft (headers, attachment paths, body) is one file, `compose-draft` in the program's data directory, mode 0600. |
| 6 | The next compose finds the draft and asks `Resume it? [y/N]`, naming its To and Subject; `y` restores headers, attachments and body, anything else starts a new message (the old draft is replaced at the first save). |
| 7 | The review screen is the place to discard: `q`/Esc asks `Discard draft? To: …, Subject: …, N line(s), M attachment(s) [y/n]`; `y` removes the draft, `n` keeps it. |
| 8 | Sending removes the draft; a send that fails is kept in the local Drafts and also removes the autosaved draft. |
| 9 | `email-tui compose` and `reply` use the same form and editor; the review screen's `b` reopens the editor with the current text. |
