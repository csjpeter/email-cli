#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-12
# Tests that the declared-tests gate fails on a missing or an undeclared test.

# EMAIL-19: `manage.sh declared DECL TESTS_DIR` passes when every project_test()
# line has an executable in TESTS_DIR/<category>/bin and every executable there
# is declared; it fails, naming the test, otherwise.  Works on a scratch tree
# under build/.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
WORK="$ROOT/build/declared-test-$$"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

mkdir -p "$WORK/tests/unit/bin" "$WORK/tests/functional/bin" || exit 1
D="$WORK/CMakeLists.txt"
cat > "$D" <<'EOF'
project_test(unit        test_parser   SOURCES test_parser.c)
project_test(functional  list_and_show SCRIPT   list_and_show.sh)
EOF
run() { out=$("$ROOT/manage.sh" declared "$D" "$WORK/tests" 2>&1); rc=$?; }

: > "$WORK/tests/unit/bin/test_parser"
: > "$WORK/tests/functional/bin/list_and_show"
run
[ "$rc" -eq 0 ] && ok "1.1 every declared test built, none extra: passes" || bad "1.1 (rc=$rc: $out)"

rm "$WORK/tests/functional/bin/list_and_show"
run
[ "$rc" -ne 0 ] && echo "$out" | grep -q "functional/list_and_show is declared" \
    && ok "2.1 a declared test with no executable fails and is named" || bad "2.1 (rc=$rc: $out)"

: > "$WORK/tests/functional/bin/list_and_show"
: > "$WORK/tests/unit/bin/test_stray"
run
[ "$rc" -ne 0 ] && echo "$out" | grep -q "unit/bin/test_stray is not declared" \
    && ok "3.1 an undeclared executable fails and is named" || bad "3.1 (rc=$rc: $out)"

rm "$WORK/tests/unit/bin/test_stray"
: > "$D"
run
[ "$rc" -ne 0 ] && echo "$out" | grep -q "no project_test" \
    && ok "4.1 a file with no declaration fails" || bad "4.1 (rc=$rc: $out)"

# The project's own declaration is read the same way.
n=$(sed -n 's/^project_test(\([A-Za-z0-9_-]*\)[[:space:]]\{1,\}\([A-Za-z0-9_.-]*\).*/\1 \2/p' "$ROOT/tests/CMakeLists.txt" | wc -l)
[ "$n" -ge 19 ] && ok "5.1 tests/CMakeLists.txt declares $n tests" || bad "5.1 only $n declarations read"

case "$WORK" in
    "$ROOT"/build/declared-test-*) ;;
    *) echo "ERROR: refusing unsafe path '$WORK'" >&2; exit 1 ;;
esac
rm -rf "$ROOT/build/declared-test-${WORK##*/declared-test-}"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
