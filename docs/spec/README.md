# Behavioural Specification

This directory contains the authoritative behavioural specification for `email-cli`.
It is written at a level of detail sufficient to re-implement the program from scratch.

## Contents

| File | Topic |
|------|-------|
| [commands.md](commands.md) | All CLI commands, options, argument parsing, exit codes |
| [output-formats.md](output-formats.md) | Exact output format for every command and mode |
| [imap-protocol.md](imap-protocol.md) | IMAP operations issued, flag handling, folder encoding |
| [caching.md](caching.md) | Local cache layout, read/write policy, eviction rules |
| [pagination.md](pagination.md) | Pagination logic, terminal detection, interactive pager |
| [html-rendering.md](html-rendering.md) | MIME part selection, HTML parser, renderer, ANSI colour policy, pager integration |

## Relationship to other docs

- `docs/user/` — end-user guides (getting started, configuration, usage)
- `docs/dev/` — developer guides (testing, logging)
- `docs/adr/` — Architecture Decision Records (why, not what)
- `docs/spec/` — **this directory**: authoritative what-and-how specification
