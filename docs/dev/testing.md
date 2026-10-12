# Developer Guide: Testing

## Running Tests

```bash
./manage.sh test        # Unit tests with AddressSanitizer
./manage.sh valgrind    # Unit tests with Valgrind
./manage.sh coverage    # Coverage report + functional tests
./manage.sh integration # End-to-end against Dockerized Dovecot (requires Docker)
```

### Running one test

```bash
./manage.sh test test_mime              # unit suites whose name contains "test_mime"
build/tests/unit/test-runner --list     # the suite names
./manage.sh functional 60-62            # functional phases (ranges and lists)
tests/functional/run_functional.sh --list
./manage.sh pty compose                 # one PTY suite (views gmail-tui mail-rules
                                        #   compose-dialog attachment send-local
                                        #   input-line compose)
./manage.sh pty compose test_password   # ... and only its tests whose name contains it
```

A functional selection pulls in the phases it needs (`PHASE_NEEDS` in the
script: phase 29 uses helpers defined by 26-28, and so on).  A filter that
matches nothing fails the run.

### Where tests keep their files, and which ports they use

No test uses `/tmp` and no test has a fixed port.

- Every fixture is under `build/tests/<category>/work-<pid>/`, removed when the
  process ends (the unit and PTY suites through `tests/common/test_scratch.h`,
  the functional and integration scripts through their exit trap).  A killed run
  leaves its directory under `build/`, and `./manage.sh clean` takes it.
- Every mock binds a loopback port the kernel picks (`MOCK_*_PORT` unset or 0)
  and writes the port it got to the file named by `MOCK_*_PORT_FILE`; the unit
  tests bind port 0 before they fork; the Docker integration container publishes
  its ports on kernel-picked loopback ports.
- Every suite sets `HOME` to a fixture and unsets the three `XDG_*` variables.

## Unit Tests (`tests/unit/`)

Each source module has a corresponding test file:

| Test file | Module under test |
|-----------|-------------------|
| `test_fs_util.c` | `libemail/src/core/fs_util.c` |
| `test_logger.c` | `libemail/src/core/logger.c` |
| `test_config.c` | `libemail/src/infrastructure/config_store.c` |
| `test_wizard.c` | `libemail/src/infrastructure/setup_wizard.c` |
| `test_local_store.c` | `libemail/src/infrastructure/local_store.c` |
| `test_imap_client.c` | `libemail/src/infrastructure/imap_client.c` |
| `test_mime.c` | `libemail/src/core/mime_util.c` |
| `test_imap_util.c` | `libemail/src/core/imap_util.c` |
| `test_email_service.c` | `libemail/src/domain/email_service.c` |
| `test_html_parser.c` | `libemail/src/core/html_parser.c` |
| `test_html_render.c` | `libemail/src/core/html_render.c` |
| `test_input_line.c` | `libemail/src/core/input_line.c` |
| `test_path_complete.c` | `libemail/src/core/path_complete.c` |
| `test_platform.c` | `libemail/src/platform/` |

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

### Build-dependent behaviour, and the hooks that neutralise it

`gmail_credentials.cmake` compiles a `GMAIL_DEFAULT_CLIENT_ID` and
`GMAIL_DEFAULT_CLIENT_SECRET` into the binaries.  Whether they are present is a
property of the build, so any test about "no credentials configured" would
otherwise test a different code path depending on the checkout.  That is how
`test_device_flow_no_credentials` came to walk into the real authorization
flow, open the loopback listener and sit out its own 300-second accept timeout
— while still returning `-1`, so it passed for the wrong reason.

`gmail_auth.c` therefore recognises three environment hooks:

| Variable | Effect |
|---|---|
| `GMAIL_TEST_NO_CLIENT_ID` | `get_client_id()`/`get_client_secret()` behave as in a build with no compiled-in credentials |
| `GMAIL_TEST_TOKEN` | `gmail_auth_refresh()` returns this value instead of contacting Google |
| `GMAIL_TEST_TOKEN_URL` | the token endpoint, so a local mock server can answer |

