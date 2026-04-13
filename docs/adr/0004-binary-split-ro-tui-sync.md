# ADR-0004: Binary Split — email-cli-ro / email-tui / email-sync

**Status:** Accepted

## Context

The current codebase packages three distinct responsibilities into a single
binary (`email-cli`):

1. **Batch / scriptable read access** — non-interactive output for pipelines
   and AI agents.
2. **Interactive TUI** — full-screen pager, folder browser, attachment picker.
3. **Background sync** — periodic fetch from IMAP into the local store.

As write support (compose, reply, send, flag) is added, a single binary becomes
a trust-boundary problem: any consumer that should only *read* email would also
gain the capability to *send* it.  This is unacceptable for AI agent use-cases
where limiting blast radius is a hard requirement.

The capability boundary must be enforced at the **binary level**, not by
convention or flags.  An agent given access to a binary that contains no
send/write code cannot send email even if compromised or misdirected — there is
nothing to call.

## Decision

The project is split into four components that share a common library:

```
libemail/          – shared read-only core
                     local store, IMAP read, MIME parser,
                     config, header/body rendering

email-cli-ro       – read-only scriptable CLI
                     links libemail only; no SMTP, no IMAP writes
                     safe to hand to AI agents without write risk

email-tui          – full interactive TUI + scriptable write CLI
                     links libemail + write modules (SMTP, IMAP APPEND/store)
                     compose, reply, send, flag, draft management

email-sync         – background synchronisation daemon
                     links libemail only (pull/fetch, no send)
                     no interactive mode, no CLI commands
```

### Trust boundary

| Binary | Read | Send / write flags | Interactive TUI | Daemon |
|--------|------|--------------------|-----------------|--------|
| `email-cli-ro` | ✓ | ✗ | ✗ | ✗ |
| `email-tui`    | ✓ | ✓ | ✓ | ✗ |
| `email-sync`   | ✓ | ✗ | ✗ | ✓ |

`email-tui` subsumes the write-capable batch CLI; a dedicated write-capable
non-interactive binary is not planned unless a concrete use-case emerges.

### Write operations and drafts

Write operations (compose, send) that require user interaction live in
`email-tui`.  Drafts are saved to the local store first; `email-sync` picks
them up and delivers them to the IMAP server.  Time-sensitive operations (e.g.
sending a newly composed message immediately) bypass the sync queue and are
performed on-demand inside `email-tui`.

### CMake structure

Write modules (`smtp_adapter`, `imap_write`) are compiled into a separate
static library linked only by `email-tui`.  `email-cli-ro` and `email-sync`
never list these libraries in their `target_link_libraries`.  This is enforced
by the build system, not by `#ifdef` guards.

## Migration path

1. **v0.1.1** — Extract `libemail` from `src/`; current `email-cli` binary
   remains unchanged (build-system restructure only, no functional change).
2. **v0.1.2** — Introduce `email-cli-ro` as a read-only binary over
   `libemail`; the original `email-cli` is kept as-is alongside it.
3. **v0.1.3** — Introduce `email-sync` as a background sync daemon.
4. **v0.1.4** — Introduce `email-tui` as the interactive TUI binary.
5. **v0.1.5** — Transform `email-cli` into the write-capable scriptable CLI
   (compose, send, flag); links `libemail` + write modules.

## Consequences

- AI agents can be granted `email-cli-ro` access without write-email risk.
- Adding send/flag/compose code does not touch `libemail` or `email-cli-ro`.
- The build system is the enforcer of the capability boundary — no runtime
  flags, no trust-on-convention.
- More CMake targets to maintain; mitigated by a well-structured `libemail`.
- The existing `src/` layered architecture (ADR-0001) maps cleanly onto
  `libemail/src/` with the same layer rules.
