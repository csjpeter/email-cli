#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-23
# Tests the package version derivation and the package tag of manage.sh.

# EMAIL-229: version = <project version>.<commit count>; a tag restarts nothing;
# a raised project version sorts above; no git gives the bare version; the
# tag-build verb makes the annotated tag v<version> locally and pushes nothing.
# Everything lives under build/.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
WORK="$ROOT/build/version-test-$$"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }
cleanup() { [ -d "$WORK" ] && ( cd "$WORK" && rm -rf repo bare nogit out ) && rmdir "$WORK"; }
trap cleanup EXIT

mkdir -p "$WORK/repo" "$WORK/nogit" || exit 1
R="$WORK/repo"
ver() { cmake -DPROJECT_VERSION="$1" -DSRC="$2" -P "$ROOT/cmake/derive_version.cmake"; }
git -C "$R" init -q || exit 1
git -C "$R" config user.email t@example.org; git -C "$R" config user.name t
for i in 1 2 3; do git -C "$R" commit -q --allow-empty -m "EMAIL-$i work"; done

# 1. the derivation
v=$(ver 0.1.0 "$R"); [ "$v" = "0.1.0.3" ] && ok "1.1 3 commits -> 0.1.0.3" || bad "1.1 ($v)"
git -C "$R" tag -a v0.1.0 -m release
v=$(ver 0.1.0 "$R"); [ "$v" = "0.1.0.3" ] && ok "1.2 the same tree after a tag -> 0.1.0.3" || bad "1.2 ($v)"
git -C "$R" commit -q --allow-empty -m "EMAIL-4 work"
v=$(ver 0.1.0 "$R"); [ "$v" = "0.1.0.4" ] && ok "1.3 one more commit after the tag -> 0.1.0.4" || bad "1.3 ($v)"
v2=$(ver 0.2.0 "$R")
dpkg --compare-versions "$v2" gt "$v" && ok "1.4 a raised project version ($v2) sorts above $v" || bad "1.4 ($v2)"
v=$(GIT_CEILING_DIRECTORIES="$WORK" ver 0.1.0 "$WORK/nogit"); [ "$v" = "0.1.0" ] && ok "1.5 no git history -> the bare version" || bad "1.5 ($v)"

# 2. the tag verb
git init -q --bare "$WORK/bare" || exit 1
git -C "$R" remote add origin "$WORK/unreachable-$$"
( cd "$R" && "$ROOT/manage.sh" tag-build 0.1.0.4 ) > "$WORK/out" 2>&1; rc=$?
[ "$rc" -eq 0 ] && [ "$(git -C "$R" cat-file -t v0.1.0.4)" = tag ] \
    && ok "2.1 an annotated tag v0.1.0.4 is made" || bad "2.1 (rc=$rc: $(cat "$WORK/out"))"
git -C "$R" remote set-url origin "$WORK/bare"
[ -z "$(git -C "$WORK/bare" tag -l)" ] \
    && ok "2.2 nothing is pushed (unreachable origin did not fail it)" || bad "2.2"
git -C "$R" tag -n1 -l v0.1.0.4 | grep -q 'EMAIL-4' && ! git -C "$R" tag -n1 -l v0.1.0.4 | grep -q 'EMAIL-2' \
    && ok "2.3 the message names the keys since the previous tag only" || bad "2.3 ($(git -C "$R" tag -n1 -l v0.1.0.4))"
( cd "$R" && "$ROOT/manage.sh" tag-build 0.1.0.4 ) > "$WORK/out" 2>&1; rc=$?
[ "$rc" -eq 0 ] && ok "2.4 the same tag at the same commit again is fine" || bad "2.4 (rc=$rc)"
git -C "$R" commit -q --allow-empty -m "EMAIL-5 work"
( cd "$R" && "$ROOT/manage.sh" tag-build 0.1.0.4 ) > "$WORK/out" 2>&1; rc=$?
[ "$rc" -ne 0 ] && ok "2.5 the same tag at another commit is refused" || bad "2.5"

echo "version: $PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
