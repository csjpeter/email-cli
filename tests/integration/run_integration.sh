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
# Environment lifecycle:
#   - Container already running  → reused (fast)
#   - Container stopped, volume exists → restarted in seconds (emails preserved)
#   - First run ever             → full build + start (~30-60s)
#
# Usage:
#   ./run_integration.sh            # run test (start env if needed)
#   ./run_integration.sh --down     # stop containers (keeps email volume)
#   ./run_integration.sh --clean    # stop containers AND remove volume

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
COMPOSE="docker compose -f $SCRIPT_DIR/docker-compose.yml"
CONTAINER="email-cli-imap-test"

# TLS connection – self-signed cert, so verify is disabled
# The container's IMAPS port is published on a loopback port the kernel picks
# (docker-compose.yml); ensure_env_running leaves it in IMAP_HOST.
IMAP_HOST=""
IMAP_USER="testuser"
IMAP_PASS="testpass"
IMAP_FOLDER="INBOX"

# ── Lifecycle helpers ────────────────────────────────────────────────────

start_env() {
    echo "Building and starting Dovecot IMAP/TLS test server..."
    $COMPOSE up -d --build
    wait_for_imaps
}

wait_for_imaps() {
    echo "Waiting for IMAPS on port 993..."
    for i in $(seq 1 30); do
        if docker exec "$CONTAINER" nc -z localhost 993; then
            echo "IMAPS server is ready."
            return 0
        fi
        sleep 1
    done
    echo "ERROR: IMAPS server did not become ready in time."
    docker logs "$CONTAINER" | tail -20
    exit 1
}

ensure_env_running() {
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${CONTAINER}$"; then
        echo "Reusing running IMAP test server."
    elif docker ps -a --format '{{.Names}}' 2>/dev/null | grep -q "^${CONTAINER}$"; then
        echo "Restarting stopped IMAP test server..."
        $COMPOSE start
        wait_for_imaps
    else
        start_env
    fi
}

# ── Subcommands ──────────────────────────────────────────────────────────

case "${1:-}" in
    --down)
        echo "Stopping integration test environment..."
        $COMPOSE stop
        echo "Done. Volume 'email-cli-maildata' preserved."
        exit 0
        ;;
    --clean)
        echo "Removing integration test environment and volume..."
        $COMPOSE down -v
        echo "Done."
        exit 0
        ;;
esac

# ── Run integration test ─────────────────────────────────────────────────

echo "=== email-cli Integration Test (Dovecot IMAPS / TLS 1.2+) ==="
echo ""

# 1. Build release binary if needed
if [ ! -f "$PROJECT_ROOT/bin/email-cli" ]; then
    echo "Binary not found. Building..."
    (cd "$PROJECT_ROOT" && ./manage.sh build)
fi

# 2. Start or reuse the IMAP/TLS server
ensure_env_running

# 3. Write temporary config pointing at the TLS server.  The home lives under
#    build/ with this shell's pid in its name; nothing is written to the system temp directory.
HOST_MAPPING="$(docker port "$CONTAINER" 993/tcp | head -n 1)"
IMAP_PORT="${HOST_MAPPING##*:}"
case "$IMAP_PORT" in
    ''|*[!0-9]*) echo "ERROR: no published IMAPS port for $CONTAINER ('$HOST_MAPPING')" >&2; exit 1 ;;
esac
IMAP_HOST="imaps://localhost:$IMAP_PORT"
echo "IMAPS is published on $IMAP_HOST"

TEST_HOME="$PROJECT_ROOT/build/tests/integration/work-$$"
mkdir -p "$TEST_HOME"
cleanup_home() {
    case "$TEST_HOME" in
        "$PROJECT_ROOT"/build/tests/integration/work-*) ;;
        *) echo "ERROR: refusing unsafe path '$TEST_HOME'" >&2; return 1 ;;
    esac
    rm -rf "$PROJECT_ROOT/build/tests/integration/work-${TEST_HOME##*/work-}"
}
trap cleanup_home EXIT
mkdir -p "$TEST_HOME/.config/email-cli"
cat > "$TEST_HOME/.config/email-cli/config.ini" <<EOF
EMAIL_HOST=$IMAP_HOST
EMAIL_USER=$IMAP_USER
EMAIL_PASS=$IMAP_PASS
EMAIL_FOLDER=$IMAP_FOLDER
SSL_NO_VERIFY=1
EOF

# Helper: query FLAGS for all messages in INBOX via doveadm inside the container
flags_for_user() {
    docker exec "$CONTAINER" doveadm fetch -u "$IMAP_USER" "flags" mailbox INBOX ALL 2>/dev/null
}

# 4. Verify FLAGS before any email-cli operation (baseline)
BEFORE_FLAGS=$(flags_for_user)

# 5. Run email-cli list (batch) and capture output
echo "Running: email-cli list --batch ..."
LIST_OUTPUT=$(HOME="$TEST_HOME" "$PROJECT_ROOT/bin/email-cli" list --batch 2>&1)
echo "$LIST_OUTPUT"
echo ""

# 6. Run email-cli show for each UID found in list output
UIDS=$(echo "$LIST_OUTPUT" | grep -Eo '^[[:space:]]+[0-9]+' | tr -d ' ' | head -5)
SHOW_OUTPUT=""
for UID in $UIDS; do
    echo "Running: email-cli show $UID ..."
    OUT=$(HOME="$TEST_HOME" "$PROJECT_ROOT/bin/email-cli" --batch show "$UID" 2>&1)
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
check "Successful list completion"       "Fetch complete" "$LIST_OUTPUT"

echo ""
echo "--- CRITICAL: Read-only guarantee (FLAGS must not change) ---"
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
