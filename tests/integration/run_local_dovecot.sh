#!/bin/bash
# Integration test: IMAP APPEND via locally running Dovecot (no Docker).
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
#
# Usage:
#   ./tests/integration/run_local_dovecot.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

IMAP_PORT=14993
TEST_USER="itest-append"
TEST_PASS="testpass123"
CURRENT_UID="$(id -u)"
CURRENT_GID="$(id -g)"

TMPDIR_BASE="$(mktemp -d /tmp/dovecot-itest-XXXXXX)"
DOVE_BASE="$TMPDIR_BASE/run"           # Dovecot runtime (pids, sockets)
MAIL_HOME="$TMPDIR_BASE/mail"          # Maildir root for test user
CERTS_DIR="$TMPDIR_BASE/certs"
EMAIL_HOME="$TMPDIR_BASE/home"         # Fake HOME for email-cli

PASSED=0
FAILED=0

# ── Helpers ──────────────────────────────────────────────────────────────────

ok()   { echo "  [PASS] $*"; PASSED=$((PASSED+1)); }
fail() { echo "  [FAIL] $*"; FAILED=$((FAILED+1)); }

check_contains() {
    local desc="$1" pattern="$2" text="$3"
    if echo "$text" | grep -q "$pattern"; then ok "$desc"; else fail "$desc (pattern: '$pattern')"; fi
}

dovecot_stop() {
    local pid_file="$DOVE_BASE/master.pid"
    if sudo test -f "$pid_file" 2>/dev/null; then
        local pid
        pid="$(sudo cat "$pid_file")"
        sudo kill "$pid" 2>/dev/null || true
        local i=0
        while sudo kill -0 "$pid" 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done
        sudo kill -9 "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    dovecot_stop
    sudo rm -rf "/tmp/dovecot-itest-${TMPDIR_BASE##*/tmp/dovecot-itest-}"
}
trap cleanup EXIT

# ── Prerequisites ─────────────────────────────────────────────────────────────

if ! command -v dovecot >/dev/null 2>&1; then
    echo "ERROR: dovecot not installed.  Run: sudo apt-get install -y dovecot-imapd"
    exit 1
fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "ERROR: openssl not found."
    exit 1
fi
if ! sudo -n true 2>/dev/null; then
    echo "ERROR: sudo without password required (add NOPASSWD entry for this user)."
    exit 1
fi

echo "=== email-cli Local Dovecot Integration Test (APPEND / TRYCREATE) ==="
echo ""

# ── Build if needed ───────────────────────────────────────────────────────────

if [ ! -f "$PROJECT_ROOT/bin/email-sync" ]; then
    echo "Building email-sync..."
    (cd "$PROJECT_ROOT" && ./manage.sh build)
fi

# ── Certificates ──────────────────────────────────────────────────────────────

mkdir -p "$DOVE_BASE" "$MAIL_HOME" "$CERTS_DIR" "$EMAIL_HOME"
# Dovecot auth/imap daemons run as system user 'dovecot' (uid≠current user)
# and must be able to enter the temp directory.
chmod 755 "$TMPDIR_BASE" "$DOVE_BASE" "$MAIL_HOME" "$CERTS_DIR"

openssl req -x509 -nodes -days 1 -newkey rsa:2048 \
    -keyout "$CERTS_DIR/server.key" \
    -out    "$CERTS_DIR/server.crt" \
    -subj   "/CN=localhost" 2>/dev/null
chmod 644 "$CERTS_DIR/server.crt"
chmod 640 "$CERTS_DIR/server.key"

# ── Dovecot configuration ─────────────────────────────────────────────────────

# passwd-file format: user:password:uid:gid::home
# We reuse the current user's uid/gid so Dovecot imap processes can write
# to mail directories owned by the current user.
echo "$TEST_USER:{PLAIN}$TEST_PASS:$CURRENT_UID:$CURRENT_GID::$MAIL_HOME" \
    > "$TMPDIR_BASE/users"
chmod 644 "$TMPDIR_BASE/users"

cat > "$TMPDIR_BASE/dovecot.conf" <<DOVECONF
protocols = imap
listen = 127.0.0.1

base_dir = $DOVE_BASE
state_dir = $DOVE_BASE

log_path = $TMPDIR_BASE/dovecot.log
info_log_path = $TMPDIR_BASE/dovecot.log

ssl = yes
ssl_cert = <$CERTS_DIR/server.crt
ssl_key  = <$CERTS_DIR/server.key
ssl_min_protocol = TLSv1.2

disable_plaintext_auth = no
auth_mechanisms = plain

mail_location = maildir:$MAIL_HOME

passdb {
  driver = passwd-file
  args = scheme=PLAIN $TMPDIR_BASE/users
}
userdb {
  driver = passwd-file
  args = $TMPDIR_BASE/users
}
service imap-login {
  inet_listener imap {
    port = 0
  }
  inet_listener imaps {
    address = 127.0.0.1
    port = $IMAP_PORT
    ssl = yes
  }
}
DOVECONF

# ── Start Dovecot ─────────────────────────────────────────────────────────────

echo "Starting Dovecot on 127.0.0.1:$IMAP_PORT ..."
sudo dovecot -c "$TMPDIR_BASE/dovecot.conf"

# Wait until the port is ready (max 10 s)
for i in $(seq 1 50); do
    if nc -z 127.0.0.1 "$IMAP_PORT" 2>/dev/null; then
        echo "Dovecot ready."
        break
    fi
    sleep 0.2
    if [ "$i" -eq 50 ]; then
        echo "ERROR: Dovecot did not start in time."
        cat "$TMPDIR_BASE/dovecot.log" 2>/dev/null || true
        exit 1
    fi
done

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
    sudo tail -50 "$TMPDIR_BASE/dovecot.log" 2>/dev/null || true
    echo ""
    echo "LOCAL DOVECOT INTEGRATION TEST FAILED"
    exit 1
fi

echo "LOCAL DOVECOT INTEGRATION TEST PASSED"
