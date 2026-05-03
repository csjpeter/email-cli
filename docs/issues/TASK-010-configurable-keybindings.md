# TASK-010 — Configurable Keyboard Shortcuts

## Summary

All interactive key bindings in `email_service.c` (list view, reader, folder browser)
and `main_tui.c` are hardcoded.  Power users and terminal environments with conflicting
shortcuts cannot customise them.

## Motivation

- The `'l'` = rules-editor binding was chosen as a compromise after `'R'` was already
  used for refresh (`'U'`) and `'r'` for reply.  With configurable bindings, users
  could assign any key they prefer.
- Blind / assistive-technology users sometimes need to remap keys to avoid conflicts
  with screen-reader shortcuts.

## Acceptance criteria

| # | Criterion |
|---|-----------|
| 1 | A `[keybindings]` section in `~/.config/email-cli/config.ini` (or a dedicated `keybindings.ini`) lets users override individual actions. |
| 2 | Every action in the list view, reader, and folder/label browser has a named identifier (e.g. `list.rules_editor`, `list.refresh`, `reader.toggle_raw`). |
| 3 | Unrecognised keys in the config file are rejected with a clear error on startup. |
| 4 | Built-in defaults are used for any action not present in the config file. |
| 5 | Two actions cannot be bound to the same key; the program exits with an error if a conflict is detected at startup. |
| 6 | `email-tui --list-keys` prints the full action → key mapping (including any user overrides). |
| 7 | The help panel (`?`) reflects the actual configured keys, not hardcoded strings. |
| 8 | Unit tests cover config parsing, conflict detection, and default fallback. |

## Scope

Affects: `libemail/src/domain/email_service.c`, `src/main_tui.c`,
`libemail/src/infrastructure/config_store.c` (or a new `keybindings.c`).

## Related

- US-62: TUI rules editor (`'l'` binding introduced here)
- US-19: background sync refresh (`'U'` binding)
- TASK-009: US-62 PTY tests
