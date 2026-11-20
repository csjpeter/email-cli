#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-11-20
# Tests the work queue of manage.sh (EMAIL-319).

# The queue runs the items on several workers, keeps each item's output whole,
# serialises the items of one resource mark, and gives the verdicts of a serial run.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
WORK="$ROOT/build/queue-test-$$"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }
mkdir -p "$WORK/result/log" || exit 1

# Load only the functions under test from manage.sh.
BUILD_DIR="$WORK"; RESULT_DIR="$WORK/result"; R_VERB=qt; R_PREFIX=""
R_SUITES=(); R_BAD=0
result_write() { :; }
# shellcheck disable=SC1090
source <(sed -n '/^count_assertions() {/,/^}/p' "$ROOT/manage.sh")
# shellcheck disable=SC1090
source <(sed -n '/^# suite_verdict RC LOG/,/^# The suites of the verbs, defined once/p' "$ROOT/manage.sh")
QUEUE_WEIGHTS="$WORK/weights"

# an item: prints five lines slowly, then a Tests Run summary
cat > "$WORK/item.sh" <<'EOS'
#!/bin/bash
tag="$1"; res="${2:-0}"
for i in 1 2 3 4 5; do echo "$tag line $i"; sleep 0.1; done
printf 'Tests Run: 5\nTests Failed: %s\n' "$res"
[ "$res" -eq 0 ]
EOS
chmod +x "$WORK/item.sh"

# 1. five items of 0.5 s on 5 workers finish in well under the serial 2.5 s
export MANAGE_JOBS=5
queue_begin
for n in a b c d e; do queue_add "item-$n" - "$WORK" "$WORK/item.sh" "$n"; done
t0=$(date +%s.%N)
queue_run > "$WORK/out1" 2>&1; rc=$?
t1=$(date +%s.%N)
[ "$rc" -eq 0 ] && [ "${#R_SUITES[@]}" -eq 5 ] && ok "1.1 five items pass, five suites recorded" || bad "1.1 (rc=$rc, ${#R_SUITES[@]} suites)"
awk -v a="$t0" -v b="$t1" 'BEGIN { exit !(b - a < 1.8) }' && ok "1.2 the items ran in parallel" || bad "1.2 wall time too long ($t0 .. $t1)"

# 2. the output of an item is whole: its five lines follow each other
whole=1
for n in a b c d e; do
    first=$(grep -n "^$n line 1" "$WORK/out1" | cut -d: -f1)
    last=$(grep -n "^$n line 5" "$WORK/out1" | cut -d: -f1)
    if [ -z "$first" ] || [ -z "$last" ] || [ $((last - first)) -ne 4 ]; then whole=0; fi
done
[ "$whole" -eq 1 ] && ok "2.1 no output of two items mixes" || bad "2.1 output mixed"
[ -s "$RESULT_DIR/log/qt-item-a.log" ] && ok "2.2 each item has a log of its own" || bad "2.2 no item log"

# 3. a failing item fails the step, the others keep their verdicts
R_SUITES=(); R_BAD=0
queue_begin
queue_add ok-1 - "$WORK" "$WORK/item.sh" x
queue_add bad-1 - "$WORK" "$WORK/item.sh" y 1
queue_add ok-2 - "$WORK" "$WORK/item.sh" z
queue_run > "$WORK/out3" 2>&1; rc=$?
if [ "$rc" -ne 0 ] && [ "$R_BAD" -eq 1 ] && printf '%s\n' "${R_SUITES[@]}" | grep -q '^bad-1 fail ' \
    && [ "$(printf '%s\n' "${R_SUITES[@]}" | grep -c ' pass ')" -eq 2 ]; then
    ok "3.1 one failing item: step fails, the others pass"
else bad "3.1 (rc=$rc bad=$R_BAD: ${R_SUITES[*]})"; fi
[ "${R_SUITES[0]%% *}" = ok-1 ] && [ "${R_SUITES[2]%% *}" = ok-2 ] && ok "3.2 verdicts are in registration order" || bad "3.2"

# 4. items with one resource mark never overlap
cat > "$WORK/res.sh" <<'EOS'
#!/bin/bash
d="$1"
mkdir "$d/in" || { echo "OVERLAP"; exit 1; }
sleep 0.3
rmdir "$d/in"
printf 'Tests Run: 1\nTests Failed: 0\n'
EOS
chmod +x "$WORK/res.sh"
R_SUITES=(); R_BAD=0
queue_begin
for n in 1 2 3 4; do queue_add "res-$n" shared "$WORK" "$WORK/res.sh" "$WORK"; done
queue_run > "$WORK/out4" 2>&1; rc=$?
[ "$rc" -eq 0 ] && ! grep -q OVERLAP "$WORK/out4" && ok "4.1 items of one resource ran one at a time" || bad "4.1 (rc=$rc)"

# 5. one worker is the serial run: same names and verdicts
export MANAGE_JOBS=1
R_SUITES=(); R_BAD=0
queue_begin
queue_add ok-1 - "$WORK" "$WORK/item.sh" x
queue_add bad-1 - "$WORK" "$WORK/item.sh" y 1
queue_add ok-2 - "$WORK" "$WORK/item.sh" z
queue_run > "$WORK/out5" 2>&1
got=$(printf '%s\n' "${R_SUITES[@]}" | awk '{ print $1, $2, $3, $4 }')
want=$(printf 'ok-1 pass 5 0\nbad-1 fail 5 1\nok-2 pass 5 0')
[ "$got" = "$want" ] && ok "5.1 one worker gives the serial names and verdicts" || bad "5.1: $got"

# Our own files, by literal name.
( cd "$WORK" && rm -f -- ./item.sh ./res.sh ./out1 ./out3 ./out4 ./out5 ./weights ./result/log/qt-*.log \
    && rmdir result/log result ) || echo "cleanup failed in $WORK"
rmdir "$WORK"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
