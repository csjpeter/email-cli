#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-11
# Tests that the source header gate of manage.sh check fails on a file without a header.

# EMAIL-29: `manage.sh headers DIR` passes a tree whose files all carry the
# header and fails, naming the file, on each kind of defective header.  Works
# in a scratch tree under build/, so it needs no real build.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
WORK="$ROOT/build/source-header-test-$$"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

mkdir -p "$WORK/tree/src" || exit 1
T="$WORK/tree"
good_c() {
    printf '%s\n' '/* SPDX-License-Identifier: GPL-3.0-only */' \
        '/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */' \
        "/* Created: ${2:-2026-03-24} */" '/* A file that is headed. */' 'int x;' > "$1"
}
run() { out=$("$ROOT/manage.sh" headers "$T" 2>&1); rc=$?; }

# 1. a tree of headed files passes, a shebang may come first.
good_c "$T/src/a.c"; good_c "$T/src/a.h"
printf '%s\n' '#!/bin/sh' '# SPDX-License-Identifier: GPL-3.0-only' \
    '# Copyright (c) 2026 Peter Csaszar (Császár Péter)' '# Created: 2026-03-24' \
    '# A headed script.' 'true' > "$T/src/s.sh"
run
[ "$rc" -eq 0 ] && ok "1.1 a headed tree passes" || bad "1.1 headed tree failed: $out"
echo "$out" | grep -q "3 files checked" && ok "1.2 all three files were looked at" || bad "1.2 count: $out"

# 2. each defect fails and names the file.
expect_fail() {
    run
    [ "$rc" -ne 0 ] && echo "$out" | grep -q "$2" && ok "$1" || bad "$1 (rc=$rc: $out)"
    good_c "$T/src/bad.c"; rm "$T/src/bad.c"
}
printf 'int x;\n' > "$T/src/bad.c";                        expect_fail "2.1 no header fails" "src/bad.c"
good_c "$T/src/bad.c"; sed -i '1d' "$T/src/bad.c";         expect_fail "2.2 no licence line fails" "src/bad.c"
good_c "$T/src/bad.c"; sed -i '2d' "$T/src/bad.c";         expect_fail "2.3 no copyright line fails" "src/bad.c"
good_c "$T/src/bad.c"; sed -i '3d' "$T/src/bad.c";         expect_fail "2.4 no Created line fails" "src/bad.c"
good_c "$T/src/bad.c" 2026-02-31;                          expect_fail "2.5 an invalid date fails" "src/bad.c"
good_c "$T/src/bad.c" 2999-01-01;                          expect_fail "2.6 a future date fails" "src/bad.c"
good_c "$T/src/bad.c"; sed -i '4d' "$T/src/bad.c";         expect_fail "2.7 no purpose line fails" "src/bad.c"
good_c "$T/src/bad.c"; sed -i 's/GPL-3.0-only/MIT/' "$T/src/bad.c"; expect_fail "2.8 a licence differing from LICENSE fails" "src/bad.c"
printf 'cmake_minimum_required(VERSION 3.10)\n' > "$T/CMakeLists.txt"
run
[ "$rc" -ne 0 ] && echo "$out" | grep -q "CMakeLists.txt" && ok "2.9 an unheaded CMakeLists.txt fails" || bad "2.9 (rc=$rc: $out)"

case "$WORK" in
    "$ROOT"/build/source-header-test-*) ;;
    *) echo "ERROR: refusing unsafe path '$WORK'" >&2; exit 1 ;;
esac
rm -rf "$ROOT/build/source-header-test-${WORK##*/source-header-test-}"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
