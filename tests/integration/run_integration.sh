#!/bin/bash
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
IMAP_HOST="imaps://localhost:993"
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
        if docker exec "$CONTAINER" nc -z localhost 993 2>/dev/null; then
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

# 3. Write temporary config pointing at the TLS server
TEST_HOME="$(mktemp -d)"
trap "rm -rf $TEST_HOME" EXIT
mkdir -p "$TEST_HOME/.config/email-cli"
cat > "$TEST_HOME/.config/email-cli/config.ini" <<EOF
EMAIL_HOST=$IMAP_HOST
EMAIL_USER=$IMAP_USER
EMAIL_PASS=$IMAP_PASS
EMAIL_FOLDER=$IMAP_FOLDER
SSL_NO_VERIFY=1
EOF

# 4. Run email-cli and capture output
echo "Connecting to $IMAP_HOST as $IMAP_USER (TLS, self-signed cert)..."
echo ""
OUTPUT=$(HOME="$TEST_HOME" "$PROJECT_ROOT/bin/email-cli" 2>&1)
echo "$OUTPUT"
echo ""

# 5. Assertions
PASSED=0
FAILED=0

check() {
    local desc="$1"
    local pattern="$2"
    if echo "$OUTPUT" | grep -q "$pattern"; then
        echo "  [PASS] $desc"
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] $desc  (expected pattern: '$pattern')"
        FAILED=$((FAILED + 1))
    fi
}

echo "--- Assertions ---"
check "Fetch header printed"              "Fetching recent emails"
check "Messages found in mailbox"        "message"
check "First test email content found"   "Integration Test Email 1"
check "Second test email content found"  "Integration Test Email 2"
check "Successful completion"            "Fetch complete"

echo ""
echo "--- Integration Test Results ---"
echo "Passed: $PASSED / $((PASSED + FAILED))"

if [ "$FAILED" -gt 0 ]; then
    echo "INTEGRATION TEST FAILED"
    exit 1
fi

echo "INTEGRATION TEST PASSED"
