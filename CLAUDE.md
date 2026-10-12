# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Workspace policies

This project follows the workspace policies in `../policies/` (GitHub
`csjpeter/policies`, private). They stand above this project. **Before changing
anything, read the ones that touch your task**; start from
`../policies/README.md`, which lists them all. When this file and a policy
disagree, stop and tell the owner; do not choose silently.

The ones that bind every task:

- `change-flow-model.md` and `issue-tracking-model.md`: every change has a
  ticket in the issue manager and a branch named exactly after its key; commit
  messages carry the key; a finished ticket goes to `READY`, and the owner closes.
- `project-boundaries-model.md`: this project writes only inside its own
  directory. **It never writes into `policies/`**; a finding about a policy or
  another project is filed as a ticket under the key that owns it.
- `testing-model.md`: every behaviour has a test that fails without the change;
  tests keep every path under `build/` and never use `/tmp`.
- `manage-sh-model.md`, `packaging-and-release-model.md`,
  `dependencies-model.md`: build, package and link the way they say.
- `source-header-model.md`, `licensing-model.md`: every source and script
  carries the header; the licence is the project's own.
- `platform-scope-model.md`, `private-names-model.md`: Linux only for now; the
  owner's private domain appears in no file.

Working through the approved tickets of this project by itself is the
`work-on-tickets` skill; the supervisor may ask for one ticket by key.

## Build Commands

```bash
./manage.sh build      # Release build → bin/{email-cli,email-cli-ro,email-tui,email-sync,email-import-rules}
./manage.sh debug      # Debug build with Address Sanitizer (ASAN)
./manage.sh test       # Unit tests with ASAN
./manage.sh valgrind   # Unit tests with Valgrind
./manage.sh coverage   # GCOV/LCOV coverage report (>90% goal for core/infra)
                       # also runs the functional suite and the PTY tests
./manage.sh clean      # Remove build artifacts
./manage.sh integration # Integration test against real Dovecot IMAP (Docker required)
./manage.sh integration-local # APPEND integration test against a local Dovecot (no Docker)
./manage.sh imap-down  # Stop integration container (volume preserved)
./manage.sh imap-clean # Remove integration container and volume
./manage.sh deps       # Install system dependencies (Ubuntu 24.04 / Rocky 9)
./manage.sh run        # Release build + launch email-cli
./manage.sh install    # Release build + install binaries into ~/.local/bin
./manage.sh uninstall  # Remove installed binaries from ~/.local/bin
./manage.sh package [deb|rpm|all]  # Build CPack distribution package(s)
./manage.sh clean-logs # Purge application log files
./manage.sh help       # Show available commands
```

There is no Makefile — `manage.sh` calls CMake directly. A single unit suite runs with `./manage.sh test <name-part>`, a functional phase with `./manage.sh functional <phases>`, a PTY suite with `./manage.sh pty <suite> [<name-part>]` (see `docs/dev/testing.md`).

The functional suite has no dedicated `manage.sh` target; run it directly with
`./tests/functional/run_functional.sh` after a build (it drives the mock IMAP /
Gmail servers itself).

## Architecture

The project follows a strict layered CLEAN architecture with zero circular dependencies:

```
Application    →  src/main.c, main_ro.c, main_sync.c, main_tui.c, main_import_rules.c
Domain         →  libemail/src/domain/email_service.c
Infrastructure →  libemail/src/infrastructure/{config_store,local_store,imap_client,setup_wizard,
                                                  gmail_auth,gmail_client,gmail_sync,mail_client,
                                                  mail_rules,when_expr}.c
Core           →  libemail/src/core/{logger,fs_util,config,mime_util,html_parser,html_render,
                                     imap_util,input_line,path_complete,json_util}.c + raii.h
Platform       →  libemail/src/platform/{terminal,path,process,credential_key}.h + html_medium.h
                    libemail/src/platform/posix/{terminal,path,process,credential_key,html_medium}.c    (Linux)
Write library  →  libwrite/src/{smtp_adapter,compose_service}.c
```

Dependency rule: every layer may depend on layers below it; `platform/` sits
alongside `core/` and is depended upon by `domain/` and `infrastructure/`.
No layer may contain `#ifdef` guards for platform selection — that is the
build system's (CMake's) responsibility.

**Data flow:** `main.c` initializes logger, paths, and local store → loads config (or runs `setup_wizard` on first run) → calls `email_service` → which uses `mail_client` (dispatch layer) to talk to the IMAP server or Gmail REST API → results go to stdout, messages stored in `~/.local/share/email-cli/accounts/`, diagnostics to `~/.cache/email-cli/logs/`.

