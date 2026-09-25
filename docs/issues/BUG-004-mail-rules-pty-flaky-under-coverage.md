# BUG-004 — mail-rules PTY suite is unstable under the coverage build

**Status:** FIXED
**Severity:** Medium — no product defect proven, but the suite cannot be
trusted to gate anything while it behaves this way
**Component:** `tests/functional/mock_imap_server.c`, the PTY suites, `manage.sh`
**Found:** while investigating the Coverage workflow failure of BUG-003

---

## Description

Run under `./manage.sh coverage`, the mail-rules PTY suite fails a varying
number of assertions between runs on an unchanged tree:

```
[FAIL] tests/pty/test_pty_mail_rules.c:277: from-glob flag: manifest entry found
[FAIL] tests/pty/test_pty_mail_rules.c:338: nonmatch rule: manifest entry found
[FAIL] tests/pty/test_pty_mail_rules.c:372: multi-rule: manifest entry found
[FAIL] tests/pty/test_pty_mail_rules.c:404: apply-rules retro: manifest entry found after initial sync
```

Two consecutive runs of the same binary produced four failures and then two,
so the failures are timing-dependent, not deterministic.

The passing count also differs sharply from the ordinary build: 92 assertions
under `./manage.sh pty` against 14-23 under the coverage build, which suggests
the suite stops early rather than simply failing individual checks.

## Why it went unnoticed

`manage.sh` ran each suite as

```sh
if ! (cd "$ABS_BUILD" && "$@" 2>/dev/null >/dev/null); then
    echo "  [warn] PTY suite '$label' reported failures (coverage run continues)"
fi
```

Tolerating failures during a coverage run is deliberate — the report must
still be produced — but discarding both streams left nothing to diagnose
from.  The warning has been present in CI logs since at least 2026-08-21
without anyone being able to say what failed.  `manage.sh` now captures each
suite's output to `build/pty-coverage-<label>.log` and prints the failure
lines, which is how the assertions above became visible.

## Not the cause of the Coverage workflow failure

That was a truncated `.gcda` from the input-line harness, fixed separately.
The coverage run tolerates PTY failures, so this suite never failed the
workflow — it only meant the mail-rules paths contributed unreliable coverage
data.  The two share a theme, though: both were a test process being treated
as disposable, and both hid their own evidence.

## Root cause

`tests/functional/mock_imap_server.c` set **`SO_REUSEPORT`** alongside
`SO_REUSEADDR` on its listening socket:

```c
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
```

`SO_REUSEPORT` exists so several processes can share one listening port, with
the kernel distributing incoming connections between them at random.  For a
test mock that quietly destroys isolation.

Port 9993 is the mock's own default and the port three PTY suites use.  Under
`./manage.sh coverage` the functional suite runs first and starts a mock on
9993 with `MOCK_IMAP_SUBJECT="AlphaAccountMsg"`; any instance left behind kept
listening.  The mail-rules suite then started its own mock, which bound
*successfully beside it*, and from then on roughly every other connection went
to the wrong server.  Hence a different subset of assertions failing each run,
and the giveaway once diagnostics were added:

```
DIAG: manifest row subj='AlphaAccountMsg (folded continuation)' flags='67'
```

— the suite asserting over another fixture's mail.

The two guards that should have caught it both failed: the mock's stderr went
to `/dev/null`, so `bind failed` was invisible, and the connect probe cannot
tell our server from a stranger's.

## Fix

1. `mock_imap_server.c` keeps `SO_REUSEADDR` (wanted, for TIME_WAIT) and drops
   `SO_REUSEPORT`.  A second bind on a busy port now fails loudly, which is
   the correct outcome — two suites must not share a port.
2. The suites that fork a mock now check the child is still alive after the
   startup delay, and report a port conflict instead of proceeding.  The
   mail-rules mock also keeps its stderr, so `bind failed` is visible.
3. `manage.sh coverage` clears stale mock servers before the PTY run, as the
   `pty` target already did — on this path the functional run immediately
   precedes the PTY run, so it needs it most.
4. `sync_finish()` waits for the sync process to exit (new
   `pty_wait_exit()` in libptytest) before reading the manifest, replacing
   fixed settle delays with a real condition.

## Verified

- Port free: 28/28 assertions pass, where the suite previously passed 21-24
  with a varying set of failures.
- Port deliberately taken by a foreign mock: the suite now prints
  `bind failed: Address already in use` and `mock server exited immediately
  … Refusing to test against it` rather than silently testing against it.
