#!/usr/bin/env bash
# demo/restart_smtp.sh
# Restarts the mock SMTP server (it exits after one exchange).
# Requires env vars set by demo_setup.sh (source that first).
pkill -f "mock_smtp_server" 2>/dev/null || true
sleep 0.3
(cd "$DEMO_BUILD_DIR" && MOCK_SMTP_PORT="$DEMO_SMTP_PORT" "$DEMO_SMTP_BIN") \
    >/tmp/email-cli-demo-smtp.log 2>&1 &
sleep 0.5
