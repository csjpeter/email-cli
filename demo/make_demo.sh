#!/usr/bin/env bash
# demo/make_demo.sh — Master script: build the full YouTube demo video end-to-end.
#
# Steps:
#   1. Install dependencies  (demo/install_deps.sh)
#   2. Record terminal video  (vhs demo/demo.tape  — internally sources demo_setup.sh)
#   3. Generate AI narration  (edge-tts)
#   4. Merge video + audio    (ffmpeg)
#   5. Stop demo servers and clean up temp files  (demo/demo_teardown.sh)
#
# Optional env vars:
#   NARRATION_LANG     "hu" or "en"  (default: hu)
#   EDGE_VOICE         edge-tts voice name  (default: hu-HU-NoemiNeural)
#   ELEVENLABS_API_KEY use ElevenLabs TTS instead of edge-tts
#   OPENAI_API_KEY     use OpenAI TTS instead of edge-tts
#
# Usage:
#   bash demo/make_demo.sh
#   NARRATION_LANG=en bash demo/make_demo.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Always run teardown on exit (success or failure).
# Pass --keep-intermediate to preserve demo.mp4 + narration.mp3.
TEARDOWN_OPTS=""
[[ "${1:-}" == "--keep-intermediate" ]] && TEARDOWN_OPTS="--keep-intermediate"
trap "bash '$SCRIPT_DIR/demo_teardown.sh' $TEARDOWN_OPTS" EXIT

echo ""
echo "╔══════════════════════════════════════════╗"
echo "║   email-cli demo video — full pipeline   ║"
echo "╚══════════════════════════════════════════╝"
echo ""

# ── Step 1: Dependencies ─────────────────────────────────────────────────────
echo "[ 1/4 ] Checking / installing dependencies..."
bash "$SCRIPT_DIR/install_deps.sh"
echo ""

# ── Steps 2–4: Record + TTS + Merge ─────────────────────────────────────────
echo "[ 2/4 ] Recording terminal session (VHS)..."
echo "[ 3/4 ] Generating narration audio (edge-tts / ElevenLabs / OpenAI)..."
echo "[ 4/4 ] Merging video + audio (ffmpeg)..."
echo ""
bash "$SCRIPT_DIR/pipeline.sh"

# ── Teardown runs via trap ───────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════╗"
echo "║   Done!  Output: demo/email-cli-demo.mp4 ║"
echo "╚══════════════════════════════════════════╝"
