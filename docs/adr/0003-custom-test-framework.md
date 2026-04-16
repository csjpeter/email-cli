# ADR-0003: Custom Minimal Test Framework (no external libs)

**Status:** Accepted

## Context

External C test frameworks (Check, CMocka, Unity) add build-system complexity and
introduce an external dependency that must be present on every developer machine
and CI runner. For a focused CLI tool, this overhead is not justified.

## Decision

Tests use two macros defined in `tests/common/test_helpers.h`:

```c
ASSERT(condition, message)   // increments run/fail counters; prints on failure
RUN_TEST(func)               // calls func(), prints PASS/FAIL header
```

The test runner (`tests/unit/test_runner.c`) calls `RUN_TEST` for each test
function and prints a summary. Exit code is non-zero if any test failed.

Tests are designed to run under three diagnostic harnesses without modification:

| Tool | How | Detects |
|------|-----|---------|
| ASAN | `-fsanitize=address` at compile time | Buffer overflows, use-after-free, leaks |
| Valgrind | `valgrind --leak-check=full ./test-runner` | Uninitialized memory, complex leaks |
| GCOV/LCOV | `-fprofile-arcs -ftest-coverage` | Untested code paths |

**Coverage goal:** >90% line coverage for the `core` and `infrastructure` layers combined.

## Consequences

- Zero additional dependencies beyond GCC, libssl (OpenSSL), and lcov/valgrind (already needed).
- No mocking framework — infrastructure dependencies are tested via real implementations
  (config_store writes real files to /tmp; imap_client uses real OpenSSL connections).
- Functional tests use a separate mock IMAP server written in C (`tests/functional/`).
- Integration tests use a real Dockerized Dovecot server (`tests/integration/`).
