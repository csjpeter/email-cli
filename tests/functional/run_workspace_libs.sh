#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-12
# Checks both ways the workspace libraries come in, and the submodule handling.

# EMAIL-41 (policies/dependencies-model.md, "two ways"): WORKSPACE_LIBS selects
# bundled, system or auto; the system way builds and passes the unit tests
# against the installed -dev packages (libtuikit through its static flavour: the
# programs are fully static, EMAIL-103); manage.sh
# fetches a missing or out-of-pin submodule and refuses a pin that is not a
# release tag.  Everything lives under build/.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
SYS="$ROOT/build/workspace-libs-system"
SCR="wl-scratch-$$"
PASSED=0; FAILED=0

ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

# 1. The switch.
out=$(cmake -S "$ROOT" -B "$SYS" -DWORKSPACE_LIBS=bogus 2>&1); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "use bundled" && ok "1.1 an unknown way is refused with the choices" || bad "1.1 (rc=$rc) $out"

# 2. System way: configure, build, run the unit tests.
if ! pkg-config --exists libtuikit libptytest; then
    bad "2.0 the -dev packages libtuikit-dev and libptytest-dev are not installed"
else
    out=$(cmake -S "$ROOT" -B "$SYS" -DCMAKE_BUILD_TYPE=Release -DWORKSPACE_LIBS=system 2>&1); rc=$?
    [ "$rc" -eq 0 ] && ok "2.1 system way configures" || { bad "2.1 configure rc=$rc"; echo "$out" | tail -8; }
    echo "$out" | grep -q 'libtuikit: system' && ok "2.2 libtuikit comes from the system" || bad "2.2 no 'libtuikit: system'"
    echo "$out" | grep -q 'libptytest: system' && ok "2.3 libptytest comes from the system" || bad "2.3 no 'libptytest: system'"
    out=$(cmake --build "$SYS" --target email-tui email-cli tests-unit -- -j"$(nproc)" 2>&1); rc=$?
    [ "$rc" -eq 0 ] && ok "2.4 system way builds" || { bad "2.4 build rc=$rc"; echo "$out" | tail -15; }
    if [ "$(readelf -d "$SYS/email-tui" 2>&1 | grep -c 'Shared library')" = "0" ] && [ "$(readelf -l "$SYS/email-tui" | grep -c INTERP)" = "0" ]; then
        ok "2.5 email-tui is fully static also against the system libtuikit"
    else
        bad "2.5 email-tui needs a shared library or a loader"
    fi
    out=$( cd "$SYS" && ./tests/unit/bin/test-runner 2>&1 ); rc=$?
    [ "$rc" -eq 0 ] && ok "2.6 unit tests pass against the system libraries" || { bad "2.6 unit tests rc=$rc"; echo "$out" | tail -15; }
    # auto takes the installed package when it is there.
    out=$(cmake -S "$ROOT" -B "$SYS" -DWORKSPACE_LIBS=auto 2>&1); rc=$?
    echo "$out" | grep -q 'libtuikit: system' && ok "2.7 auto takes the installed package" || bad "2.7 auto (rc=$rc)"
    # auto without the package falls back to the submodule.
    out=$(cmake -S "$ROOT" -B "$ROOT/build/$SCR-auto" -DWORKSPACE_LIBS=auto -DCMAKE_DISABLE_FIND_PACKAGE_tuikit=ON 2>&1); rc=$?
    echo "$out" | grep -q 'libtuikit: bundled' && ok "2.8 auto falls back to the submodule" || bad "2.8 auto fallback (rc=$rc)"
    ( cd "$ROOT/build" && rm -rf "$SCR-auto" )
fi

# 3. The bundled way is what the main build uses (the package test asserts the
#    static link); the cache says so.
grep -q '^WORKSPACE_LIBS:STRING=bundled$' "$ROOT/build/CMakeCache.txt" \
    && ok "3.1 the main build is bundled" || bad "3.1 build/CMakeCache.txt: $(grep '^WORKSPACE_LIBS' "$ROOT/build/CMakeCache.txt")"

# 4. manage.sh and the submodules, in a scratch project under build/.
P="$ROOT/build/$SCR/proj"; L="$ROOT/build/$SCR/libx"
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=protocol.file.allow GIT_CONFIG_VALUE_0=always
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.org GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.org
export MANAGE_TIMING_LOG="$ROOT/build/$SCR/timing.log"
mkdir -p "$L" "$P" || exit 1
( cd "$L" && git init -q && echo x > x && git add x && git commit -q -m one ) || exit 1
( cd "$P" && git init -q && cp "$ROOT/manage.sh" . && git submodule -q add "$L" deps/libx \
    && git add -A && git commit -q -m proj ) || { bad "4.0 scratch project"; }

run() { ( cd "$P" && "$@" 2>&1 ); }
out=$(run env WORKSPACE_LIBS=bundled ./manage.sh build); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q 'not a release tag' && ok "4.1 a pin that is not a release tag fails" || bad "4.1 (rc=$rc) $out"

( cd "$L" && git tag v1.0.0 ) && ( cd "$P/deps/libx" && git fetch -q --tags )
out=$(run env WORKSPACE_LIBS=bundled ./manage.sh build)
echo "$out" | grep -q 'deps/libx: v1.0.0' && ok "4.2 a release-tag pin is accepted and named" || bad "4.2 $out"

( cd "$P" && git submodule -q deinit -f deps/libx )
out=$(run env WORKSPACE_LIBS=bundled ./manage.sh build)
echo "$out" | grep -q 'Fetching submodule deps/libx' && echo "$out" | grep -q 'deps/libx: v1.0.0' \
    && ok "4.3 a missing submodule is fetched by manage.sh" || bad "4.3 $out"

( cd "$P" && git submodule -q deinit -f deps/libx )
out=$(run env WORKSPACE_LIBS=system ./manage.sh build)
echo "$out" | grep -q 'Fetching' && bad "4.4 system way fetched a submodule" || ok "4.4 the system way fetches nothing"

out=$(run env WORKSPACE_LIBS=bogus ./manage.sh build); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "WORKSPACE_LIBS='bogus'" && ok "4.5 manage.sh refuses an unknown way" || bad "4.5 (rc=$rc) $out"

( cd "$ROOT/build" && rm -rf "$SCR" )
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
