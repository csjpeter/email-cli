#!/bin/bash
# manage.sh keeps its promises about verdicts (EMAIL-32): a failed copy fails
# the build, `check` reports each gate and fails when one fails, and nothing in
# the script swallows an error.  Runs a copy of manage.sh in a scratch tree
# under build/ with a stub cmake first on PATH, so it needs no real build.
set -u
cd "$(dirname "$0")/../.." || exit 1
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
out=$(cd "$WORK/tree" && PATH="$WORK/stubs:$PATH" ./manage.sh build 2>&1); rc=$?
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
echo "$out" | grep -q "PASS"                  && bad "2.3 a failing gate is shown as PASS"    || ok "2.3 no gate shown as PASS"

# 3. the script itself suppresses nothing.
SUPPRESS=$(grep -c -E '2>/dev/null|\|\| true|set \+e' "$ROOT/manage.sh")
[ "$(wc -l < "$ROOT/manage.sh")" -gt 100 ]   && ok "3.1 manage.sh has content"               || bad "3.1 manage.sh is short"
[ "$SUPPRESS" -eq 0 ]                         && ok "3.2 no suppressed error output or verdict" || bad "3.2 $SUPPRESS suppressing line(s)"
"$ROOT/manage.sh" help | grep -q '^  check '  && ok "3.3 help lists check"                    || bad "3.3 help does not list check"

case "$WORK" in
    "$ROOT"/build/manage-sh-test-*) ;;
    *) echo "ERROR: refusing unsafe path '$WORK'" >&2; exit 1 ;;
esac
rm -rf "$ROOT/build/manage-sh-test-${WORK##*/manage-sh-test-}"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
