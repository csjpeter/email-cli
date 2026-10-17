#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-11
# Tests the timing log and the timing verb of manage.sh.

# EMAIL-76: a verb that does work appends "<UTC time> <verb> <s>s <ok|fail>" to
# the timing log whatever its outcome, and `manage.sh timing` prints the Gate
# line from the latest run of check, package and install, or "Gate: not
# measured" with a non-zero status.  Everything lives under build/.
# EMAIL-121: each line also carries the commit it ran on ("+" when the tree was
# dirty), and `timing KEY` posts a measured record once but a newer one again.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
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
[ "$rc" -eq 0 ] && grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}Z headers [0-9]+s ok [0-9a-f]+\+?$' "$MANAGE_TIMING_LOG" \
    && ok "1.1 a passing verb appends an ok line" || bad "1.1 (rc=$rc: $(cat "$MANAGE_TIMING_LOG"))"

printf 'int x;\n' > "$T/a.c"; git -C "$T" add a.c
"$ROOT/manage.sh" headers "$T" > "$WORK/out" 2>&1; rc=$?
[ "$rc" -ne 0 ] && tail -n 1 "$MANAGE_TIMING_LOG" | grep -Eq ' headers [0-9]+s fail [0-9a-f]+\+?$' \
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

# 5. a record is posted once; a newer record is posted again, even with the
# same text (EMAIL-121)
: > "$WORK/issue-calls"
printf '#!/bin/sh\nprintf "%%s\\n" "$*" >> "%s/issue-calls"\n' "$WORK" > "$WORK/stubs/issue-cli"
chmod +x "$WORK/stubs/issue-cli"
printf '2026-10-11T10:00:00Z check 100s ok aaaaaaa\n2026-10-11T10:12:00Z package 30s ok aaaaaaa\n' > "$MANAGE_TIMING_LOG"
rm -f "$WORK/timing.posted"
PATH="$WORK/stubs:$PATH" "$ROOT/manage.sh" timing KEY-5 > "$WORK/out" 2>&1
[ "$(wc -l < "$WORK/issue-calls")" -eq 1 ] && ok "5.1 the first report is posted" || bad "5.1 ($(cat "$WORK/issue-calls"))"
PATH="$WORK/stubs:$PATH" "$ROOT/manage.sh" timing KEY-5 > "$WORK/out" 2>&1; rc=$?
[ "$rc" -eq 0 ] && [ "$(wc -l < "$WORK/issue-calls")" -eq 1 ] && grep -q "already posted" "$WORK/out" \
    && ok "5.2 the same record is not posted again (and says so, exit 0)" || bad "5.2 (rc=$rc: $(cat "$WORK/out"))"
printf '2026-10-11T11:00:00Z check 100s ok bbbbbbb+\n2026-10-11T11:12:00Z package 30s ok bbbbbbb+\n' >> "$MANAGE_TIMING_LOG"
PATH="$WORK/stubs:$PATH" "$ROOT/manage.sh" timing KEY-5 > "$WORK/out" 2>&1
[ "$(wc -l < "$WORK/issue-calls")" -eq 2 ] && ok "5.3 a newer record with an identical line is posted" || bad "5.3 ($(cat "$WORK/issue-calls"))"
PATH="$WORK/stubs:$PATH" "$ROOT/manage.sh" timing KEY-6 > "$WORK/out" 2>&1
[ "$(wc -l < "$WORK/issue-calls")" -eq 3 ] && ok "5.4 another key has its own record" || bad "5.4 ($(cat "$WORK/issue-calls"))"
printf '2026-10-11T12:00:00Z install 5s ok bbbbbbb+\n' >> "$MANAGE_TIMING_LOG"
PATH="$WORK/stubs:$PATH" "$ROOT/manage.sh" timing KEY-5 > "$WORK/out" 2>&1
[ "$(wc -l < "$WORK/issue-calls")" -eq 4 ] && grep -q "install 5s" "$WORK/issue-calls" && ok "5.5 the install part makes a new record" || bad "5.5 ($(cat "$WORK/issue-calls"))"

case "$WORK" in
    "$ROOT"/build/timing-test-*) ;;
    *) echo "ERROR: refusing unsafe path '$WORK'" >&2; exit 1 ;;
esac
rm -rf "$ROOT/build/timing-test-${WORK##*/timing-test-}"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
