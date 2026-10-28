#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-26
# Tests the result files and the uniform verb rules of manage.sh.

# EMAIL-233 (policies/manage-sh-model.md): test, functional, pty, integration,
# valgrind and coverage write build/result/<verb>.result, deleted at the start,
# "running" while the suites run, final on every way out; an unknown verb exits
# 2; nothing is skipped; every suite runs and then the verb fails.  Runs a copy
# of manage.sh in a scratch tree under build/ with a stub cmake and stub suites,
# so it needs no real build.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
WORK="$ROOT/build/result-test-$$"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

T="$WORK/tree"
mkdir -p "$WORK/stubs" "$T/build/tests/unit/bin" "$T/build/tests/functional/bin" \
         "$T/tests/integration" "$T/bin" || exit 1
cp "$ROOT/manage.sh" "$T/manage.sh" || { bad "0.1 manage.sh copy"; exit 1; }
# A cmake that "builds" the five programs and nothing else.
cat > "$WORK/stubs/cmake" <<'STUB'
#!/bin/sh
[ -z "${STUB_CMAKE_FAIL:-}" ] || { echo "stub cmake: failing on request" >&2; exit 1; }
d=$(pwd)
case "$d" in */build) ;; *) d="$d/build" ;; esac
mkdir -p "$d"
for b in email-cli email-cli-ro email-sync email-tui email-import-rules; do : > "$d/$b"; done
exit 0
STUB
chmod 755 "$WORK/stubs/cmake"

