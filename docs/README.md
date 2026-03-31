# Documentation

## Behavioural Specification

Authoritative what-and-how reference; sufficient to re-implement the program from scratch.

- [Spec index](spec/README.md)
- [Commands](spec/commands.md) — all commands, options, argument parsing, exit codes
- [Output formats](spec/output-formats.md) — exact output for every command and mode
- [IMAP protocol](spec/imap-protocol.md) — operations issued, flag handling, encoding
- [Local store](spec/local-store.md) — account-based store, reverse digit bucketing, text indexes
- [Pagination](spec/pagination.md) — terminal detection, pager, batch mode

## User Guide

See the project [README](../README.md) for installation, configuration, interactive mode, and CLI reference.

## Developer Guides

- [Testing](dev/testing.md) — unit, functional, integration tests; coverage requirements
- [Logging](dev/logging.md) — log levels, rotation, IMAP traffic capture

## Architecture Decision Records

- [ADR-0001: CLEAN Layered Architecture](adr/0001-clean-architecture.md)
- [ADR-0002: RAII Memory Safety](adr/0002-raii-memory-safety.md)
- [ADR-0003: Custom Test Framework](adr/0003-custom-test-framework.md)
