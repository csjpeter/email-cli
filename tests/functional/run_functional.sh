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
# We bypass the wizard by providing a config.ini
TEST_HOME="/tmp/email-cli-func-test"
rm -rf "$TEST_HOME"
mkdir -p "$TEST_HOME/.config/email-cli"
cat <<EOF > "$TEST_HOME/.config/email-cli/config.ini"
EMAIL_HOST=imap://localhost:9993
EMAIL_USER=testuser
EMAIL_PASS=testpass
EMAIL_FOLDER=INBOX
EOF

# 4. Run the client
echo "Running client..."
export HOME="$TEST_HOME"
# We expect the client to fetch message with UID 1 from our mock server
# Note: we use plain imap:// since mock server is simple TCP
"$BIN_DIR/email-cli"

echo "Functional test complete."
