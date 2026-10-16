#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-16
# Checks that the programs honour TERM.

# EMAIL-108: with TERM unset or "dumb" the programs write no colour, cursor or
# erase sequences (email-tui, which draws a full-screen interface, refuses to
# start); with TERM=xterm-256color the sequences are written as before.
# email-sync is the witness: its progress lines end in an erase-line code.
# Needs the release build (bin/).
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
BIN="$ROOT/bin"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

WORK="$ROOT/build/term-$$"
mkdir -p "$WORK/home/.config/email-cli/accounts/testuser" "$WORK/mocks" "$WORK/ports" || exit 1
PIDS=""
cleanup() {
    local p
    for p in $PIDS; do
        [ -d "/proc/$p" ] && kill "$p"
    done
    [ -n "$WORK" ] && [ -d "$WORK" ] && ( cd "$WORK" && rm -rf home mocks ports ) && rmdir "$WORK"
}
trap cleanup EXIT

ESC=$(printf '\033')

# 1. email-tui refuses a terminal without escape sequences and says why.
out=$(env -u TERM "$BIN/email-tui" 2>&1); rc=$?
[ "$rc" = "1" ] && ok "1.1 email-tui with TERM unset exits 1" || bad "1.1 email-tui with TERM unset: exit $rc"
case "$out" in *"understands escape sequences"*) ok "1.2 the message says what is needed" ;; *) bad "1.2 message: $out" ;; esac
case "$out" in *"$ESC"*) bad "1.3 the refusal contains an escape byte" ;; *) ok "1.3 the refusal is plain text" ;; esac
out=$(TERM=dumb "$BIN/email-tui" 2>&1); rc=$?
[ "$rc" = "1" ] && ok "1.4 email-tui with TERM=dumb exits 1" || bad "1.4 email-tui with TERM=dumb: exit $rc"
out=$(TERM=dumb "$BIN/email-tui" --version 2>&1)
case "$out" in email-tui\ *) ok "1.5 --version still works on a dumb terminal" ;; *) bad "1.5 --version: $out" ;; esac

# 2. email-sync against the mock IMAP server: the progress lines.
gcc "$ROOT/tests/functional/mock_imap_server.c" -o "$WORK/mocks/imap" -lssl -lcrypto || exit 1
gcc "$ROOT/tests/functional/free_port.c"        -o "$WORK/mocks/free_port" || exit 1
PORT=$("$WORK/mocks/free_port") || exit 1
( cd "$ROOT/build" && MOCK_IMAP_PORT=$PORT "$WORK/mocks/imap" ) >"$WORK/ports/imap.log" 2>&1 &
PIDS="$PIDS $!"
sleep 1
cat > "$WORK/home/.config/email-cli/accounts/testuser/config.ini" <<INI
EMAIL_HOST=imaps://localhost:$PORT
EMAIL_USER=testuser@example.com
EMAIL_PASS=testpass
EMAIL_FOLDER=INBOX
INI
chmod 0600 "$WORK/home/.config/email-cli/accounts/testuser/config.ini"

sync_with() {   # sync_with <TERM or "-"> : the output of a fresh sync
    local t="$1"
    ( cd "$WORK" && rm -rf home/.local home/.cache )
    if [ "$t" = "-" ]; then
        env -u TERM HOME="$WORK/home" SSL_CERT_FILE="$ROOT/build/tests/certs/test.crt" "$BIN/email-sync" 2>&1
    else
        env TERM="$t" HOME="$WORK/home" SSL_CERT_FILE="$ROOT/build/tests/certs/test.crt" "$BIN/email-sync" 2>&1
    fi
}
out=$(sync_with xterm-256color)
case "$out" in *"${ESC}[K"*) ok "2.1 TERM=xterm-256color: the erase-line code is written" ;; *) bad "2.1 no erase-line code in: $out" ;; esac
out=$(sync_with dumb)
case "$out" in *"$ESC"*) bad "2.2 TERM=dumb: an escape byte reached the output" ;; *"UID"*) ok "2.2 TERM=dumb: no escape byte, the progress text is there" ;; *) bad "2.2 TERM=dumb output: $out" ;; esac
out=$(sync_with -)
case "$out" in *"$ESC"*) bad "2.3 TERM unset: an escape byte reached the output" ;; *"UID"*) ok "2.3 TERM unset: no escape byte, the progress text is there" ;; *) bad "2.3 TERM unset output: $out" ;; esac

echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
