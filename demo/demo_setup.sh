#!/usr/bin/env bash
# demo/demo_setup.sh
# Starts mock IMAP + SMTP servers and sets up a demo HOME directory.
# Source this script in the VHS tape session:  source demo/demo_setup.sh
#
# After sourcing: HOME=/tmp/email-cli-demo-home, servers running on
#   IMAP 9993 (TLS, self-signed), SMTP 9465 (TLS, self-signed)

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="$PROJECT_ROOT/bin"
BUILD_DIR="$PROJECT_ROOT/build"

MOCK_IMAP_SRC="$PROJECT_ROOT/tests/functional/mock_imap_server.c"
MOCK_SMTP_SRC="$PROJECT_ROOT/tests/pty/mock_smtp_server.c"
MOCK_IMAP_BIN="$BUILD_DIR/tests/functional/mock_imap_server"
MOCK_SMTP_BIN="$BUILD_DIR/tests/functional/mock_smtp_server"

DEMO_HOME="/tmp/email-cli-demo-home"
DEMO_ACCOUNT="alice@acme.example"
IMAP_PORT=9993
SMTP_PORT=9465

# ── TLS certificates ────────────────────────────────────────────────────────
TEST_CERT="$BUILD_DIR/tests/certs/test.crt"
TEST_KEY="$BUILD_DIR/tests/certs/test.key"
if [ ! -f "$TEST_CERT" ] || [ ! -f "$TEST_KEY" ]; then
    echo "[demo] Generating test TLS certificates..."
    mkdir -p "$BUILD_DIR/tests/certs"
    openssl req -x509 -newkey rsa:2048 \
        -keyout "$TEST_KEY" -out "$TEST_CERT" \
        -days 3650 -nodes -subj "/CN=localhost" 2>/dev/null
fi

# ── Build mock servers ───────────────────────────────────────────────────────
mkdir -p "$BUILD_DIR/tests/functional"
if [ ! -f "$MOCK_IMAP_BIN" ]; then
    echo "[demo] Building mock IMAP server..."
    gcc "$MOCK_IMAP_SRC" -o "$MOCK_IMAP_BIN" -lssl -lcrypto
fi
if [ ! -f "$MOCK_SMTP_BIN" ]; then
    echo "[demo] Building mock SMTP server..."
    gcc "$MOCK_SMTP_SRC" -o "$MOCK_SMTP_BIN" -lssl -lcrypto
fi

# ── Kill stale servers ───────────────────────────────────────────────────────
pkill -f "mock_imap_server" 2>/dev/null || true
pkill -f "mock_smtp_server" 2>/dev/null || true
sleep 0.3

# ── Start mock IMAP server ───────────────────────────────────────────────────
echo "[demo] Starting mock IMAP server on port $IMAP_PORT..."
(cd "$BUILD_DIR" && \
    MOCK_IMAP_PORT=$IMAP_PORT \
    MOCK_IMAP_SUBJECT="Project Kickoff — Tuesday 10am" \
    "$MOCK_IMAP_BIN") >/tmp/email-cli-demo-imap.log 2>&1 &
DEMO_IMAP_PID=$!
export DEMO_IMAP_PID

# ── Start mock SMTP server ───────────────────────────────────────────────────
echo "[demo] Starting mock SMTP server on port $SMTP_PORT..."
(cd "$BUILD_DIR" && \
    MOCK_SMTP_PORT=$SMTP_PORT \
    "$MOCK_SMTP_BIN") >/tmp/email-cli-demo-smtp.log 2>&1 &
DEMO_SMTP_PID=$!
export DEMO_SMTP_PID

sleep 0.8

# ── Create demo HOME ─────────────────────────────────────────────────────────
rm -rf "$DEMO_HOME"
mkdir -p "$DEMO_HOME/.config/email-cli/accounts/$DEMO_ACCOUNT"
cat > "$DEMO_HOME/.config/email-cli/accounts/$DEMO_ACCOUNT/config.ini" <<CONFIG
EMAIL_HOST=imaps://localhost:$IMAP_PORT
EMAIL_USER=$DEMO_ACCOUNT
EMAIL_PASS=demopass
EMAIL_FOLDER=INBOX
EMAIL_SENT_FOLDER=INBOX.Sent
SSL_NO_VERIFY=1
SMTP_HOST=smtps://localhost:$SMTP_PORT
SMTP_USER=$DEMO_ACCOUNT
SMTP_PASS=demopass
CONFIG

# ── Export environment for the demo session ──────────────────────────────────
export HOME="$DEMO_HOME"
export XDG_CONFIG_HOME="$DEMO_HOME/.config"
unset XDG_DATA_HOME XDG_CACHE_HOME

# ── Export build paths so helper scripts can use them ───────────────────────
export DEMO_PROJECT_ROOT="$PROJECT_ROOT"
export DEMO_BUILD_DIR="$BUILD_DIR"
export DEMO_SMTP_BIN="$MOCK_SMTP_BIN"
export DEMO_SMTP_PORT="$SMTP_PORT"

# ── Add bin/ to PATH ─────────────────────────────────────────────────────────
export PATH="$BIN_DIR:$PATH"
export PS1='\[\033[01;32m\]demo@email-cli\[\033[00m\]:\[\033[01;34m\]~\[\033[00m\]\$ '

echo "[demo] Environment ready. Account: $DEMO_ACCOUNT"
echo "[demo] IMAP mock PID: $DEMO_IMAP_PID  SMTP mock PID: $DEMO_SMTP_PID"
