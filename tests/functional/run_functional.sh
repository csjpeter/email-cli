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

# 4. Test: help (no args)
echo "Running: email-cli (no args) ..."
HELP_OUTPUT=$("$BIN_DIR/email-cli" 2>&1 || true)
echo "$HELP_OUTPUT"
echo "--- Help Assertions ---"
check "General help: list cmd"    "list"                    "$HELP_OUTPUT"
check "General help: show cmd"    "show"                    "$HELP_OUTPUT"
check "General help: help cmd"    "help"                    "$HELP_OUTPUT"

# 5. Test: help list
echo ""
HELP_LIST=$("$BIN_DIR/email-cli" help list 2>&1)
echo "--- help list ---"
check "help list: usage line"     "email-cli list"          "$HELP_LIST"

# 6. Test: help show
HELP_SHOW=$("$BIN_DIR/email-cli" help show 2>&1)
echo "--- help show ---"
check "help show: usage line"     "email-cli show"          "$HELP_SHOW"

# 7. Test: list (unread only)
echo ""
echo "Running: email-cli list ..."
LIST_OUTPUT=$("$BIN_DIR/email-cli" list 2>&1)
echo "$LIST_OUTPUT"
echo "--- List Assertions ---"
check "Fetch header printed"      "Fetching emails"         "$LIST_OUTPUT"
check "Unread message count"      "unread message"          "$LIST_OUTPUT"
check "Table separator shown"     "═══"                     "$LIST_OUTPUT"
check "Subject in table"          "Test Message"            "$LIST_OUTPUT"
check "Successful completion"     "Success: Fetch complete" "$LIST_OUTPUT"

# 8. Test: list --all
echo ""
echo "Running: email-cli list --all ..."
ALL_OUTPUT=$("$BIN_DIR/email-cli" list --all 2>&1)
echo "$ALL_OUTPUT"
echo "--- List --all Assertions ---"
check "All mode: message count"   "message(s) in"           "$ALL_OUTPUT"
check "All mode: unread marker N" " N "                     "$ALL_OUTPUT"
check "All mode: subject shown"   "Test Message"            "$ALL_OUTPUT"

# 9. Test: list --folder
echo ""
echo "Running: email-cli list --folder INBOX ..."
FOLDER_OUTPUT=$("$BIN_DIR/email-cli" list --folder INBOX 2>&1)
check "Folder override used"      "INBOX"                   "$FOLDER_OUTPUT"

# 10. Test: show command
echo ""
echo "Running: email-cli show 1 ..."
SHOW_OUTPUT=$("$BIN_DIR/email-cli" show 1 2>&1)
echo "$SHOW_OUTPUT"
echo "--- Show Assertions ---"
check "From header shown"         "From:"                   "$SHOW_OUTPUT"
check "Subject header shown"      "Subject:"                "$SHOW_OUTPUT"
check "Email body shown"          "Hello from Mock Server"  "$SHOW_OUTPUT"
check "Successful completion"     "Success: Fetch complete" "$SHOW_OUTPUT"

# 11. Test: folders (flat)
echo ""
echo "Running: email-cli folders ..."
FOLDERS_OUTPUT=$("$BIN_DIR/email-cli" folders 2>&1)
echo "$FOLDERS_OUTPUT"
echo "--- Folders Assertions ---"
check "Folders: INBOX listed"     "INBOX"                   "$FOLDERS_OUTPUT"
check "Folders: subfolder listed" "INBOX.Sent"              "$FOLDERS_OUTPUT"

# 12. Test: folders --tree
echo ""
echo "Running: email-cli folders --tree ..."
TREE_OUTPUT=$("$BIN_DIR/email-cli" folders --tree 2>&1)
echo "$TREE_OUTPUT"
echo "--- Folders Tree Assertions ---"
check "Tree: has branch char"     "──"                      "$TREE_OUTPUT"
check "Tree: INBOX shown"         "INBOX"                   "$TREE_OUTPUT"
check "Tree: Sent shown"          "Sent"                    "$TREE_OUTPUT"

echo ""
echo "--- Functional Test Results ---"
echo "Passed: $PASSED / $((PASSED + FAILED))"

if [ "$FAILED" -gt 0 ]; then
    echo "FUNCTIONAL TEST FAILED"
    exit 1
fi

echo "Functional test complete."
