# email-tui: key tables per view (PTY study, EMAIL-140)

One section per view: key, label on screen, behaviour, test.  The tests are PTY
tests against the mock servers; where a view is studied at 80x24 the test says
so.  Policy: `policies/tui-interface-model.md`.

## Attachment picker and save prompts (EMAIL-147)

The picker opens with `a` in the reader when the message has two or more
attachments; with exactly one, `a` goes straight to the save prompt; with none,
`a` shows `No attachments` in the reader.  Saving all attachments is in the `:`
menu of the reader (`Save all attachments`, prompt `Save all to:`).  The help
(`?`) is made from the key table view `attach-picker`
(`libemail/src/core/key_table.c`).  Tests are in `tests/pty/test_pty_views.c`
(80x24, the mock with `MOCK_IMAP_ATTACHMENTS`), test names below.

| Key | Label | Behaviour | Test |
|---|---|---|---|
| `Down`, `j` | `select` | cursor down one row, stops at the last | `test_picker_navigation` |
| `Up`, `k` | `select` | cursor up one row, stops at the first | `test_picker_navigation` |
| `PgDn` / `PgUp` | help: Page down / up | one page (rows of the screen less 4), clamped | `test_picker_navigation` |
| `Home`, `g` / `End`, `G` | help: First / Last attachment | first / last row | `test_picker_navigation` |
| `Enter` | `choose` | the row under the cursor; prompt `Save as:` with the path in `Downloads` or the home directory | `test_picker_navigation`, `test_picker_choose_second_then_cancel`, `test_show_save_single` |
| `Esc`, `q`, `Backspace` | `Esc/q/BS back` | back to the reader | `test_picker_back_keys` |
| `ctrl-c` | help: Quit | the program quits at once | `test_picker_ctrl_c_quits` |
| `?` | help | overlay from the key table; `Esc` closes only the overlay | `test_picker_help` |
| any other key | | changes nothing, shows nothing | `test_picker_unbound_key` |
| `[n/N]` | status line | cursor row of the count | `test_picker_back_keys` |

Save prompts (`Save as:`, `Save all to:`; the line editor is in `input-line.md`):

| Key | Behaviour | Test |
|---|---|---|
| `Enter` | save and say `Saved: PATH` / `Saved N/N files to: DIR` | `test_show_save_single`, `test_a_one_attachment`, `test_show_save_all_confirm` |
| `Esc` | cancel, nothing written, back to the reader | `test_picker_choose_second_then_cancel`, `test_a_one_attachment`, `test_show_save_all_cancel` |
| `Home` `End` `Left` `Right` `Backspace` `Delete` | line editing | `test_show_save_input_line_cursor` |
| `Tab` / `Shift-Tab` | path completion forward / back | `test_show_save_tab_completion` |

0 attachments: `test_a_without_attachments`; 1 attachment: `test_a_one_attachment`;
many (25, paging): `test_picker_navigation`.

Findings of the study, all fixed with EMAIL-147: EMAIL-176 (`q` was ignored),
EMAIL-177 (`ctrl-c` went back instead of quitting), EMAIL-178 (no `j k g G
Home End`, `PgUp`/`PgDn` moved one row), EMAIL-179 (no `?`), EMAIL-180 (`a`
without attachments said nothing).  Already right: `Esc` and `Backspace` back,
arrows, `Enter`, the save and cancel paths, the file name sanitising, the
count in the status line.
