#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-11-15
# Checks the email-tui libtuikit inventory table in docs/dev/tui-inventory.md.

# EMAIL-290: every part of email-tui has a row; every row says which libtuikit
# widget it uses or which hand-made code it is, and its status is `done`,
# `waits TUI-N` (the ticket exists in the docs, N a number) or `todo`.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
DOC="docs/dev/tui-inventory.md"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

[ -f "$DOC" ] && ok "1.1 the inventory exists" || { bad "1.1 $DOC is missing"; echo "$FAILED failed"; exit 1; }

rows=$(grep -E '^\| ' "$DOC" | grep -v -e '^| Part ' -e '^|---')
n=$(printf '%s\n' "$rows" | wc -l)
[ "$n" -ge 20 ] && ok "2.1 $n rows" || bad "2.1 only $n rows"

badrows=$(printf '%s\n' "$rows" | awk -F'|' '{s=$(NF-1); gsub(/^ +| +$/,"",s);
    if (s !~ /^(done|waits TUI-[0-9]+|todo( \(.*\))?)$/) print $0}')
[ -z "$badrows" ] && ok "3.1 every row has a valid status" || bad "3.1 invalid status: $badrows"

emptycol=$(printf '%s\n' "$rows" | awk -F'|' 'NF != 6 || $2 ~ /^ *$/ || $3 ~ /^ *$/ || $4 ~ /^ *$/ {print $0}')
[ -z "$emptycol" ] && ok "4.1 every row names part, place and widget" || bad "4.1 incomplete: $emptycol"

for part in "Raw mode" "Message list" "Reader" "Rule add form" "Send screen" "Path Tab"; do
    grep -q "| $part" "$DOC" && ok "5.x row: $part" || bad "5.x row missing: $part"
done

echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
