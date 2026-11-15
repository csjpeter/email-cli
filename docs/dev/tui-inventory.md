# email-tui: libtuikit inventory (EMAIL-290)

Rule: `policies/tui-interface-model.md`, "The toolbox first: no improvised
widgets". One row per part of email-tui. **Status** is one of:

- `done`: drawn and handled by a libtuikit widget (or an adapter over it);
- `waits TUI-N`: the library lacks the widget, ticket `TUI-N` asks for it;
- `todo`: a libtuikit widget exists, the hand-made code is still in place and
  is to be replaced (follow-up of EMAIL-290; the pty tests pin the behaviour).

| Part | Where | libtuikit widget / hand-made code | Status |
|---|---|---|---|
| Raw mode, key decoding, size, char width | platform/posix/terminal.c | `tui_tty_*`, `tui_cp_width` (the file is an adapter) | done |
| Masked password prompt | platform/posix/terminal.c | `tui_tty_read_password_masked` | done |
| Line editor (one-line fields) | core/input_line.c | `tui_lineedit_handle` | done |
| Status line of a screen | core/status_line.c, `screen_bar` | `tui_statusline` | done |
| Header bar and notices | core/tui_frame.c | `tui_header` | done |
| Accounts, folder tree, flat folder view | email_service.c | `tui_table` via `tui_frame_draw_table` | done |
| Message list | email_service.c | `tui_table` | done |
| Gmail label browser | email_service.c | `tui_table` | done |
| Help overlay, label picker, discard dialog | email_service.c, main_tui.c | `tui_dialog_help`, `tui_dialog_checklist`, `tui_dialog_confirm` | done |
| Address and file drop-downs of compose | main_tui.c | `tui_table` overlay | done |
| Compose body editor | core/body_editor.c | `tui_textedit` | done |
| History view | domain/history_view.c | `tui_histview` | done |
| Rules editor list, condition list | main_tui.c | `tui_table` | done |
| Reader: scrolling body with styles | email_service.c | hand-made printf; needs styled text | waits TUI-122 |
| Reader: HTML to styled text | core/html_render.c, html_parser.c | hand-made renderer | waits TUI-124 |
| Rules: `(y/N)` prompts on the bottom row | main_tui.c (`tui_read_confirm`, 5 places) | hand-made printf and key read | waits TUI-123 |
| Search line `/` and `Search [..]:` | email_service.c | printf, own cursor | todo (`tui_lineedit`, `tui_cmdline`) |
| Pre-compose dialog fields (To, Cc, Bcc, Subject, Attach) | main_tui.c (`compose_dialog`) | printf rows and cursor | todo (`tui_fieldset`) |
| Add-attachment prompt | main_tui.c | printf rows | todo (`tui_fieldset`) |
| Send screen | main_tui.c (`send_screen_*`) | printf rows | todo (`tui_textview`) |
| Unsent-draft question | main_tui.c | `tui_dialog_confirm` | done |
| Rule add form | main_tui.c (`tui_rules_add_form`) | printf rows and cursor | todo (`tui_fieldset`) |
| Rule detail | main_tui.c (`tui_rules_detail`) | printf rows | todo (`tui_textview`) |
| Rules report | main_tui.c (`tui_rules_report`) | printf rows | todo (`tui_textview`) |
| Gmail re-authorisation text | main_tui.c (`tui_reauth_gmail`) | printf text | todo (`tui_dialog_message`) |
| Path Tab-completion row | core/path_complete.c | printf drop row | todo (`tui_table` overlay, `tui_fieldset_update_completion`) |
| Folder size table (non-TUI listing) | email_service.c | printf table | todo (`tui_table`) |
