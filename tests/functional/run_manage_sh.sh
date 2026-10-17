#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-10
# Tests that manage.sh reports honest verdicts.

# manage.sh keeps its promises about verdicts (EMAIL-32): a failed copy fails
# the build, `check` reports each gate and fails when one fails, and nothing in
# the script swallows an error.  Runs a copy of manage.sh in a scratch tree
# under build/ with a stub cmake first on PATH, so it needs no real build.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
WORK="$ROOT/build/manage-sh-test-$$"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

mkdir -p "$WORK/stubs" "$WORK/tree/build" || exit 1
cp "$ROOT/manage.sh" "$WORK/tree/manage.sh" || { bad "0.1 manage.sh copy"; exit 1; }
# A cmake that "succeeds" and builds nothing: the build directory stays empty.
printf '#!/bin/sh\nexit 0\n' > "$WORK/stubs/cmake"
chmod 755 "$WORK/stubs/cmake"

# 1. build: the binaries were not produced, so the copy must fail the build.
out=$(cd "$WORK/tree" && WORKSPACE_LIBS=system PATH="$WORK/stubs:$PATH" ./manage.sh build 2>&1); rc=$?
[ "$rc" -ne 0 ]                              && ok "1.1 build exits non-zero without binaries" || bad "1.1 build exited 0"
echo "$out" | grep -q "cannot stat"            && ok "1.2 the cp error is shown"               || bad "1.2 no cp error in: $(echo "$out" | tail -2)"
echo "$out" | grep -q "Build complete"         && bad "1.3 claims 'Build complete'"            || ok "1.3 does not claim 'Build complete'"
[ -n "$out" ]                                || bad "1.4 output is empty"

# 2. check: every gate is named, a failing gate is FAIL, the verb exits 1.
out=$(cd "$WORK/tree" && PATH="$WORK/stubs:$PATH" ./manage.sh check 2>&1); rc=$?
[ "$rc" -ne 0 ]                              && ok "2.1 check exits non-zero when gates fail"  || bad "2.1 check exited 0"
for g in test functional pty valgrind; do
    echo "$out" | grep -Eq "^  $g +FAIL$"    && ok "2.2 summary shows $g FAIL"               || bad "2.2 no '$g FAIL' line"
done
echo "$out" | grep -Eq "^  (test|functional|pty|valgrind) +PASS$" && bad "2.3 a failing gate is shown as PASS" || ok "2.3 no failing gate shown as PASS"
echo "$out" | grep -Eq "^  headers +(PASS|FAIL)$" && ok "2.4 summary names the headers gate"   || bad "2.4 no headers gate in the summary"

# 3. the script itself suppresses nothing.
SUPPRESS=$(grep -c -E '2>/dev/null|\|\| true|set \+e' "$ROOT/manage.sh")
[ "$(wc -l < "$ROOT/manage.sh")" -gt 100 ]   && ok "3.1 manage.sh has content"               || bad "3.1 manage.sh is short"
[ "$SUPPRESS" -eq 0 ]                         && ok "3.2 no suppressed error output or verdict" || bad "3.2 $SUPPRESS suppressing line(s)"
"$ROOT/manage.sh" help | grep -q '^  check '  && ok "3.3 help lists check"                    || bad "3.3 help does not list check"

# 4. coverage: an invalid .gcda is named, removed and fails the check (EMAIL-119);
# valid ones are left alone; the coverage run gives the PTY children time to exit.
G="$WORK/gcda"
mkdir -p "$G/sub" || exit 1
: > "$G/sub/empty.c.gcda"
printf 'adcgvalid-looking' > "$G/sub/good.c.gcda"
printf 'xy' > "$G/sub/short.c.gcda"
out=$("$ROOT/tests/functional/gcda_check.sh" "$G" 2>&1); rc=$?
[ "$rc" -eq 1 ]                                   && ok "4.1 invalid .gcda files fail the check"    || bad "4.1 exit $rc"
echo "$out" | grep -q "empty.c.gcda"              && ok "4.2 the empty file is named"                || bad "4.2 no name in: $out"
echo "$out" | grep -q "short.c.gcda"              && ok "4.3 the truncated file is named"            || bad "4.3 no name in: $out"
echo "$out" | grep -q "good.c.gcda"               && bad "4.4 a valid file is reported"              || ok "4.4 the valid file is not reported"
[ ! -e "$G/sub/empty.c.gcda" ] && [ -e "$G/sub/good.c.gcda" ] && ok "4.5 bad files removed, good one kept" || bad "4.5 file state wrong"
out=$("$ROOT/tests/functional/gcda_check.sh" "$G" 2>&1); rc=$?
[ "$rc" -eq 0 ]                                   && ok "4.6 a clean tree passes"                    || bad "4.6 exit $rc: $out"
grep -q 'PTYTEST_CLOSE_GRACE_MS' "$ROOT/manage.sh" && ok "4.7 the coverage run sets the PTY close grace" || bad "4.7 grace not set"

case "$WORK" in
    "$ROOT"/build/manage-sh-test-*) ;;
    *) echo "ERROR: refusing unsafe path '$WORK'" >&2; exit 1 ;;
esac
rm -rf "$ROOT/build/manage-sh-test-${WORK##*/manage-sh-test-}"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
