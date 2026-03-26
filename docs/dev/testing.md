# Developer Guide: Testing

## Running Tests

```bash
./manage.sh test        # Unit tests with AddressSanitizer
./manage.sh valgrind    # Unit tests with Valgrind
./manage.sh coverage    # Coverage report + functional tests
./manage.sh integration # End-to-end against Dockerized Dovecot (requires Docker)
```

There is no mechanism to run a single test in isolation — all unit tests run
together via `build/tests/unit/test-runner`.

## Unit Tests (`tests/unit/`)

Each source module has a corresponding test file:

| Test file | Module under test |
|-----------|-------------------|
| `test_fs_util.c` | `src/core/fs_util.c` |
| `test_logger.c` | `src/core/logger.c` |
| `test_config.c` | `src/infrastructure/config_store.c` |
| `test_wizard.c` | `src/infrastructure/setup_wizard.c` |
| `test_curl.c` | `src/infrastructure/curl_adapter.c` |

### Writing a New Test

1. Add a `void test_foo(void)` function in the appropriate `test_*.c` file.
2. Use `ASSERT(condition, "message")` for each assertion.
3. Register it with `RUN_TEST(test_foo)` in `test_runner.c`.

```c
void test_foo(void) {
    int result = foo(42);
    ASSERT(result == 0, "foo(42) should return 0");
}
```

### Struct Initialization

Always initialize stack-allocated structs with `= {0}` to avoid Valgrind
uninitialized-value warnings:

```c
Config cfg = {0};   // correct
Config cfg;         // wrong — cfg.ssl_no_verify is garbage
```

### Stdin-dependent Tests

`setup_wizard_run()` reads from stdin. To test it, redirect stdin via a pipe:

```c
int pipefd[2];
pipe(pipefd);
ssize_t n = write(pipefd[1], input, strlen(input));
ASSERT(n > 0, "write to pipe should succeed");
close(pipefd[1]);
int saved = dup(STDIN_FILENO);
dup2(pipefd[0], STDIN_FILENO);
close(pipefd[0]);
Config *cfg = setup_wizard_run();
dup2(saved, STDIN_FILENO);
close(saved);
```

## Functional Tests (`tests/functional/`)

The mock IMAP server (`mock_imap_server.c`) listens on TCP port 9993 and
implements a minimal IMAP state machine:

- Accepts multiple sequential connections (SEARCH uses one connection,
  each FETCH uses a separate connection).
- Always authenticates any credentials.
- Returns `* SEARCH 1` (one message exists).
- Returns a hardcoded test email body on FETCH.

Run via `./tests/functional/run_functional.sh`. The script:
1. Compiles and starts the mock server in background.
2. Runs `email-cli` with a temporary config pointing at `imap://localhost:9993`.
3. Checks 5 assertions on the output.
4. Kills the server on exit (via trap).

## Integration Tests (`tests/integration/`)

Requires Docker. Runs a real Dovecot IMAP server with:
- Self-signed TLS certificate (openssl, `ssl_min_protocol = TLSv1.2`)
- Two seed emails pre-loaded into the mailbox

```bash
./manage.sh integration   # start Dovecot if needed, run test, assert output
./manage.sh imap-down     # stop container (volume preserved)
./manage.sh imap-clean    # stop + remove volume
```

## Coverage Requirements

| Scope | Target |
|-------|--------|
| `src/core/` + `src/infrastructure/` combined | >90% line coverage |
| `src/domain/` | best-effort |
| `src/main.c` | best-effort (wiring code) |

Known uncoverable lines in `setup_wizard.c`: TTY manipulation branches
(`tcgetattr`/`tcsetattr` inside `hide && is_tty`) require a PTY to test.
These 6 lines are excluded from the >90% target by convention.
