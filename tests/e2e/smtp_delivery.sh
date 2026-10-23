#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-13
# End-to-end test: email-cli sends through a real SMTP daemon and the message arrives.

# EMAIL-35.  What this tests, in a fresh LXC container:
#   - the email-cli package, built from this tree, installs from its .deb
#   - Postfix (real server software, installed from Debian) accepts a message
#     over implicit TLS with a certificate email-cli VERIFIES (no SSL_NO_VERIFY)
#   - `email-cli send` delivers it: the local mailbox holds the headers, the body
#     and, with --attach, the attachment
#   - a certificate for another name is refused
#   - a closed port makes the send fail with a non-zero status
# Gmail is tested at the functional level against a mock and has no end-to-end
# test: there is no Gmail to run in a container (policies/testing-model.md).
#
# Needs the package (./manage.sh package deb; `./manage.sh e2e` makes it), LXC
# and sudo for lxc-* on the container this run creates.  Runs by itself:
#   build/tests/e2e/bin/smtp_delivery
set -u

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=../common/testenv.sh
. "$PROJECT_ROOT/tests/common/testenv.sh"

PASSED=0
FAILED=0
ok()   { echo "  [PASS] $*"; PASSED=$((PASSED+1)); }
fail() { echo "  [FAIL] $*"; FAILED=$((FAILED+1)); }
check_contains() {  # desc pattern text
    if printf '%s\n' "$3" | grep -q -- "$2"; then ok "$1"; else fail "$1 (missing: '$2')"; fi
}
check_not_contains() {
    if printf '%s\n' "$3" | grep -q -- "$2"; then fail "$1 (found: '$2')"; else ok "$1"; fi
}

trap 'testenv_destroy' EXIT

DEB=""
for f in "$PROJECT_ROOT"/build/packages/email-cli_*_amd64.deb; do
    [ -f "$f" ] && DEB="$f"
done
if [ -z "$DEB" ]; then
    echo "ERROR: no email-cli .deb under build/packages; run ./manage.sh package deb" >&2
    exit 1
fi
echo "=== e2e: SMTP delivery through Postfix ($(basename "$DEB")) ==="

testenv_create smtp || { echo "ERROR: container not created" >&2; exit 1; }
testenv_install postfix openssl ca-certificates "$DEB" ||
    { echo "ERROR: install into the container failed" >&2; exit 1; }
testenv_push "$SCRIPT_DIR/smtp_server_setup.sh" /root/smtp_server_setup.sh || exit 1
OUT=$(testenv_exec sh /root/smtp_server_setup.sh)
RC=$?
echo "$OUT" | tail -n 5
if [ "$RC" -ne 0 ]; then echo "ERROR: Postfix setup failed" >&2; echo "$OUT" >&2; exit 1; fi
check_contains "1.1 Postfix accepts smtps connections" "smtps is up" "$OUT"

# The sender's account: a verified connection (no SSL_NO_VERIFY), the real name.
mkcfg() {  # mkcfg ACCOUNT HOST PORT
    testenv_exec su sender -c "mkdir -p ~/.config/email-cli/accounts/$1 && cat > ~/.config/email-cli/accounts/$1/config.ini <<CFG
EMAIL_HOST=imaps://127.0.0.1:1
EMAIL_USER=sender@mail.example.org
EMAIL_PASS=pw
EMAIL_FOLDER=INBOX
SMTP_HOST=smtps://$2:$3
SMTP_USER=sender@mail.example.org
SMTP_PASS=pw
CFG
chmod 0600 ~/.config/email-cli/accounts/$1/config.ini"
}
mkcfg sender@mail.example.org mail.example.org 465 || { fail "1.2 config written"; exit 1; }
CFGTXT=$(testenv_exec su sender -c 'cat ~/.config/email-cli/accounts/sender@mail.example.org/config.ini')
check_not_contains "1.2 verification is on (no SSL_NO_VERIFY)" "SSL_NO_VERIFY" "$CFGTXT"

# 2. A plain message.
OUT=$(testenv_exec su sender -c 'email-cli --account sender@mail.example.org -q send --to rcpt@mail.example.org --subject "E2E plain" --body "hello-e2e-body"')
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] && ok "2.1 send exits 0" || fail "2.1 send exits 0 (status $RC)"
check_contains "2.2 send reports success" "Message sent" "$OUT"

BOX=""
for _ in $(seq 1 30); do
    BOX=$(testenv_exec cat /var/mail/rcpt)
    printf '%s' "$BOX" | grep -q "hello-e2e-body" && break
    sleep 1
done
check_contains "2.3 the message is in the mailbox"      "hello-e2e-body"            "$BOX"
check_contains "2.4 Subject header arrived"             "^Subject: E2E plain"       "$BOX"
check_contains "2.5 To header arrived"                  "^To: .*rcpt@mail.example.org" "$BOX"
check_contains "2.6 From header arrived"                "^From: .*sender@mail.example.org" "$BOX"

# 3. A message with an attachment.
testenv_exec su sender -c 'printf "hello-attachment" > ~/note.txt' >/dev/null
OUT=$(testenv_exec su sender -c 'email-cli --account sender@mail.example.org -q send --to rcpt@mail.example.org --subject "E2E attach" --body "with file" --attach ~/note.txt')
RC=$?
echo "$OUT"
[ "$RC" -eq 0 ] && ok "3.1 send with attachment exits 0" || fail "3.1 send with attachment exits 0 (status $RC)"
BOX=""
for _ in $(seq 1 30); do
    BOX=$(testenv_exec cat /var/mail/rcpt)
    printf '%s' "$BOX" | grep -q "E2E attach" && break
    sleep 1
done
check_contains "3.2 the attachment message arrived"     "^Subject: E2E attach"      "$BOX"
check_contains "3.3 it carries the file name"           "filename=\"note.txt\""     "$BOX"
# "hello-attachment" in base64
check_contains "3.4 it carries the file content"        "aGVsbG8tYXR0YWNobWVudA=="  "$BOX"

# 4. A certificate for another name is refused (verification really is on).
mkcfg wrongname@mail.example.org localhost 465 >/dev/null
OUT=$(testenv_exec su sender -c 'email-cli --account wrongname@mail.example.org -q send --to rcpt@mail.example.org --subject "E2E wrong name" --body "never"')
RC=$?
echo "$OUT"
[ "$RC" -ne 0 ] && ok "4.1 a certificate for another host name fails" \
                || fail "4.1 a certificate for another host name fails (status 0)"
BOX=$(testenv_exec cat /var/mail/rcpt)
check_not_contains "4.2 nothing was delivered"          "E2E wrong name"            "$BOX"

# 5. A closed port fails.
mkcfg closedport@mail.example.org mail.example.org 1 >/dev/null
OUT=$(testenv_exec su sender -c 'email-cli --account closedport@mail.example.org -q send --to rcpt@mail.example.org --subject "E2E closed" --body "never"')
RC=$?
echo "$OUT"
[ "$RC" -ne 0 ] && ok "5.1 a closed port fails with a non-zero status" \
                || fail "5.1 a closed port fails with a non-zero status (status 0)"

echo "=== e2e smtp_delivery: $PASSED passed, $FAILED failed ==="
[ "$FAILED" -eq 0 ]
