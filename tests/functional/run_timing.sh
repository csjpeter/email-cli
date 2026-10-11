#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-11
# Tests the timing log and the timing verb of manage.sh.

# EMAIL-76: a verb that does work appends "<UTC time> <verb> <s>s <ok|fail>" to
# the timing log whatever its outcome, and `manage.sh timing` prints the Gate
# line from the latest run of check, package and install, or "Gate: not
# measured" with a non-zero status.  Everything lives under build/.
set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT="$PWD"
WORK="$ROOT/build/timing-test-$$"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

mkdir -p "$WORK/tree" "$WORK/stubs" || exit 1
T="$WORK/tree"
export MANAGE_TIMING_LOG="$WORK/timing.log"
git -C "$T" init -q || exit 1

# 1. verbs log themselves, on success and on failure
"$ROOT/manage.sh" headers "$T" > "$WORK/out" 2>&1; rc=$?
[ "$rc" -eq 0 ] && grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}Z headers [0-9]+s ok$' "$MANAGE_TIMING_LOG" \
    && ok "1.1 a passing verb appends an ok line" || bad "1.1 (rc=$rc: $(cat "$MANAGE_TIMING_LOG"))"

printf 'int x;\n' > "$T/a.c"; git -C "$T" add a.c
"$ROOT/manage.sh" headers "$T" > "$WORK/out" 2>&1; rc=$?
[ "$rc" -ne 0 ] && tail -n 1 "$MANAGE_TIMING_LOG" | grep -Eq ' headers [0-9]+s fail$' \
    && ok "1.2 a failing verb appends a fail line and keeps its status" || bad "1.2 (rc=$rc)"

# 2. the timing verb with no log
rm -f "$MANAGE_TIMING_LOG"
out=$("$ROOT/manage.sh" timing 2>&1); rc=$?
[ "$rc" -ne 0 ] && [ "$out" = "Gate: not measured" ] \
    && ok "2.1 no log: 'not measured' and non-zero" || bad "2.1 (rc=$rc: $out)"

# 3. only parts that ran are summed, latest run of each wins
printf '2026-10-11T10:00:00Z check 100s ok\n2026-10-11T10:05:00Z test 40s ok\n2026-10-11T10:10:00Z check 120s ok\n2026-10-11T10:12:00Z package 30s ok\n' > "$MANAGE_TIMING_LOG"
out=$("$ROOT/manage.sh" timing 2>&1); rc=$?
[ "$rc" -eq 0 ] && [ "$out" = "Gate: 150s (check 120s, package 30s)" ] \
    && ok "3.1 latest of each part, missing install left out" || bad "3.1 (rc=$rc: $out)"

printf '2026-10-11T10:20:00Z install 5s ok\n' >> "$MANAGE_TIMING_LOG"
out=$("$ROOT/manage.sh" timing 2>&1)
[ "$out" = "Gate: 155s (check 120s, package 30s, install 5s)" ] \
    && ok "3.2 all three parts" || bad "3.2 ($out)"

# 4. with a key the same line is commented through issue-cli
printf '#!/bin/sh\nprintf "%%s\\n" "$*" > "%s/issue-args"\n' "$WORK" > "$WORK/stubs/issue-cli"
chmod +x "$WORK/stubs/issue-cli"
PATH="$WORK/stubs:$PATH" "$ROOT/manage.sh" timing KEY-1 > "$WORK/out" 2>&1; rc=$?
[ "$rc" -eq 0 ] && [ "$(cat "$WORK/issue-args")" = "issue comment KEY-1 Gate: 155s (check 120s, package 30s, install 5s)" ] \
    && ok "4.1 timing KEY posts the line as a comment" || bad "4.1 (rc=$rc: $(cat "$WORK/issue-args"))"

case "$WORK" in
    "$ROOT"/build/timing-test-*) ;;
    *) echo "ERROR: refusing unsafe path '$WORK'" >&2; exit 1 ;;
esac
rm -rf "$ROOT/build/timing-test-${WORK##*/timing-test-}"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
