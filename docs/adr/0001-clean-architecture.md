# ADR-0001: CLEAN Layered Architecture

**Status:** Accepted

## Context

A C CLI tool for IMAP email can easily become a tangle of networking, config
parsing, and business logic in a single file. As the codebase grows, this makes
testing impossible and changes risky.

## Decision

The codebase follows a strict 4-layer architecture with zero upward dependencies:

```
Application  →  src/main.c
Domain       →  src/domain/
Infrastructure → src/infrastructure/
Core         →  src/core/
```

Each layer may only depend on layers below it. No circular dependencies.

### Layer Responsibilities

| Layer | Path | Responsibility |
|-------|------|----------------|
| Core | `libemail/src/core/` | Zero-dependency utilities: logger, fs_util, config, mime_util, html_parser, html_render, imap_util, input_line, path_complete, raii.h |
| Infrastructure | `libemail/src/infrastructure/` | External system adapters: config_store, imap_client, local_store, setup_wizard |
| Domain | `libemail/src/domain/` | Business logic: email_service — coordinates fetch, does not know how config is stored |
| Application | `src/main.c` + `main_ro.c`, `main_sync.c`, `main_tui.c` | CLI entry points, wire layers together |

### Dependency Inversion

Higher layers depend on data structures, not on lower-layer internals.
`email_service_fetch_recent()` receives a `Config *` — it does not know or care
whether it came from a file, environment variable, or wizard.

### Doxygen

All public functions carry Doxygen-style comments (`@brief`, `@param`, `@return`)
so the codebase is self-documenting without a separate API reference.

## Consequences

- Unit-testing each layer in isolation is straightforward.
- Adding a new infrastructure adapter (e.g., SMTP send) does not touch Domain or Core.
- `main.c` coverage is inherently low (wiring code); the >90% coverage goal applies
  only to Core and Infrastructure.
