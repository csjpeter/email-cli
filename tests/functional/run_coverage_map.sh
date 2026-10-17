#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-17
# Checks that docs/functional-coverage.md lists every capability the programs state.

# EMAIL-124 (testing-model.md, "Every capability has a functional test"): the
# commands, sub-commands and options each program names in its own --help must
# have a row in the map, every row names an existing test (F:<id> a check of
# run_functional.sh, P:<name> a PTY test) or a gap with its reason, and a row
# nothing in the programs supports any more is removed.  Needs the release build.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
DOC=docs/functional-coverage.md
FUNC=tests/functional/run_functional.sh
BIN=bin
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

[ -f "$DOC" ] && ok "1.1 $DOC exists" || { bad "1.1 $DOC is missing"; echo "Passed: $PASSED / $((PASSED + FAILED))"; exit 1; }

# keys <program>: one capability per line, "<program> <command|option>"
keys() {
    local prog="$1" out rc
    out=$("$BIN/$prog" --help 2>&1); rc=$?
    [ "$rc" -eq 0 ] || { echo "ERROR: $prog --help exited $rc" >&2; return 1; }
    printf '%s\n' "$out" | awk -v p="$prog" '
        function emit(k) { gsub(/,$/, "", k); print p " " k }
        /^  [a-z]/ {
            t1 = $1; t2 = $2
            if (t1 == "rules" || t1 == "cron") { emit(t1 " " t2); next }
            if ((t1 == "list" || t1 == "show") && $0 ~ / --/) {
                for (i = 2; i <= NF; i++) if ($i ~ /^--/) { emit(t1 " " $i); break }
                next
            }
            emit(t1); next
        }
        /^  -/ { emit($1) }'
}

all=""
for prog in email-cli email-cli-ro email-sync email-import-rules; do
    k=$(keys "$prog") || { bad "2.0 cannot list the capabilities of $prog"; continue; }
    all="$all$k"$'\n'
done
all=$(printf '%s' "$all" | sort -u)
n=$(printf '%s\n' "$all" | grep -c .)
[ "$n" -ge 60 ] && ok "2.1 $n capabilities read from the programs' --help" || bad "2.1 only $n capabilities found"

ref_exists() {
    case "$1" in
        F:*) grep -Eq "check(_not)?[[:space:]]+[^ ]*\"${1#F:}[[:space:]]" "$FUNC" ;;
        P:*) grep -Eqw -- "${1#P:}" tests/pty/*.c ;;
        S:*) [ -f "tests/functional/${1#S:}" ] ;;
        *)   return 1 ;;
    esac
}

# the map: "| `key` | tests | gap |"
rows=$(grep -E '^\| ' "$DOC" | grep -Ev '^\| (Capability|-)')
# the rows of the programs' own --help lists: everything before "## Other capabilities"
help_rows=$(sed '/^## Other capabilities/,$d' "$DOC" | grep -E '^\| `')
missing=0
while IFS= read -r key; do
    [ -n "$key" ] || continue
    printf '%s\n' "$help_rows" | grep -Fq "| \`$key\` |" || { bad "3.1 no row for '$key'"; missing=$((missing + 1)); }
done <<< "$all"
[ "$missing" -eq 0 ] && ok "3.2 every capability has a row"

stale=0
while IFS= read -r line; do
    key=$(printf '%s' "$line" | sed -E 's/^\| `([^`]*)`.*/\1/')
    printf '%s\n' "$all" | grep -Fxq "$key" || { bad "3.3 row '$key' matches nothing in the programs' --help"; stale=$((stale + 1)); }
done <<< "$help_rows"
[ "$stale" -eq 0 ] && ok "3.4 no row is stale"

bad_rows=0
while IFS= read -r line; do
    IFS='|' read -r _ key tests gap _ <<< "$line"
    key=$(echo "$key" | sed 's/^ *//;s/ *$//')
    tests=$(echo "$tests" | sed 's/^ *//;s/ *$//')
    gap=$(echo "$gap" | sed 's/^ *//;s/ *$//')
    if [ -z "$tests" ] || [ "$tests" = "-" ]; then
        [ -n "$gap" ] || { bad "4.1 $key: no test and no gap reason"; bad_rows=$((bad_rows + 1)); }
        continue
    fi
    for r in $tests; do
        ref_exists "$r" || { bad "4.2 $key: test '$r' does not exist"; bad_rows=$((bad_rows + 1)); }
    done
done <<< "$rows"
[ "$bad_rows" -eq 0 ] && ok "4.3 every row names existing tests or a reasoned gap"

echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
