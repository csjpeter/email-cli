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

# 2. Start Mock Server in background, capturing its stdout for later assertions
echo "Starting Mock IMAP Server..."
MOCK_LOG="$PROJECT_ROOT/build/tests/functional/mock_server.log"
"$MOCK_SERVER_BIN" >"$MOCK_LOG" 2>&1 &
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
# Ensure XDG overrides do not redirect the binary to a different config/cache path
unset XDG_CONFIG_HOME XDG_CACHE_HOME

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

check_not() {
    local desc="$1"
    local pattern="$2"
    local text="$3"
    if ! echo "$text" | grep -q "$pattern"; then
        echo "  [PASS] $desc"
        PASSED=$((PASSED + 1))
    else
        echo "  [FAIL] $desc  (unexpected pattern found: '$pattern')"
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

# 7. Test: list
echo ""
echo "Running: email-cli list ..."
LIST_OUTPUT=$("$BIN_DIR/email-cli" list 2>&1 || true)
echo "$LIST_OUTPUT"
echo "--- List Assertions ---"
check "Message count shown"       "message(s) in"           "$LIST_OUTPUT"
check "Unread count shown"        "unread"                  "$LIST_OUTPUT"
check "Table separator shown"     "═══"                     "$LIST_OUTPUT"
check "Subject in table"          "Test Message"            "$LIST_OUTPUT"
check "Successful completion"     "Success: Fetch complete" "$LIST_OUTPUT"

# 8. Test: list --all (same behavior — all messages always shown)
echo ""
echo "Running: email-cli list --all ..."
ALL_OUTPUT=$("$BIN_DIR/email-cli" list --all 2>&1 || true)
echo "$ALL_OUTPUT"
echo "--- List --all Assertions ---"
check "All mode: message count"   "message(s) in"           "$ALL_OUTPUT"
check "All mode: unread count"    "unread"                  "$ALL_OUTPUT"
check "All mode: subject shown"   "Test Message"            "$ALL_OUTPUT"

# 9. Test: list --folder
echo ""
echo "Running: email-cli list --folder INBOX ..."
FOLDER_OUTPUT=$("$BIN_DIR/email-cli" list --folder INBOX 2>&1 || true)
check "Folder override used"      "INBOX"                   "$FOLDER_OUTPUT"

# 10. Test: show command
echo ""
echo "Running: email-cli show 1 ..."
SHOW_OUTPUT=$("$BIN_DIR/email-cli" show 1 2>&1 || true)
echo "$SHOW_OUTPUT"
echo "--- Show Assertions ---"
check "From header shown"         "From:"                   "$SHOW_OUTPUT"
check "Subject header shown"      "Subject:"                "$SHOW_OUTPUT"
check "Email body shown"          "Hello from Mock Server"  "$SHOW_OUTPUT"
check "Successful completion"     "Success: Fetch complete" "$SHOW_OUTPUT"
check_not "Show: no CSS in output (color)"     "color"      "$SHOW_OUTPUT"
check_not "Show: no CSS in output (font-size)" "font-size"  "$SHOW_OUTPUT"

# 11. Test: folders (flat)
echo ""
echo "Running: email-cli folders ..."
FOLDERS_OUTPUT=$("$BIN_DIR/email-cli" folders 2>&1 || true)
echo "$FOLDERS_OUTPUT"
echo "--- Folders Assertions ---"
check "Folders: INBOX listed"     "INBOX"                   "$FOLDERS_OUTPUT"
check "Folders: subfolder listed" "INBOX.Sent"              "$FOLDERS_OUTPUT"

# 12. Test: folders --tree
echo ""
echo "Running: email-cli folders --tree ..."
TREE_OUTPUT=$("$BIN_DIR/email-cli" folders --tree 2>&1 || true)
echo "$TREE_OUTPUT"
echo "--- Folders Tree Assertions ---"
check "Tree: has branch char"     "──"                      "$TREE_OUTPUT"
check "Tree: INBOX shown"         "INBOX"                   "$TREE_OUTPUT"
check "Tree: Sent shown"          "Sent"                    "$TREE_OUTPUT"

# 13. Test: empty folder (batch mode)
echo ""
echo "Running: email-cli list --folder INBOX.Empty --batch ..."
EMPTY_OUTPUT=$("$BIN_DIR/email-cli" list --folder INBOX.Empty --batch 2>&1 || true)
echo "$EMPTY_OUTPUT"
echo "--- Empty folder assertions ---"
check "Empty folder: no messages msg" "No messages"  "$EMPTY_OUTPUT"
check "Empty folder: exit success"    "Success"       "$EMPTY_OUTPUT"

# CRITICAL: verify email-cli never issued a STORE (flag-modification) command
MOCK_CMDS=$(cat "$MOCK_LOG" 2>/dev/null || true)
echo ""
echo "--- CRITICAL: Read-only guarantee (no STORE commands issued) ---"
check_not "No STORE command sent to server" "STORE" "$MOCK_CMDS"
echo "(mock server log: $MOCK_LOG)"

echo ""
echo "--- Functional Test Results ---"
echo "Passed: $PASSED / $((PASSED + FAILED))"

if [ "$FAILED" -gt 0 ]; then
    echo "FUNCTIONAL TEST FAILED"
    exit 1
fi

echo "Functional test complete."
