#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-17
# Checks that docs/interface-parity.md is backed by tests.

# EMAIL-123 (interface-parity-model.md): every operation row of the parity
# table marked `yes` for an interface names at least one test of it, and every
# named test exists (`F:<id>` is a check of run_functional.sh, `P:<name>` a PTY
# test function); a `no` has a reason; the survey carries its date and commit.
# The named tests themselves run in the functional and PTY suites.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
DOC=docs/interface-parity.md
FUNC=tests/functional/run_functional.sh
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

[ -f "$DOC" ] && ok "1.1 $DOC exists" || { bad "1.1 $DOC is missing"; echo "Passed: $PASSED / $((PASSED + FAILED))"; exit 1; }
grep -Eq '^Survey of \*\*20[0-9]{2}-[0-9]{2}-[0-9]{2}\*\*, read from the code at commit `[0-9a-f]{12}`' "$DOC" \
    && ok "1.2 the survey carries its date and the commit it was read at" || bad "1.2 no 'Survey of **DATE**, read from the code at commit `ID`' line"

# ref_exists <ref>: F:<id> or P:<name>
ref_exists() {
    case "$1" in
        F:*) grep -Eq "check(_not)?[[:space:]]+[^ ]*\"${1#F:}[[:space:]]" "$FUNC" ;;
        P:*) grep -Eqw -- "${1#P:}" tests/pty/*.c ;;
        *)   return 1 ;;
    esac
}

rows=0; yes_refs=0; in_ops=0
while IFS= read -r line; do
    case "$line" in
        "## Operations") in_ops=1; continue ;;
        "## "*) in_ops=0; continue ;;
    esac
    [ "$in_ops" -eq 1 ] || continue
    case "$line" in
        "| Operation |"*|"|---"*|"|-----"*) continue ;;
        "| "*) ;;
        *) continue ;;
    esac
    # only the operations table has six columns
    IFS='|' read -r _ op cli clit tui tuit reason _ <<< "$line"
    [ -n "${tuit+x}" ] || continue
    op=$(echo "$op" | sed 's/^ *//;s/ *$//')
    rows=$((rows + 1))
    for side in cli tui; do
        if [ "$side" = cli ]; then val="$cli"; refs="$clit"; else val="$tui"; refs="$tuit"; fi
        val=$(echo "$val" | sed 's/^ *//;s/ *$//')
        refs=$(echo "$refs" | sed 's/^ *//;s/ *$//')
        case "$val" in
            yes*)
                if [ -z "$refs" ]; then bad "2.1 '$op' ($side) is yes but names no test"; continue; fi
                for r in $refs; do
                    yes_refs=$((yes_refs + 1))
                    ref_exists "$r" || bad "2.2 '$op' ($side): test '$r' does not exist"
                done ;;
            no*)
                [ -n "$(echo "$reason" | tr -d ' ')" ] || bad "2.3 '$op': $side is no, with no reason" ;;
            *) bad "2.4 '$op': $side column is '$val' (yes or no expected)" ;;
        esac
    done
done < "$DOC"

[ "$rows" -ge 30 ] && ok "3.1 the table lists $rows operations" || bad "3.1 only $rows operation rows found"
[ "$yes_refs" -ge 40 ] && ok "3.2 $yes_refs test references checked" || bad "3.2 only $yes_refs references"
[ "$FAILED" -eq 0 ] && ok "3.3 every yes is backed by an existing test and every no has a reason"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
