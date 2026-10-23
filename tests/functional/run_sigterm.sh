#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-23
# Tests that email-tui leaves on SIGTERM without running exit() inside the handler.

# EMAIL-224: the handler was `signal(SIGTERM, exit)`.  exit() runs the atexit
# and OpenSSL cleanup handlers, which free memory; a SIGTERM that lands inside
# free() or malloc() then frees the same block twice (AddressSanitizer:
# "attempting double-free", OSSL_ERR_STATE_free, seen in the sanitize run).
# Calling exit() from a handler is not async-signal-safe.  The handler now
# leaves with _exit(128 + SIGTERM), so the program's own status is 143 and no
# cleanup code runs in the middle of an allocation.  (A coverage build still
# calls exit(), to flush the counters.)  With exit() the status was 15.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
BIN="${EMAIL_TEST_BIN_DIR:-$ROOT/bin}"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

WORK="$ROOT/build/sigterm-test-$$"
mkdir -p "$WORK/home" || exit 1
PID=""
cleanup() {
    [ -n "$PID" ] && [ -d "/proc/$PID" ] && kill -KILL "$PID"
    exec 3>&-
    [ -n "$WORK" ] && [ -d "$WORK" ] && ( cd "$WORK" && rm -rf home && rm -f fifo out err ) && rmdir "$WORK"
}
trap cleanup EXIT

if [ ! -x "$BIN/email-tui" ]; then bad "0.0 $BIN/email-tui is not built"; echo "Passed: $PASSED / $((PASSED + FAILED))"; exit 1; fi

# No configuration: email-tui starts the setup wizard and blocks reading its
# standard input, which is a fifo nobody writes to.
mkfifo "$WORK/fifo" || exit 1
exec 3<>"$WORK/fifo"
HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" XDG_DATA_HOME="$WORK/home/.local/share" \
    XDG_CACHE_HOME="$WORK/home/.cache" TERM=xterm \
    "$BIN/email-tui" < "$WORK/fifo" > "$WORK/out" 2> "$WORK/err" &
PID=$!

# Wait until the handler is installed: /proc/PID/status lists the signals the
# process catches (SigCgt, a hex mask); SIGTERM is bit 14 (0x4000).  The wrapper
# of valgrind-all execs Valgrind first, so this can take a while.
caught=0
for _ in $(seq 1 600); do
    [ -d "/proc/$PID" ] || break
    mask=$(awk '/^SigCgt:/ { print $2 }' "/proc/$PID/status")
    if [ -n "$mask" ] && [ $(( 0x$mask & 0x4000 )) -ne 0 ]; then caught=1; break; fi
    sleep 0.1
done
if [ "$caught" -eq 1 ] && [ -d "/proc/$PID" ]; then ok "1.1 email-tui is running and catches SIGTERM"
else bad "1.1 email-tui does not catch SIGTERM: $(cat "$WORK/err")"; fi

kill -TERM "$PID"
wait "$PID"; rc=$?
PID=""
[ "$rc" -eq 143 ] && ok "2.1 SIGTERM ends it with status 143 (128 + SIGTERM)" \
    || bad "2.1 status $rc after SIGTERM, expected 143 (15 means exit() ran in the handler)"

echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
