#!/bin/bash
# Integration test: email-cli against a real Dovecot IMAP server in Docker.
#
# Environment lifecycle:
#   - Container already running  → reused (fast)
#   - Container stopped, volume exists → restarted in seconds (emails preserved)
#   - First run ever             → full build + start (~30s)
#
# Usage:
#   ./run_integration.sh            # run test (start env if needed)
#   ./run_integration.sh --down     # stop and remove containers (keeps volume)
#   ./run_integration.sh --clean    # stop, remove containers AND volume

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
COMPOSE="docker compose -f $SCRIPT_DIR/docker-compose.yml"
CONTAINER="email-cli-imap-test"
IMAP_HOST="imap://localhost:143"
IMAP_USER="testuser"
IMAP_PASS="testpass"
IMAP_FOLDER="INBOX"

# --- Lifecycle helpers ---

start_env() {
    echo "Starting Dovecot IMAP test server..."
    $COMPOSE up -d --build
    wait_for_imap
}

wait_for_imap() {
    echo "Waiting for IMAP server on port 143..."
    for i in $(seq 1 30); do
        if docker exec "$CONTAINER" nc -z localhost 143 2>/dev/null; then
            echo "IMAP server is ready."
            return 0
        fi
        sleep 1
    done
    echo "ERROR: IMAP server did not become ready in time."
    docker logs "$CONTAINER" | tail -20
    exit 1
}

ensure_env_running() {
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${CONTAINER}$"; then
        echo "Reusing running IMAP test server."
    elif docker ps -a --format '{{.Names}}' 2>/dev/null | grep -q "^${CONTAINER}$"; then
        echo "Restarting stopped IMAP test server..."
        $COMPOSE start
        wait_for_imap
    else
        start_env
    fi
}

# --- Subcommands ---

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

# --- Run integration test ---

echo "--- email-cli Integration Test (Dovecot IMAP) ---"
echo ""

# 1. Make sure the binary is built
if [ ! -f "$PROJECT_ROOT/bin/email-cli" ]; then
    echo "Binary not found. Building..."
    (cd "$PROJECT_ROOT" && ./manage.sh build)
fi

# 2. Ensure the IMAP server is up
ensure_env_running

# 3. Write temporary config pointing at the test server
TEST_HOME="$(mktemp -d)"
trap "rm -rf $TEST_HOME" EXIT
mkdir -p "$TEST_HOME/.config/email-cli"
cat > "$TEST_HOME/.config/email-cli/config.ini" <<EOF
EMAIL_HOST=$IMAP_HOST
EMAIL_USER=$IMAP_USER
EMAIL_PASS=$IMAP_PASS
EMAIL_FOLDER=$IMAP_FOLDER
EOF

# 4. Run email-cli and capture output
echo "Connecting to $IMAP_HOST as $IMAP_USER..."
OUTPUT=$(HOME="$TEST_HOME" "$PROJECT_ROOT/bin/email-cli" 2>&1)
echo "$OUTPUT"
echo ""

# 5. Verify output
PASSED=0
FAILED=0

check() {
    local desc="$1"
    local pattern="$2"
    if echo "$OUTPUT" | grep -q "$pattern"; then
        echo "  [PASS] $desc"
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] $desc (expected: '$pattern')"
        FAILED=$((FAILED + 1))
    fi
}

echo "--- Assertions ---"
check "Fetch header printed"         "Fetching recent emails"
check "No CURL error in output"      "Fetch complete"
check "Integration Test Email found" "Integration Test Email"

echo ""
echo "--- Integration Test Results ---"
echo "Passed: $PASSED"
echo "Failed: $FAILED"

if [ "$FAILED" -gt 0 ]; then
    echo "INTEGRATION TEST FAILED"
    exit 1
fi

echo "INTEGRATION TEST PASSED"
