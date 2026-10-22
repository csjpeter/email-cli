#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-04-30
# Integration test of IMAP APPEND against a locally running Dovecot.

# Integration test: IMAP APPEND via locally running Dovecot (no container).
#
# What this tests:
#   - email-sync uploads a pending message to the server's "Sent" folder
#   - If "Sent" doesn't exist yet, email-cli creates it automatically (TRYCREATE)
#   - After a successful sync the pending_appends.tsv entry is removed
#
# Requirements:
#   - dovecot-imapd installed  (sudo apt-get install -y dovecot-imapd)
#   - sudo available for starting/stopping the Dovecot process
#   - openssl for generating a self-signed test certificate
#   - the Dovecot setup is shared with run_integration.sh (dovecot_env.sh)
#
# Usage:
#   ./tests/integration/run_local_dovecot.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TEST_USER="itest-append"
TEST_PASS="testpass123"

. "$SCRIPT_DIR/dovecot_env.sh"
dovecot_env_init "$TEST_USER" "$TEST_PASS"
trap dovecot_env_cleanup EXIT
EMAIL_HOME="$WORK_DIR/home"            # Fake HOME for email-cli
mkdir -p "$EMAIL_HOME"

PASSED=0
FAILED=0

# ── Helpers ──────────────────────────────────────────────────────────────────

ok()   { echo "  [PASS] $*"; PASSED=$((PASSED+1)); }
fail() { echo "  [FAIL] $*"; FAILED=$((FAILED+1)); }

check_contains() {
    local desc="$1" pattern="$2" text="$3"
    if echo "$text" | grep -q "$pattern"; then ok "$desc"; else fail "$desc (pattern: '$pattern')"; fi
}

echo "=== email-cli Local Dovecot Integration Test (APPEND / TRYCREATE) ==="
echo ""

# ── Build if needed ───────────────────────────────────────────────────────────

if [ ! -f "$PROJECT_ROOT/bin/email-sync" ]; then
    echo "Building email-sync..."
    (cd "$PROJECT_ROOT" && ./manage.sh build)
fi

# ── Start Dovecot ─────────────────────────────────────────────────────────────

dovecot_env_start

# ── email-cli account setup ───────────────────────────────────────────────────

# Create a HOME directory structure that email-cli will use via HOME=...
# Config path: ~/.config/email-cli/accounts/<user>/config.ini
mkdir -p "$EMAIL_HOME/.config/email-cli/accounts/$TEST_USER" \
         "$EMAIL_HOME/.cache/email-cli/logs"

cat > "$EMAIL_HOME/.config/email-cli/accounts/$TEST_USER/config.ini" <<CFG
EMAIL_HOST=imaps://127.0.0.1:$IMAP_PORT
EMAIL_USER=$TEST_USER
EMAIL_PASS=$TEST_PASS
EMAIL_FOLDER=INBOX
SSL_NO_VERIFY=1
CFG
chmod 600 "$EMAIL_HOME/.config/email-cli/accounts/$TEST_USER/config.ini"

# ── Local store: create a pending outgoing message ────────────────────────────

# Account dir: ~/.local/share/email-cli/accounts/<user>/
# Message path: <account>/store/<folder>/<last_digit(uid)>/<2nd_last_digit(uid)>/<uid>.eml
TEST_UID="t0000000001"
# digit1 = last char of uid = '1', digit2 = second-to-last = '0'
ACCT_DIR="$EMAIL_HOME/.local/share/email-cli/accounts/$TEST_USER"
MSG_DIR="$ACCT_DIR/store/Sent/1/0"
mkdir -p "$MSG_DIR"

cat > "$MSG_DIR/$TEST_UID.eml" <<'EML'
From: test@example.com
To: recipient@example.com
Subject: Integration test email
Date: Thu, 30 Apr 2026 12:00:00 +0000
Message-ID: <itest-001@localhost>
MIME-Version: 1.0
Content-Type: text/plain

This is the integration test message body.
EML

# pending_appends.tsv: folder<TAB>uid
printf "Sent\t%s\n" "$TEST_UID" > "$ACCT_DIR/pending_appends.tsv"

echo ""
echo "--- Running email-sync ---"
SYNC_OUT=$(HOME="$EMAIL_HOME" "$PROJECT_ROOT/bin/email-sync" 2>&1 || true)
echo "$SYNC_OUT"
echo ""
echo "--- Sync log (APPEND-related) ---"
grep -E "APPEND|TRYCREATE|CREATE|failed|ERROR|WARN|IN\].*BAD|IN\].*NO|IN\].*OK A0" \
    "$EMAIL_HOME/.cache/email-cli/logs/sync-session.log" 2>/dev/null | head -30 || true
echo ""

# ── Assertions ────────────────────────────────────────────────────────────────

echo "--- Assertions ---"

# 1. Sync output says "Uploading"
check_contains "Sync reports pending message" "Uploading 1 pending" "$SYNC_OUT"

# 2. No failure message
if echo "$SYNC_OUT" | grep -q "failed (retry"; then
    fail "Upload should not fail"
else
    ok "Upload did not report failure"
fi

# 3. pending_appends.tsv should be empty or removed after success
REMAINING=$(cat "$ACCT_DIR/pending_appends.tsv" 2>/dev/null | grep -c "$TEST_UID" || true)
if [ "$REMAINING" -eq 0 ]; then
    ok "pending_appends.tsv entry removed after upload"
else
    fail "pending_appends.tsv still contains the entry (upload did not complete)"
fi

# 4. Dovecot's Sent maildir should exist and contain a message
SENT_DIR="$MAIL_HOME/.Sent"
if [ -d "$SENT_DIR/new" ] || [ -d "$SENT_DIR/cur" ]; then
    MSG_COUNT=$(find "$SENT_DIR" -type f | wc -l)
    if [ "$MSG_COUNT" -gt 0 ]; then
        ok "Sent folder created on server with $MSG_COUNT message(s)"
    else
        fail "Sent folder exists but is empty"
    fi
else
    fail "Sent folder not created on server (expected $SENT_DIR)"
    echo "       Mail home contents:"
    find "$MAIL_HOME" -type d | head -20 || true
fi

# 5. The appended message contains the expected subject
if find "$MAIL_HOME" -type f | xargs grep -l "Integration test email" 2>/dev/null | grep -q .; then
    ok "Appended message contains expected subject"
else
    fail "Expected subject not found in server-side message"
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "--- Local Dovecot Integration Test Results ---"
echo "Passed: $PASSED / $((PASSED + FAILED))"

if [ "$FAILED" -gt 0 ]; then
    echo ""
    echo "Dovecot log:"
    sudo tail -50 "$WORK_DIR/dovecot.log" 2>/dev/null || true
    echo ""
    echo "LOCAL DOVECOT INTEGRATION TEST FAILED"
    exit 1
fi

echo "LOCAL DOVECOT INTEGRATION TEST PASSED"
