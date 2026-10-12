#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-11
# Tests that the private-names gate of manage.sh check fails on the private domain.

# EMAIL-40: `manage.sh names DIR` passes a git tree whose tracked files do not
# name the owner's private domain and fails, naming the file, when one does.
# Works in a scratch repository under build/.  The name is built from parts so
# that this file does not contain it either.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
WORK="$ROOT/build/private-names-test-$$"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }
NAME="csasz""ar"".em""ail"

mkdir -p "$WORK/tree" || exit 1
T="$WORK/tree"
git -C "$T" init -q || exit 1
run() { out=$("$ROOT/manage.sh" names "$T" 2>&1); rc=$?; }

printf 'mail user@example.org\n' > "$T/a.txt"
git -C "$T" add a.txt
run
[ "$rc" -eq 0 ] && ok "1.1 a clean tree passes" || bad "1.1 clean tree failed: $out"

printf 'mail user@%s\n' "$NAME" > "$T/b.txt"
run
[ "$rc" -eq 0 ] && ok "1.2 an untracked file is not looked at" || bad "1.2 untracked: $out"

git -C "$T" add b.txt
run
[ "$rc" -ne 0 ] && echo "$out" | grep -q "b.txt" && ok "2.1 a tracked file with the name fails and is named" || bad "2.1 (rc=$rc: $out)"

printf "host box.%s\n" "${NAME^^}" > "$T/b.txt"
run
[ "$rc" -ne 0 ] && ok "2.2 the name in upper case fails" || bad "2.2 (rc=$rc: $out)"

printf 'clean\n' > "$T/b.txt"
run
[ "$rc" -eq 0 ] && ok "3.1 a file cleaned of the name passes" || bad "3.1 (rc=$rc: $out)"

case "$WORK" in
    "$ROOT"/build/private-names-test-*) ;;
    *) echo "ERROR: refusing unsafe path '$WORK'" >&2; exit 1 ;;
esac
rm -rf "$ROOT/build/private-names-test-${WORK##*/private-names-test-}"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