Set a hook with `setenv()` immediately before the call and `unsetenv()` it
immediately after: they are global state, and a leaked hook changes the
meaning of every later test in the same process.

A test that waits for a network peer must also assert *how long* the answer
took when the point of the branch is that it answers without waiting.  A
correct value that arrives after a five-minute timeout is not the same answer.

## Functional Tests (`tests/functional/`)

The mock IMAP server (`mock_imap_server.c`) listens on a loopback TCP port the
kernel picks (see `MOCK_IMAP_PORT_FILE`) and implements a minimal IMAP state
machine:

- Accepts multiple sequential connections (SEARCH uses one connection,
  each FETCH uses a separate connection).
- Always authenticates any credentials.
- Returns `* SEARCH 1` (one message exists).
- Returns a hardcoded test email body on FETCH.

`./tests/functional/run_package_dir.sh` is a separate script (it performs its own
release build, so it is not part of the coverage run): it checks that
`./manage.sh package deb` leaves exactly one run's packages in `build/packages/`
and an empty directory with a non-zero exit when packaging fails (a stub
`cpack` first on `PATH` provides the failure).

`./manage.sh headers [DIR]` is the source-header gate (EMAIL-29), the first gate
of `./manage.sh check`: it fails, naming the file, on any `.c`, `.h`, `.sh`,
`.cmake` or `CMakeLists.txt` without the licence, copyright, `Created:` date and
purpose lines. `./tests/functional/run_source_header.sh` tests the gate itself
against a scratch tree under `build/`.

`./manage.sh names [DIR]` is the private-names gate (EMAIL-40): it fails, naming
the file, when a tracked file contains the owner's private domain; the pattern is
built from parts so the script passes its own check.
`./tests/functional/run_private_names.sh` tests the gate in a scratch repository
under `build/`.

`./manage.sh timing [KEY-N]` and the timing log (EMAIL-76): every verb that does
work (`build`, `debug`, `test`, `coverage`, `valgrind`, `functional`, `pty`,
`integration`, `integration-local`, `headers`, `names`, `check`, `package`,
`install`) appends `<UTC time> <verb> <s>s <ok|fail>` to `build/timing.log`,
success or failure. `timing` prints `Gate: <total>s (check .., package ..,
install ..)` from the latest run of each, leaves out a part that never ran, and
prints `Gate: not measured` with a non-zero status when there is none; with a
key it comments the line on the ticket. `./tests/functional/run_timing.sh`
tests the log and the line against a log under `build/`.

`./tests/functional/run_package_policy.sh` checks the package itself (EMAIL-16):
`lintian` reports nothing, `Depends` is discovered (libc6 present, no
hand-written or exact versions), man pages and changelog are gzipped, the
copyright file is installed, every directory is 0755 although the build runs
under `umask 002`, and a `lintian` finding (a stub first on `PATH`) makes
`package deb` exit non-zero with `build/packages/` empty.

Run via `./tests/functional/run_functional.sh`. The script:
1. Compiles and starts the mock server in background.
2. Runs `email-cli` with a temporary config pointing at the port the mock got.
3. Checks 5 assertions on the output.
4. Kills the server on exit (via trap).

### Mock IMAP server options

`tests/functional/mock_imap_server.c` is configured through the environment.
Besides `MOCK_IMAP_PORT`, `MOCK_IMAP_SUBJECT`, `MOCK_IMAP_COUNT` and the
CONDSTORE/QRESYNC switches:

| Variable | Effect |
|---|---|
| `MOCK_IMAP_PASSWORD` | `LOGIN` must present exactly this password, or the server answers `NO [AUTHENTICATIONFAILED]` and closes |

Without it every `LOGIN` is accepted, which means a wrong password is
indistinguishable from a right one and the product's own handling of a refusal
cannot be tested at all.

## Integration Tests (`tests/integration/`)

