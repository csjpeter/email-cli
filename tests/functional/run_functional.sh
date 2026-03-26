#!/bin/bash

# email-cli Functional Test Runner
# Orchestrates mock server and client execution

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN_DIR="$PROJECT_ROOT/bin"
MOCK_SERVER_SRC="$PROJECT_ROOT/tests/functional/mock_imap_server.c"
MOCK_SERVER_BIN="$PROJECT_ROOT/build/tests/functional/mock_imap_server"

echo "--- email-cli Functional Tests ---"

# 1. Compile Mock Server
mkdir -p "$PROJECT_ROOT/build/tests/functional"
gcc "$MOCK_SERVER_SRC" -o "$MOCK_SERVER_BIN"

# 2. Start Mock Server in background
echo "Starting Mock IMAP Server..."
"$MOCK_SERVER_BIN" &
SERVER_PID=$!

# Ensure cleanup on exit
trap "kill $SERVER_PID || true" EXIT

# Give it a second to start
sleep 1

# 3. Create a temporary config for the test
TEST_HOME="/tmp/email-cli-func-test"
rm -rf "$TEST_HOME"
mkdir -p "$TEST_HOME/.config/email-cli"
cat <<EOF > "$TEST_HOME/.config/email-cli/config.ini"
EMAIL_HOST=imap://localhost:9993
EMAIL_USER=testuser
EMAIL_PASS=testpass
EMAIL_FOLDER=INBOX
EOF

export HOME="$TEST_HOME"

PASSED=0
FAILED=0

check() {
    local desc="$1"
    local pattern="$2"
    local output="$3"
    if echo "$output" | grep -q "$pattern"; then
        echo "  [PASS] $desc"
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] $desc  (expected pattern: '$pattern')"
        FAILED=$((FAILED + 1))
    fi
}

# 4. Run list mode (default)
echo "Running client (list mode)..."
LIST_OUTPUT=$("$BIN_DIR/email-cli" 2>&1)
echo "$LIST_OUTPUT"

echo ""
echo "--- List Mode Assertions ---"
check "Fetch header printed"      "Fetching recent emails"  "$LIST_OUTPUT"
check "Unread message count"      "unread message"          "$LIST_OUTPUT"
check "Table separator shown"     "═══"                     "$LIST_OUTPUT"
check "Subject in table"          "Test Message"            "$LIST_OUTPUT"
check "Successful completion"     "Success: Fetch complete" "$LIST_OUTPUT"

# 5. Run read mode
echo ""
echo "Running client (--read 1)..."
READ_OUTPUT=$("$BIN_DIR/email-cli" --read 1 2>&1)
echo "$READ_OUTPUT"

echo ""
echo "--- Read Mode Assertions ---"
check "From header shown"         "From:"                   "$READ_OUTPUT"
check "Subject header shown"      "Subject:"                "$READ_OUTPUT"
check "Email body shown"          "Hello from Mock Server"  "$READ_OUTPUT"
check "Successful completion"     "Success: Fetch complete" "$READ_OUTPUT"

echo ""
echo "--- Functional Test Results ---"
echo "Passed: $PASSED / $((PASSED + FAILED))"

if [ "$FAILED" -gt 0 ]; then
    echo "FUNCTIONAL TEST FAILED"
    exit 1
fi

echo "Functional test complete."
