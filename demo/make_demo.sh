#!/usr/bin/env bash
# demo/make_demo.sh — Master script: build the full YouTube demo video end-to-end.
#
# Steps:
#   1. Install dependencies      (demo/install_deps.sh)
#   2. Generate per-section audio (demo/gen_audio.sh via pipeline.sh)
#   3. Generate VHS tape          (demo/gen_tape.sh via pipeline.sh)
#   4. Record terminal video      (vhs)
#   5. Merge video + audio        (ffmpeg)
#   6. Teardown demo servers      (demo/demo_teardown.sh)
#
# Optional env vars:
#   SCENARIO           scenario name (default: email-cli)
#   NARRATION_LANG     "hu" or "en"  (default: hu)
#   EDGE_VOICE         edge-tts voice name
#   TAPE_BUFFER        extra seconds per Sleep (default: 0.5)
#
# Usage:
#   bash demo/make_demo.sh
#   bash demo/make_demo.sh email-cli-ro
#   SCENARIO=email-tui bash demo/make_demo.sh
#   NARRATION_LANG=en bash demo/make_demo.sh email-sync

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Accept scenario as first positional argument or SCENARIO env var.
if [[ -n "${1:-}" && "${1:-}" != "--keep-intermediate" ]]; then
    export SCENARIO="${1}"
    shift
fi
export SCENARIO="${SCENARIO:-email-cli}"

# Always run teardown on exit (success or failure).
TEARDOWN_OPTS=""
[[ "${1:-}" == "--keep-intermediate" ]] && TEARDOWN_OPTS="--keep-intermediate"
trap "bash '$SCRIPT_DIR/demo_teardown.sh' $TEARDOWN_OPTS" EXIT

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║   email-cli demo video — scenario: $SCENARIO"
echo "╚══════════════════════════════════════════════════╝"
echo ""

# ── Step 1: Dependencies ─────────────────────────────────────────────────────
echo "[ 1/5 ] Checking / installing dependencies..."
bash "$SCRIPT_DIR/install_deps.sh"
echo ""

# ── Steps 2–5: Audio + Tape + Record + Merge ─────────────────────────────────
echo "[ 2/5 ] Generating per-section audio (edge-tts)..."
echo "[ 3/5 ] Generating VHS tape from template..."
echo "[ 4/5 ] Recording terminal session (VHS)..."
echo "[ 5/5 ] Merging video + audio (ffmpeg)..."
echo ""
bash "$SCRIPT_DIR/pipeline.sh"

# ── Teardown runs via trap ───────────────────────────────────────────────────
OUTPUT_FILE="$SCRIPT_DIR/scenarios/$SCENARIO/output/email-cli-${SCENARIO}-demo.mp4"
echo ""
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║   Done!  Output: $OUTPUT_FILE"
echo "╚══════════════════════════════════════════════════════════════════╝"
