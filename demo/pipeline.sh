#!/usr/bin/env bash
# demo/pipeline.sh — end-to-end YouTube demo video builder
#
# Dependencies:
#   vhs        https://github.com/charmbracelet/vhs   (brew/apt install vhs)
#   edge-tts   pip install edge-tts                   (free, no API key)
#   ffmpeg     sudo apt install ffmpeg
#
# Optional env vars:
#   NARRATION_LANG     "en" or "hu" (default: hu)
#   EDGE_VOICE         edge-tts voice name (default: hu-HU-NoemiNeural)
#   ELEVENLABS_API_KEY if set, uses ElevenLabs instead of edge-tts
#   OPENAI_API_KEY     if set and no ElevenLabs key, uses OpenAI TTS
#
# Usage:
#   ./demo/pipeline.sh                        # Hungarian, edge-tts (free)
#   NARRATION_LANG=en ./demo/pipeline.sh      # English, edge-tts (free)
#   ELEVENLABS_API_KEY=sk-... ./demo/pipeline.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LANG_CHOICE="${NARRATION_LANG:-hu}"
EDGE_VOICE="${EDGE_VOICE:-hu-HU-NoemiNeural}"

TAPE_FILE="$SCRIPT_DIR/demo.tape"
NARRATION_FILE="$SCRIPT_DIR/narration_${LANG_CHOICE}.txt"
VIDEO_RAW="$SCRIPT_DIR/demo.mp4"
AUDIO_FILE="$SCRIPT_DIR/narration.mp3"
OUTPUT_FILE="$SCRIPT_DIR/email-cli-demo.mp4"

# ── Step 1: Record terminal session ─────────────────────────────────────────
echo "=== Step 1: Record terminal session with VHS ==="
if ! command -v vhs &>/dev/null; then
    echo "ERROR: vhs not found."
    echo "  Ubuntu: snap install vhs  OR  go install github.com/charmbracelet/vhs@latest"
    echo "  macOS:  brew install vhs"
    exit 1
fi
vhs "$TAPE_FILE"
echo "  -> $VIDEO_RAW"

# ── Step 2: Generate narration audio ────────────────────────────────────────
echo ""
echo "=== Step 2: Generate narration audio ==="

# Strip file header, timestamp markers, section headers — keep only spoken text
NARRATION_CLEAN="$SCRIPT_DIR/narration_clean.txt"
grep -v '^\[' "$NARRATION_FILE" \
    | grep -v '^=\+' \
    | grep -v '^email-cli —' \
    | grep -v '^Cél hossz' \
    | grep -v '^Total target length' \
    | sed '/^$/d' \
    > "$NARRATION_CLEAN"

if [[ ! -s "$NARRATION_CLEAN" ]]; then
    echo "ERROR: narration_clean.txt is empty after stripping"
    exit 1
fi
echo "  Narration: $(wc -l < "$NARRATION_CLEAN") lines"

if [[ -n "${ELEVENLABS_API_KEY:-}" ]]; then
    echo "  Using ElevenLabs TTS..."
    VOICE_ID="${VOICE_ID:-21m00Tcm4TlvDq8ikWAM}"
    NARRATION_TEXT=$(tr '\n' ' ' < "$NARRATION_CLEAN")
    curl -s -X POST "https://api.elevenlabs.io/v1/text-to-speech/${VOICE_ID}" \
        -H "xi-api-key: ${ELEVENLABS_API_KEY}" \
        -H "Content-Type: application/json" \
        -d "{\"text\": $(echo "$NARRATION_TEXT" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'),
             \"model_id\": \"eleven_multilingual_v2\",
             \"voice_settings\": {\"stability\": 0.5, \"similarity_boost\": 0.75}}" \
        --output "$AUDIO_FILE"

elif [[ -n "${OPENAI_API_KEY:-}" ]]; then
    echo "  Using OpenAI TTS..."
    NARRATION_TEXT=$(tr '\n' ' ' < "$NARRATION_CLEAN")
    curl -s -X POST "https://api.openai.com/v1/audio/speech" \
        -H "Authorization: Bearer ${OPENAI_API_KEY}" \
        -H "Content-Type: application/json" \
        -d "{\"model\": \"tts-1\",
             \"input\": $(echo "$NARRATION_TEXT" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'),
             \"voice\": \"alloy\"}" \
        --output "$AUDIO_FILE"

else
    echo "  Using edge-tts (free, Microsoft neural voices)..."
    if ! command -v edge-tts &>/dev/null; then
        echo "  Installing edge-tts..."
        pip install --quiet edge-tts
    fi
    # English voice fallback
    if [[ "$LANG_CHOICE" == "en" && "$EDGE_VOICE" == "hu-HU-NoemiNeural" ]]; then
        EDGE_VOICE="en-US-AriaNeural"
    fi
    edge-tts --voice "$EDGE_VOICE" --file "$NARRATION_CLEAN" --write-media "$AUDIO_FILE"
fi

rm -f "$NARRATION_CLEAN"

if [[ ! -s "$AUDIO_FILE" ]]; then
    echo "ERROR: narration.mp3 is empty — TTS generation failed"
    exit 1
fi
AUDIO_DURATION=$(ffprobe -v error -show_entries format=duration \
    -of default=noprint_wrappers=1:nokey=1 "$AUDIO_FILE" 2>/dev/null || echo "unknown")
echo "  -> $AUDIO_FILE  (duration: ${AUDIO_DURATION}s)"

# ── Step 3: Merge video + audio ──────────────────────────────────────────────
echo ""
echo "=== Step 3: Merge video + audio with ffmpeg ==="
# Use audio duration as the authoritative output length.
# tpad clones the last video frame if the video ends before the audio.
# volume=2.5 boosts narration to a comfortable listening level (~8 dB).
ffmpeg -y \
    -i "$VIDEO_RAW" \
    -i "$AUDIO_FILE" \
    -filter_complex \
      "[0:v]tpad=stop_mode=clone:stop_duration=9999[v]; \
       [1:a]volume=2.5[a]" \
    -map "[v]" -map "[a]" \
    -c:v libx264 -preset fast -crf 22 \
    -c:a aac -b:a 128k \
    -t "$AUDIO_DURATION" \
    "$OUTPUT_FILE"
echo "  -> $OUTPUT_FILE"

echo ""
echo "=== Done! ==="
echo "Upload to YouTube: $OUTPUT_FILE"
