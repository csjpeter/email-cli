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

The project follows a strict 4-layer CLEAN architecture with zero circular dependencies:

```
Application  →  src/main.c
Domain       →  src/domain/email_service.c
Infrastructure → src/infrastructure/{config_store,curl_adapter,setup_wizard}.c
Core         →  src/core/{logger,fs_util}.c + raii.h
```

**Data flow:** `main.c` initializes logger and paths → loads config (or runs `setup_wizard` on first run) → calls `email_service_fetch_recent()` → which uses `curl_adapter` to talk to the IMAP server → results go to stdout, diagnostics to `~/.cache/email-cli/logs/`.

**Config** is stored at `~/.config/email-cli/config.ini` with mode 0600 (IMAP host, user, password, folder).

## RAII Memory Safety

The project uses GNU `__attribute__((cleanup(...)))` for automatic resource deallocation — see `src/core/raii.h`. Macros like `RAII_STRING`, `RAII_CURL`, `RAII_FILE` eliminate manual `free()`/cleanup boilerplate. New resources should use these macros rather than manual cleanup.

## Custom Test Framework

No external test libraries are used. Tests use `ASSERT(condition, message)` and `RUN_TEST(test_func)` macros from `tests/common/test_helpers.h`. Unit tests live in `tests/unit/`, functional tests in `tests/functional/` (with a mock IMAP state machine server).

## Language & Standard

C11 (`-std=c11`). Linked against libcurl + libssl. All public functions should have Doxygen-style comments.

## Dependency Policy

**Keep external dependencies minimal.**  The project intentionally uses only the C standard library, POSIX, libcurl, and libssl.  Before reaching for a new library, exhaust stdlib/POSIX options first.  New runtime dependencies require explicit justification and user approval.

## Documentation

```
docs/
  README.md               ← index
  user/                   ← end-user guides (getting-started, configuration, usage)
  dev/                    ← developer guides (testing, logging)
  adr/                    ← Architecture Decision Records (CLEAN arch, RAII, test framework)
```

## Project Memory

Current project status, architectural decisions, and user preferences are tracked in `.claude/memory/`. Read these files at the start of each session for context on what has been done and what comes next.