**Gmail support:** Gmail accounts use the native Gmail REST API (OAuth2 + libcurl), not IMAP. The setup wizard offers account type selection ([1] IMAP / [2] Gmail). Gmail uses label-based navigation instead of folders. See `docs/spec/gmail-api.md` for the full specification.

**Config** is stored at `~/.config/email-cli/config.ini` with mode 0600 (IMAP host, user, password, folder). Gmail accounts add `GMAIL_MODE=1`, `GMAIL_REFRESH_TOKEN`, and optional `GMAIL_CLIENT_ID`/`GMAIL_CLIENT_SECRET` fields.

**Local store** uses account-based directories with reverse digit bucketing and text indexes. See `docs/spec/local-store.md` for the full specification.

## RAII Memory Safety

The project uses GNU `__attribute__((cleanup(...)))` for automatic resource deallocation — see `libemail/src/core/raii.h`. Macros like `RAII_STRING`, `RAII_FILE`, `RAII_DIR` eliminate manual `free()`/cleanup boilerplate. New resources should use these macros rather than manual cleanup.

## Custom Test Framework

No external test libraries are used. Tests use `ASSERT(condition, message)` and `RUN_TEST(test_func)` macros from `tests/common/test_helpers.h`. Unit tests live in `tests/unit/`, functional tests in `tests/functional/` (with a mock IMAP state machine server).

## Language & Standard

C11 (`-std=c11`). Linked against libssl (OpenSSL) for IMAP, and libcurl for SMTP (libwrite) and Gmail REST API (libemail). All public functions should have Doxygen-style comments.

## GNU/Linux CLI Conventions

New commands and options must follow standard GNU/Linux CLI conventions:

- **`--help`** must be supported for every command and for the bare binary.
  `email-cli --help` shows the general help page.
  `email-cli <cmd> --help` shows the same page as `email-cli help <cmd>`.
- **`--version`** should be added when a version string is available.
- Long options use `--option-name` (double dash, lowercase, hyphen-separated).
- Short single-character aliases (`-h`, `-v`) are welcome but not required.
- Exit codes: `0` = success, non-zero = failure (use `EXIT_SUCCESS`/`EXIT_FAILURE`).
- Errors go to **stderr**; normal output to **stdout**.

## Dependency Policy

**Keep external dependencies minimal.**  The project intentionally uses only the C standard library, POSIX, libssl (OpenSSL), and libcurl (SMTP only, in libwrite).  Before reaching for a new library, exhaust stdlib/POSIX options first.  New runtime dependencies require explicit justification and user approval.

## Platform

**The target platform is Linux (Debian).** A release for, or support of, macOS,
iOS, Android or Windows is not a goal and nothing in this project may claim it
(see `../policies/platform-scope-model.md`).  What binds the project to Linux is
listed in `docs/portability.md`; keep that file current when a new Linux-specific
dependency is added.

Platform-specific behaviour is isolated in `libemail/src/platform/`: a thin C
header per concern (`terminal.h`, `path.h`, `process.h`, `credential_key.h`,
`html_medium.h`) and its implementation in `platform/posix/`, chosen by CMake.
No `#ifdef` platform guards in shared code, and none written for a platform
nobody builds on.  Prefer standard C11 / POSIX where the portable form is no
harder to write; GCC is the compiler.

## Documentation

```
docs/
  README.md               ← index
  spec/                   ← behavioural specification (commands, local-store, etc.)
  dev/                    ← developer guides (testing, logging)
  adr/                    ← Architecture Decision Records (CLEAN arch, RAII, test framework)
  userstories/            ← numbered user stories (US-1 …), one file each
libs/
  libptytest/             ← PTY-based terminal test library (self-contained)
```

## Claude Code Usage Guidelines

- Use direct Grep/Read/Edit tools for targeted changes. Use Agent for genuinely parallel, independent work.
- Avoid reading entire large files when only a section is needed — use offset/limit.
- Prefer direct edits over exploratory agents when the change location is known.
- Do **not** keep a persistent project-memory store (e.g. `.claude/memory/`).
  The authoritative project state lives in the repository itself: `docs/spec/`
  for behaviour, `docs/adr/` for decisions, the issue manager (`issue-cli`, project `EMAIL`) for
  open work and defects, and the git history for what has been done.