# A unit runner whose behaviour the test chooses: STUB_RUNS tests, STUB_FAILS of
# them failed, exit STUB_EXIT.  It copies the result file as it sees it.
cat > "$T/build/tests/unit/bin/test-runner" <<'STUB'
#!/bin/sh
if [ -f result/test.result ]; then cp result/test.result seen-while-running.txt; else cp result/valgrind.result seen-while-running.txt; fi
echo "--- Test Results ---"
echo "Tests Run:    ${STUB_RUNS:-3}"
echo "Tests Failed: ${STUB_FAILS:-0}"
exit "${STUB_EXIT:-0}"
STUB
chmod 755 "$T/build/tests/unit/bin/test-runner"
cat > "$WORK/stubs/valgrind" <<'STUB'
#!/bin/sh
# drop the options, run the program
while [ $# -gt 0 ]; do case "$1" in --*) shift ;; *) break ;; esac; done
exec "$@"
STUB
chmod 755 "$WORK/stubs/valgrind"

run() {  # run <verb> [args]: output in $out, status in $rc
    out=$(cd "$T" && WORKSPACE_LIBS=system PATH="$WORK/stubs:$PATH" ./manage.sh "$@" 2>&1); rc=$?
}
RES="$T/build/result"
field() { sed -n "s/^$2: //p" "$RES/$1.result" | head -n 1; }

# 1. test, passing: the file is there, in the order of the format, and agrees.
run test
[ "$rc" -eq 0 ] && ok "1.1 test passes with a passing runner" || bad "1.1 rc=$rc: $out"
[ -f "$RES/test.result" ] && ok "1.2 build/result/test.result exists" || bad "1.2 no result file"
keys=$(sed 's/:.*//' "$RES/test.result" | head -n 11 | tr '\n' ' ')
[ "$keys" = "format verb commit started seconds exit status suites assertions failed suite " ] \
    && ok "1.3 the keys come in the order of the format" || bad "1.3 keys: $keys"
[ "$(field test format)" = "manage-result/1" ] && [ "$(field test verb)" = test ] \
    && [ "$(field test status)" = pass ] && [ "$(field test exit)" = 0 ] \
    && [ "$(field test suites)" = 1 ] && [ "$(field test assertions)" = 3 ] && [ "$(field test failed)" = 0 ] \
    && ok "1.4 a passing run: status pass, exit 0, 1 suite, 3 assertions, 0 failed" \
    || bad "1.4 $(cat "$RES/test.result")"
sed -n 's/^suite: //p' "$RES/test.result" | grep -Eq '^unit pass 3 0 [0-9]+$' \
    && ok "1.5 the suite line is: name pass assertions failed seconds" || bad "1.5 $(cat "$RES/test.result")"
grep -Eq '^started: [0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}Z$' "$RES/test.result" \
    && grep -Eq '^seconds: [0-9]+$' "$RES/test.result" && grep -Eq '^commit: [^ ]+$' "$RES/test.result" \
    && ok "1.6 started, seconds and commit have their shapes" || bad "1.6 $(cat "$RES/test.result")"
grep -q '^status: running' "$T/build/seen-while-running.txt" \
    && ok "1.7 the file said 'status: running' while the suite ran" || bad "1.7 saw: $(cat "$T/build/seen-while-running.txt")"
[ -f "$T/build/result/log/test-unit.log" ] && ok "1.8 the suite's output is kept in build/result/log" || bad "1.8 no log"

# 2. test, failing suite: exit 1, status fail, the counts are the runner's.
STUB_RUNS=5 STUB_FAILS=2 STUB_EXIT=1 run test
[ "$rc" -eq 1 ] && ok "2.1 a failing suite fails the verb" || bad "2.1 rc=$rc"
[ "$(field test status)" = fail ] && [ "$(field test exit)" = 1 ] && [ "$(field test failed)" = 2 ] \
    && [ "$(field test assertions)" = 5 ] && ok "2.2 the file says fail, exit 1, 2 of 5 failed" || bad "2.2 $(cat "$RES/test.result")"
sed -n 's/^suite: //p' "$RES/test.result" | grep -Eq '^unit fail 5 2 ' && ok "2.3 the suite is listed as fail" || bad "2.3"

# 3. a suite that crashes with no count, a suite with no assertions, and exit 77.
STUB_RUNS=0 STUB_EXIT=139 run test
[ "$rc" -ne 0 ] && sed -n 's/^suite: //p' "$RES/test.result" | grep -Eq '^unit crash 0 0 ' && [ "$(field test status)" = fail ] \
    && ok "3.1 a crash without a count is a crash suite and a failed verb" || bad "3.1 rc=$rc $(cat "$RES/test.result")"
STUB_RUNS=0 STUB_EXIT=0 run test
[ "$rc" -ne 0 ] && ! sed -n 's/^suite: //p' "$RES/test.result" | grep -q '^unit pass' && [ "$(field test status)" = fail ] \
    && ok "3.2 a suite that counted no assertion does not pass (the tests exist)" || bad "3.2 rc=$rc $(cat "$RES/test.result")"
STUB_RUNS=4 STUB_EXIT=77 run test
[ "$rc" -ne 0 ] && [ "$(field test status)" = fail ] && ok "3.3 a runner's exit 77 is a failure, not a skip" || bad "3.3 rc=$rc $(cat "$RES/test.result")"

# 4. a stale pass is deleted at the start, and a build failure leaves status error.
run test
printf 'format: manage-result/1\nverb: test\nstatus: pass\nexit: 0\n' > "$RES/test.result"
STUB_CMAKE_FAIL=1 run test
[ "$rc" -ne 0 ] && ok "4.1 a failing build fails the verb" || bad "4.1 rc=$rc"
[ "$(field test status)" = error ] && [ "$(field test exit)" = "$rc" ] && [ "$(field test suites)" = 0 ] \
    && ok "4.2 the stale pass is gone: status error, the verb's exit code, no suite" || bad "4.2 $(cat "$RES/test.result")"

# 5. valgrind: the unit suite under Valgrind, same suite name as test.
run valgrind
[ "$rc" -eq 0 ] && [ "$(field valgrind status)" = pass ] \
    && sed -n 's/^suite: //p' "$RES/valgrind.result" | grep -Eq '^unit pass 3 0 ' \
    && ok "5.1 valgrind writes its result with the suite 'unit'" || bad "5.1 rc=$rc $out $(cat "$RES/valgrind.result")"

# 6. functional: every suite runs, then the verb fails; names are single words.
cat > "$T/build/tests/functional/bin/run_functional" <<'STUB'
#!/bin/sh
echo "Passed: 7 / 7"
STUB
cat > "$T/build/tests/functional/bin/run_alpha" <<'STUB'
#!/bin/sh
echo "  [PASS] a"; echo "  [FAIL] b"; exit 1
STUB
cat > "$T/build/tests/functional/bin/run_beta" <<'STUB'
#!/bin/sh
echo "  [PASS] c"; echo "  [PASS] d"
STUB
chmod 755 "$T/build/tests/functional/bin/"*
run functional
[ "$rc" -eq 1 ] && ok "6.1 a failing script fails the verb" || bad "6.1 rc=$rc: $out"
[ "$(field functional suites)" = 3 ] && [ "$(field functional status)" = fail ] \
    && ok "6.2 every suite ran: the later script ran after the failed one" || bad "6.2 $(cat "$RES/functional.result")"
sed -n 's/^suite: //p' "$RES/functional.result" > "$WORK/suites.txt"
grep -Eq '^run_functional pass 7 0 ' "$WORK/suites.txt" && grep -Eq '^run_alpha fail 2 1 ' "$WORK/suites.txt" \
    && grep -Eq '^run_beta pass 2 0 ' "$WORK/suites.txt" && ok "6.3 counts: Passed: P / T, and [PASS]/[FAIL] lines" || bad "6.3 $(cat "$WORK/suites.txt")"
[ "$(field functional assertions)" = 11 ] && [ "$(field functional failed)" = 1 ] \
    && ok "6.4 the totals are the sums of the suites" || bad "6.4 $(cat "$RES/functional.result")"
awk '{print $1}' "$WORK/suites.txt" | sort | uniq -d | grep -q . && bad "6.5 duplicate suite names" || ok "6.5 suite names are unique"

# 7. pty: an unknown suite is an error; the file says error and the exit code.
run pty nosuch-suite
[ "$rc" -ne 0 ] && [ "$(field pty status)" = error ] && echo "$out" | grep -q "no PTY suite is called 'nosuch-suite'" \
    && ok "7.1 an unknown PTY suite fails with status error and says why" || bad "7.1 rc=$rc $out"

# 8. integration: both scripts of tests/integration run, none is skipped.
printf '#!/bin/sh\necho "Passed: 4 / 4"\n' > "$T/tests/integration/run_integration.sh"
printf '#!/bin/sh\necho "no dovecot" >&2\nexit 1\n' > "$T/tests/integration/run_local_dovecot.sh"
chmod 755 "$T/tests/integration/"*.sh
run integration
[ "$rc" -eq 1 ] && [ "$(field integration suites)" = 2 ] && [ "$(field integration status)" = fail ] \
    && sed -n 's/^suite: //p' "$RES/integration.result" | grep -Eq '^run_local_dovecot crash ' \
    && ok "8.1 both scripts ran; the failing one fails the verb" || bad "8.1 rc=$rc $(cat "$RES/integration.result")"

# 9. unknown verbs: usage on stderr, exit 2; help exits 0; the removed names are unknown.
for v in nosuch e2e valgrind-all integration-local; do
    (cd "$T" && ./manage.sh "$v" > "$WORK/o.txt" 2> "$WORK/e.txt"); rc=$?
    [ "$rc" -eq 2 ] && [ ! -s "$WORK/o.txt" ] && grep -q '^Usage:' "$WORK/e.txt" \
        && ok "9.1 '$v' prints the usage to stderr and exits 2" || bad "9.1 '$v': rc=$rc"
done
(cd "$T" && ./manage.sh help > "$WORK/o.txt" 2> "$WORK/e.txt"); rc=$?
[ "$rc" -eq 0 ] && grep -q '^Usage:' "$WORK/o.txt" && [ ! -s "$WORK/e.txt" ] && ok "9.2 help exits 0 on stdout" || bad "9.2 rc=$rc"
for v in e2e valgrind-all integration-local; do
    grep -q "^  $v " "$WORK/o.txt" && bad "9.3 help still lists $v" || ok "9.3 help does not list $v"
done

# 10. the script keeps no skip and no discarded verdict.
grep -n 'NOT RUN' "$ROOT/manage.sh" > /dev/null && bad "10.1 manage.sh still has a NOT RUN skip" || ok "10.1 no NOT RUN skip in manage.sh"
grep -Eq '\|\| (true|:)( |$)' "$ROOT/manage.sh" && bad "10.2 a discarded verdict" || ok "10.2 no '|| true' in manage.sh"

case "$WORK" in
    "$ROOT"/build/result-test-*) ;;
    *) echo "ERROR: refusing unsafe path '$WORK'" >&2; exit 1 ;;
esac
rm -rf "$ROOT/build/result-test-${WORK##*/result-test-}"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
