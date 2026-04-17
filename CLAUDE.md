# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
./manage.sh build      # Release build → bin/email-cli
./manage.sh debug      # Debug build with Address Sanitizer (ASAN)
./manage.sh test       # Unit tests with ASAN
./manage.sh valgrind   # Unit tests with Valgrind
./manage.sh coverage   # GCOV/LCOV coverage report (>90% goal for core/infra)
./manage.sh clean      # Remove build artifacts
./manage.sh integration # Integration test against real Dovecot IMAP (Docker required)
./manage.sh imap-down  # Stop integration container (volume preserved)
./manage.sh imap-clean # Remove integration container and volume
./manage.sh deps       # Install system dependencies (Ubuntu 24.04 / Rocky 9)
```

There is no Makefile — `manage.sh` calls CMake directly. There is no mechanism to run a single test in isolation; all unit tests run together via `build/tests/unit/test-runner`.

## Architecture

The project follows a strict layered CLEAN architecture with zero circular dependencies:

```
Application    →  src/main.c, main_ro.c, main_sync.c, main_tui.c
Domain         →  libemail/src/domain/email_service.c
Infrastructure →  libemail/src/infrastructure/{config_store,local_store,imap_client,setup_wizard,
                                                  gmail_auth,gmail_client,gmail_sync,mail_client}.c
Core           →  libemail/src/core/{logger,fs_util,config,mime_util,html_parser,html_render,
                                     imap_util,input_line,path_complete}.c + raii.h
Platform       →  libemail/src/platform/{terminal,path}.h
                    libemail/src/platform/posix/{terminal,path}.c    (Linux/macOS/Android)
                    libemail/src/platform/windows/{terminal,path}.c  (MinGW-w64)
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

## Portability

**Target platforms: Linux (primary), macOS, Windows, Android.**

Prefer standard C11 / POSIX interfaces.  Where platform-specific APIs are
unavoidable, isolate them behind thin abstraction layers (e.g. a `platform/`
module) so each target only needs to implement a small, well-defined surface.

Known portability gaps that need shims before non-Linux builds work:

| API / feature | macOS | Android (NDK) | Windows |
|---|---|---|---|
| `termios.h` raw mode | ✅ | ✅ | ❌ needs `GetConsoleMode`/`SetConsoleMode` |
| `ioctl TIOCGWINSZ` | ✅ | ✅ | ❌ needs `GetConsoleScreenBufferInfo` |
| `wcwidth(3)` | ✅ | ✅ | ❌ needs bundled implementation |
| `asprintf` | ✅ | ✅ | ❌ needs a thin wrapper (available in MinGW) |
| `iconv(3)` | ✅ | ⚠️ limited (NDK r23+) | ❌ needs libiconv or `WideCharToMultiByte` |
| `__attribute__((cleanup(...)))` (RAII) | ✅ GCC / Apple Clang | ✅ Clang NDK | ✅ MinGW-w64 (GCC) |
| Home dir (`$HOME`) | ✅ | ⚠️ use app data dir | ❌ use `%USERPROFILE%` |
| Cache/config paths (`~/.cache`, `~/.config`) | ✅ | ❌ use app-specific dirs | ❌ use `%APPDATA%` |

**Compiler policy: GCC (or Clang) on every platform — MSVC is out of scope.**

| Platform | Toolchain |
|----------|-----------|
| Linux | GCC |
| macOS | GCC (Homebrew) or Apple Clang |
| Windows | MinGW-w64 (GCC) |
| Android | NDK Clang |

`__attribute__((cleanup(...)))` is supported by all of the above and is the
canonical RAII mechanism for this project.  MSVC is explicitly not a target.

**Rules for new code:**
- Never add a new POSIX/platform-specific call without noting it in the table above.
- Platform differences are resolved by the **build system, not `#ifdef` macros**.
  See the Platform Abstraction section below.
- Android TUI works only inside a terminal emulator; non-interactive (batch) mode must always be functional.

## Platform Abstraction

Platform-specific behaviour is isolated in `src/platform/`.  The layer exposes
a thin C header with a single canonical interface; each platform provides its
own implementation file.  CMake selects the right source file at configure
time — **no `#ifdef` guards in shared code**.

```
src/platform/
  terminal.h          ← canonical interface (get_cols, set_raw, restore, …)
  posix/terminal.c    ← termios + ioctl  (Linux, macOS, Android)
  windows/terminal.c  ← GetConsoleMode + GetConsoleScreenBufferInfo

  path.h              ← home_dir(), cache_dir(), config_dir()
  posix/path.c        ← $HOME / XDG
  windows/path.c      ← %USERPROFILE% / %APPDATA%
```

CMakeLists.txt pattern:

```cmake
if(WIN32)
    target_sources(email-cli PRIVATE src/platform/windows/terminal.c
                                     src/platform/windows/path.c)
else()
    target_sources(email-cli PRIVATE src/platform/posix/terminal.c
                                     src/platform/posix/path.c)
endif()
```

**Allowed `#ifdef` use:** only inside a platform implementation file itself
(e.g. to distinguish Linux vs macOS within the POSIX backend), never in
`core/`, `domain/`, `infrastructure/`, or `main.c`.

## Documentation

```
docs/
  README.md               ← index
  spec/                   ← behavioural specification (commands, local-store, etc.)
  dev/                    ← developer guides (testing, logging)
  adr/                    ← Architecture Decision Records (CLEAN arch, RAII, test framework)
libs/
  libptytest/             ← PTY-based terminal test library (self-contained)
```

## Claude Code Usage Guidelines

Cost-conscious workflow:
- **Minimise subagent spawning.** Use direct Grep/Read/Edit tools for targeted changes. Reserve Agent for genuinely parallel, independent work on large files. Use `model: "haiku"` for simple subagents (search, audit).
- **Keep context short.** `/compact` after completing each task. `/clear` when switching to unrelated tasks. Avoid reading entire large files — use offset/limit.
- **Batch small tasks** into a single session turn instead of one-task-per-turn cycles.
- **Prefer direct edits** over exploratory agents when the change location is known.

## Project Memory

Current project status, architectural decisions, and user preferences are tracked in `.claude/memory/`. Read these files at the start of each session for context on what has been done and what comes next.
