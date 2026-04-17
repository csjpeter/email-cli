# Documentation

## Behavioural Specification

Authoritative what-and-how reference; sufficient to re-implement the program from scratch.

- [Spec index](spec/README.md)
- [Commands](spec/commands.md) — all commands, options, argument parsing, exit codes
- [Output formats](spec/output-formats.md) — exact output for every command and mode
- [IMAP protocol](spec/imap-protocol.md) — operations issued, flag handling, encoding
- [Local store](spec/local-store.md) — account-based store, reverse digit bucketing, text indexes
- [Pagination](spec/pagination.md) — terminal detection, pager, batch mode
- [Email sync](spec/email-sync.md) — email synchronization
- [Gmail API](spec/gmail-api.md) — Gmail native API specification
- [HTML rendering](spec/html-rendering.md) — HTML-to-text rendering
- [Input line](spec/input-line.md) — interactive line editing
- [Manifest cache](spec/manifest-cache.md) — warm-start optimization

## User Guide

See the project [README](../README.md) for installation, configuration, interactive mode, and CLI reference.

## User Stories

- [User stories directory](userstories/) — 32 user stories covering all features
- Gmail-specific: [US-27](userstories/27-gmail-account-setup.md), [US-28](userstories/28-gmail-label-navigation.md), [US-29](userstories/29-gmail-message-operations.md), [US-30](userstories/30-gmail-label-picker.md), [US-31](userstories/31-gmail-sync.md), [US-32](userstories/32-gmail-compose-send.md)

## Developer Guides

- [Testing](dev/testing.md) — unit, functional, integration tests; coverage requirements
- [Logging](dev/logging.md) — log levels, rotation, IMAP traffic capture
- [Gmail milestones](dev/gmail-milestones.md) — GML-01 through GML-24 implementation log

## Architecture Decision Records

- [ADR-0001: CLEAN Layered Architecture](adr/0001-clean-architecture.md)
- [ADR-0002: RAII Memory Safety](adr/0002-raii-memory-safety.md)
- [ADR-0003: Custom Test Framework](adr/0003-custom-test-framework.md)
- [ADR-0004: Binary Split](adr/0004-binary-split-ro-tui-sync.md) — trust boundaries for email-cli-ro / email-tui / email-sync