Requires Docker. Runs a real Dovecot IMAP server with:
- Self-signed TLS certificate (openssl, `ssl_min_protocol = TLSv1.2`)
- Two seed emails pre-loaded into the mailbox

```bash
./manage.sh integration   # start Dovecot if needed, run test, assert output
./manage.sh imap-down     # stop container (volume preserved)
./manage.sh imap-clean    # stop + remove volume
```

## PTY-based TUI Tests (`libs/libptytest`, submodule)

`libptytest` is a reusable C library for automated testing of terminal programs.
It opens a pseudo-terminal, forks/execs the program under test, sends keystrokes,
and inspects a virtual screen buffer.

### Where the library comes from

`libs/libptytest` is a **git submodule** pinned to a release tag of the
libptytest repository (now v0.1.5), not a copy.  Clone with
`--recurse-submodules`, or run `git submodule update --init` in an existing
checkout.  It is built through `add_subdirectory()` (library only) and the PTY
suites link the static target `ptytest::ptytest_static`, so they carry no
run-time dependency on it.

The library's own test suite and release gates run in its repository
(`./manage.sh check` there), not in this one.  To move to a newer release:

```bash
git -C libs/libptytest fetch --tags
git -C libs/libptytest checkout v0.1.6      # the release tag
git add libs/libptytest && git commit
```

### Key API

```c
PtySession *s = pty_open(80, 24);
const char *argv[] = { "bin/email-cli", NULL };
pty_run(s, argv);
pty_wait_for(s, "message(s)", 2000);       /* wait for text, max 2s */
pty_row_contains(s, 23, "↑↓=step");       /* check last row */
pty_cell_attr(s, 23, 2) & PTY_ATTR_REVERSE /* check reverse video */
pty_send_key(s, PTY_KEY_ESC);
pty_close(s);
```

### VT100 subset

The parser handles cursor positioning (`CSI H`), screen/line erase
(`CSI 2J`, `CSI K`), SGR attributes (bold, dim, reverse), and 24-bit
colour sequences (parsed but not stored).  This covers the escape
sequences used by `email-cli`'s TUI.

### Assertion macros (`pty_assert.h`)

```c
ASSERT_ROW_CONTAINS(s, row, "text");
ASSERT_CELL_ATTR(s, row, col, PTY_ATTR_REVERSE);
ASSERT_SCREEN_CONTAINS(s, "text");
ASSERT_WAIT_FOR(s, "text", timeout_ms);
```

---

## Coverage Requirements

`./manage.sh coverage` measures **line coverage of product code only**
(`src/`, `libemail/src/`, `libwrite/src/`; never `tests/`, never libptytest or
other libraries) with one definition for every report, and fails the run
below the policy floors (policies/testing-model.md, *Coverage*):

| Figure | Measured from | Requirement |
|--------|---------------|-------------|
| Unit tests alone | fresh counters, `test-runner` only | at least 80% (gate) |
| Functional + PTY together | the black-box suites on the instrumented binaries | at least 80% (gate) |
| Everything combined | unit on top of the above | target above 90% (reported, not a gate) |

The published badges are the combined figure and the functional + PTY figure,
both line coverage over the same code.

| Scope | Expectation |
|-------|-------------|
| `libemail/src/core/` + `libemail/src/infrastructure/` combined | >90% line coverage |
| `libemail/src/domain/` | best-effort |
| `src/main.c` | best-effort (wiring code) |

Known uncoverable lines in `setup_wizard.c`: TTY manipulation branches
(`tcgetattr`/`tcsetattr` inside `hide && is_tty`) require a PTY to test.
These 6 lines are excluded from the >90% target by convention.

## CI and the library submodules

The workflows in `.github/workflows/` check out with `submodules: recursive`.
libtuikit and libptytest are public (GPL-3.0), so the default `GITHUB_TOKEN`
fetches them and no deploy key or secret is needed. Functional phase 101 fails
when a workflow's checkout step lacks the option.
