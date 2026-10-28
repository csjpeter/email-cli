#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-03-25
# Integration test of email-cli against a real Dovecot IMAP server over TLS.

# Integration test: email-cli against a real Dovecot IMAP server over TLS 1.2+.
#
# Acceptance criteria:
#   - Connects via imaps:// (TLS 1.2 minimum, enforced by Dovecot ssl_min_protocol)
#   - Lists messages in INBOX (UID SEARCH ALL)
#   - Downloads and displays message content
#
# Environment: one Dovecot process of its own, started by this script under
# build/tests/integration/work-<pid> on a port the kernel picks, seeded with
# the two messages of tests/integration/seed, and stopped with its files
# removed when the script ends.  No container, no compose, no volume: agents
# running it side by side share nothing (EMAIL-228; the setup is
# dovecot_env.sh, shared with run_local_dovecot.sh).
#
# Usage:
#   ./run_integration.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# The programs under test (manage.sh sanitize / memcheck set it).
BIN_DIR="${EMAIL_TEST_BIN_DIR:-$PROJECT_ROOT/bin}"

IMAP_USER="testuser"
IMAP_PASS="testpass"
IMAP_FOLDER="INBOX"

. "$SCRIPT_DIR/dovecot_env.sh"
dovecot_env_init "$IMAP_USER" "$IMAP_PASS"
trap dovecot_env_cleanup EXIT
TEST_HOME="$WORK_DIR/home"

# ── Run integration test ─────────────────────────────────────────────────

echo "=== email-cli Integration Test (Dovecot IMAPS / TLS 1.2+) ==="
echo ""

# 1. Build release binary if needed
if [ ! -f "$BIN_DIR/email-cli" ]; then
    echo "Binary not found. Building..."
    (cd "$PROJECT_ROOT" && ./manage.sh build)
fi

# 2. Start this run's own Dovecot with the seeded messages, and write a
#    config pointing at it.  The home lives under the work dir in build/.
dovecot_env_seed "$SCRIPT_DIR/seed"
dovecot_env_start
IMAP_HOST="imaps://localhost:$IMAP_PORT"
echo "IMAPS is on $IMAP_HOST"

mkdir -p "$TEST_HOME/.config/email-cli/accounts/$IMAP_USER" "$TEST_HOME/.cache/email-cli/logs"
cat > "$TEST_HOME/.config/email-cli/accounts/$IMAP_USER/config.ini" <<EOF
EMAIL_HOST=$IMAP_HOST
EMAIL_USER=$IMAP_USER
EMAIL_PASS=$IMAP_PASS
EMAIL_FOLDER=$IMAP_FOLDER
SSL_NO_VERIFY=1
EOF

# Helper: query FLAGS for all messages in INBOX via doveadm against this run's Dovecot
flags_for_user() {
    dovecot_env_doveadm fetch -u "$IMAP_USER" "flags" mailbox INBOX ALL | grep -v '^$' | sed 's/\\Recent//'
    # \Recent belongs to the session that first sees a message, not to the
    # message, so it is left out of the comparison.
}

# 4. Verify FLAGS before any email-cli operation (baseline)
BEFORE_FLAGS=$(flags_for_user)

# 5. Run email-cli list (batch) and capture output
echo "Running: email-cli list --batch ..."
LIST_RC=0
LIST_OUTPUT=$(HOME="$TEST_HOME" "$BIN_DIR/email-cli" list --batch 2>&1) || LIST_RC=$?
echo "email-cli list exit code: $LIST_RC"
echo "$LIST_OUTPUT"
echo ""

# 6. Run email-cli show for each UID found in list output
UIDS=$(echo "$LIST_OUTPUT" | grep -Eo '^[[:space:]]+[0-9]+' | tr -d ' ' | head -5)
SHOW_OUTPUT=""
for MSG_UID in $UIDS; do
    echo "Running: email-cli show $MSG_UID ..."
    OUT=$(HOME="$TEST_HOME" "$BIN_DIR/email-cli" --batch show "$MSG_UID" 2>&1) || echo "email-cli show exit code: $?"
    echo "$OUT"
    SHOW_OUTPUT="$SHOW_OUTPUT $OUT"
done
echo ""

# 7. Verify FLAGS after email-cli operations (must be unchanged)
AFTER_FLAGS=$(flags_for_user)

# 8. Assertions
PASSED=0
FAILED=0

check() {
    local desc="$1"
    local pattern="$2"
    local text="$3"
    if echo "$text" | grep -q "$pattern"; then
        echo "  [PASS] $desc"
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] $desc  (expected pattern: '$pattern')"
        FAILED=$((FAILED + 1))
    fi
}

check_equal() {
    local desc="$1"
    local a="$2"
    local b="$3"
    if [ "$a" = "$b" ]; then
        echo "  [PASS] $desc"
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] $desc"
        echo "         BEFORE: $a"
        echo "         AFTER:  $b"
        FAILED=$((FAILED + 1))
    fi
}

ALL_OUTPUT="$LIST_OUTPUT $SHOW_OUTPUT"

echo "--- Assertions ---"
check "Messages listed"                  "message"      "$LIST_OUTPUT"
check "First test email in list"         "Test Email 1" "$LIST_OUTPUT"
check "Second test email in list"        "Test Email 2" "$LIST_OUTPUT"
check "Show output has message content"  "Test Email"   "$SHOW_OUTPUT"
check "Both seeded messages counted"    "2 message(s) in INBOX (2 unread)" "$LIST_OUTPUT"
check_equal "email-cli list exit code is 0" "0" "$LIST_RC"

echo ""
echo "--- CRITICAL: Read-only guarantee (FLAGS must not change) ---"
check "Baseline FLAGS were read from the server" "flags:" "$BEFORE_FLAGS"
check_equal "Message FLAGS unchanged after list+show" "$BEFORE_FLAGS" "$AFTER_FLAGS"

echo ""
echo "--- Integration Test Results ---"
echo "Passed: $PASSED / $((PASSED + FAILED))"

if [ "$FAILED" -gt 0 ]; then
    echo "INTEGRATION TEST FAILED"
    exit 1
fi

echo "INTEGRATION TEST PASSED"
echo ""
echo "NOTE: The FLAGS-unchanged assertion above is CRITICAL."
echo "      A failure there means email-cli is marking messages as read on the server."
