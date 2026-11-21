#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-11-21
# Tests that the Dovecot runtime dir is outside the checkout and free of ACLs (EMAIL-320).

# The work tree gets a default ACL (as in the agent containers); the runtime
# directory of dovecot_env_init must not inherit it and must lie outside the
# tree.  No Dovecot is started; sudo is a fake that runs its command as is.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
PROJECT_ROOT="$PWD"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

WORK="$PROJECT_ROOT/build/dovecot-env-test-$$"
mkdir -p "$WORK/bin" || exit 1
printf '#!/bin/sh\nexec "$@"\n' > "$WORK/bin/sudo"
chmod +x "$WORK/bin/sudo"
PATH="$WORK/bin:$PATH"
export PATH

# shellcheck source=../integration/dovecot_env.sh
. "$PROJECT_ROOT/tests/integration/dovecot_env.sh"

# The default ACL on the directory the work dir is made in.
ACL_DIR="$PROJECT_ROOT/build/tests/integration"
mkdir -p "$ACL_DIR"
if command -v setfacl >/dev/null; then
    setfacl -d -m o::r-x "$ACL_DIR" && ok "1.1 default ACL set on the work tree" || bad "1.1 setfacl failed"
else
    ok "1.1 setfacl absent: the location check below still holds"
fi

dovecot_env_init testuser testpass || bad "2.0 init failed"
case "$DOVE_BASE" in
    "$PROJECT_ROOT"/*) bad "2.1 runtime dir is inside the checkout: $DOVE_BASE" ;;
    *) ok "2.1 runtime dir is outside the checkout" ;;
esac
[ "$(stat -c %a "$DOVE_BASE")" = "755" ] && ok "2.2 mode 755" || bad "2.2 mode $(stat -c %a "$DOVE_BASE")"
if command -v getfacl >/dev/null; then
    if getfacl -p -c "$DOVE_BASE" | grep -qE '^(default:|user:[^:]|group:[^:])'; then
        bad "2.3 ACL entries on $DOVE_BASE"
    else
        ok "2.3 no ACL entries on the runtime dir"
    fi
fi
RUNDIR="$DOVE_BASE"
dovecot_env_cleanup
[ ! -e "$RUNDIR" ] && ok "3.1 cleanup removed the runtime dir" || bad "3.1 $RUNDIR remains"
[ ! -e "$WORK_DIR" ] && ok "3.2 cleanup removed the work dir" || bad "3.2 $WORK_DIR remains"

# The guard refuses a path that is not the made one.
mkdir -p "$WORK/other"
DOVE_BASE="$WORK/other"
WORK_DIR="$WORK_BASE/work-guard"
out=$(dovecot_env_cleanup 2>&1); rc=$?
[ "$rc" -ne 0 ] && [ -d "$WORK/other" ] && ok "4.1 an unsafe runtime path is refused" || bad "4.1 (rc=$rc: $out)"

if command -v setfacl >/dev/null; then
    setfacl -k "$ACL_DIR" || bad "5.1 default ACL not removed"
fi
rmdir "$WORK/other"
rm -f "$WORK/bin/sudo"
rmdir "$WORK/bin" "$WORK"
echo "dovecot_env: $PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
