# BUG-004 — mail-rules PTY suite is unstable under the coverage build

**Status:** OPEN
**Severity:** Medium — no product defect proven, but the suite cannot be
trusted to gate anything while it behaves this way
**Component:** `tests/pty/test_pty_mail_rules.c`
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
The coverage run tolerates PTY failures, so this suite does not fail the
workflow — it only means the mail-rules paths contribute unreliable coverage
data.

## Next steps

1. Establish whether the product misbehaves without ASAN, or whether the test
   simply races the sync it waits on — the assertions all wait for a manifest
   entry to appear after a rule is applied.
2. If it is a race, give the wait a real condition instead of a fixed settle
   time.
3. `./manage.sh pty` (the gate CI enforces) passes all 92, so this is not
   currently blocking; it should not stay open on that basis.
