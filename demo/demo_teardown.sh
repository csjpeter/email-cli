#!/usr/bin/env bash
# demo/demo_teardown.sh — Stop demo mock servers and remove temp files.
# Safe to call multiple times (all operations are idempotent).
#
# Usage:
#   bash demo/demo_teardown.sh                  # full cleanup (default)
#   bash demo/demo_teardown.sh --keep-intermediate  # keep demo.mp4 + narration.mp3

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEEP_INTERMEDIATE=false
[[ "${1:-}" == "--keep-intermediate" ]] && KEEP_INTERMEDIATE=true

echo "=== Demo teardown ==="

# ── Stop mock servers ────────────────────────────────────────────────────────
if pkill -f "mock_imap_server" 2>/dev/null; then
    echo "  Stopped mock IMAP server"
else
    echo "  mock_imap_server was not running"
fi

if pkill -f "mock_smtp_server" 2>/dev/null; then
    echo "  Stopped mock SMTP server"
else
    echo "  mock_smtp_server was not running"
fi

# ── Remove /tmp temp files ───────────────────────────────────────────────────
for path in \
    /tmp/email-cli-demo-home \
    /tmp/email-cli-demo-imap.log \
    /tmp/email-cli-demo-smtp.log \
    /tmp/email-cli-ft-alpha \
    /tmp/email-cli-ft-beta \
    /tmp/email-cli-ft-gamma \
    /tmp/email-cli-ft-ab \
    /tmp/email-cli-ft-bg \
    /tmp/email-cli-ft-all \
    /tmp/email-cli-ft-shared
do
    if [[ -e "$path" ]]; then
        rm -rf "$path"
        echo "  Removed $path"
    fi
done

# ── Remove intermediate demo output files ───────────────────────────────────
if $KEEP_INTERMEDIATE; then
    echo "  Kept intermediate files (demo.mp4, narration.mp3)"
else
    for path in \
        "$SCRIPT_DIR/demo.mp4" \
        "$SCRIPT_DIR/narration.mp3"
    do
        if [[ -e "$path" ]]; then
            rm -f "$path"
            echo "  Removed $path"
        fi
    done
fi

echo "=== Teardown complete ==="
