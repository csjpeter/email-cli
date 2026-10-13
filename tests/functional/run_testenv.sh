#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-13
# Tests the guards of tests/common/testenv.sh that need no container.

# EMAIL-35: testenv_destroy removes only a container of this project and this
# run, and the other functions refuse to work without one.  The container
# calls themselves are covered by tests/e2e/smtp_delivery.sh.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

# A directory of the test itself under build/, with a fake sudo that records
# any call: the guards must stop before one is made.
WORK="$ROOT/build/testenv-test-$$"
mkdir -p "$WORK/bin" || exit 1
cat > "$WORK/bin/sudo" <<'FAKE'
#!/bin/sh
echo "sudo $*" >> "$SUDO_LOG"
exit 0
FAKE
chmod +x "$WORK/bin/sudo"
export SUDO_LOG="$WORK/sudo.log"
: > "$SUDO_LOG"
PATH="$WORK/bin:$PATH"
export PATH

# shellcheck source=../common/testenv.sh
. "$ROOT/tests/common/testenv.sh"

TESTENV_NAME="issue-manager-pg"
out=$(testenv_destroy 2>&1); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "refusing to destroy" \
    && ok "1.1 a container of another project is refused" || bad "1.1 (rc=$rc: $out)"
[ ! -s "$SUDO_LOG" ] && ok "1.2 no sudo call was made for it" || bad "1.2 ($(cat "$SUDO_LOG"))"

TESTENV_NAME=""
out=$(testenv_destroy 2>&1); rc=$?
[ "$rc" -eq 0 ] && ok "2.1 destroy without a container is a no-op" || bad "2.1 (rc=$rc: $out)"

out=$(testenv_exec true 2>&1); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "no container" \
    && ok "3.1 exec without a container fails" || bad "3.1 (rc=$rc: $out)"
out=$(testenv_push "$0" /root/x 2>&1); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "no container" \
    && ok "3.2 push without a container fails" || bad "3.2 (rc=$rc: $out)"

TESTENV_NAME="email-cli-x-1"
out=$(testenv_push "$WORK/missing" /root/x 2>&1); rc=$?
[ "$rc" -ne 0 ] && echo "$out" | grep -q "no such file" \
    && ok "3.3 push of a missing file fails" || bad "3.3 (rc=$rc: $out)"
TESTENV_NAME=""

[ "$(grep -c '^sudo' "$SUDO_LOG")" -eq 0 ] && ok "4.1 no sudo call anywhere" || bad "4.1 ($(cat "$SUDO_LOG"))"

# Interface: the five functions of the policy exist.
for fn in testenv_create testenv_install testenv_push testenv_exec testenv_destroy; do
    type "$fn" >/dev/null && ok "5.$fn defined" || bad "5.$fn missing"
done

rm -f "$WORK/bin/sudo" "$SUDO_LOG" "$WORK/rc"
rmdir "$WORK/bin" "$WORK"
echo "=== run_testenv: $PASSED passed, $FAILED failed ==="
[ "$FAILED" -eq 0 ]
