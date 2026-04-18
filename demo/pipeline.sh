#!/usr/bin/env bash
# demo/pipeline.sh — Per-scenario YouTube demo video builder.
#
# Dependencies:
#   vhs        https://github.com/charmbracelet/vhs   (brew/apt install vhs)
#   edge-tts   pip install edge-tts                   (free, no API key)
#   ffmpeg     sudo apt install ffmpeg
#
# Optional env vars:
#   SCENARIO           scenario name (default: email-cli)
#   NARRATION_LANG     "en" or "hu" (default: hu)
#   EDGE_VOICE         edge-tts voice name (overrides language default)
#   TAPE_BUFFER        extra seconds added to each Sleep (default: 0.5)
#
# Usage:
#   ./demo/pipeline.sh
#   SCENARIO=email-cli-ro ./demo/pipeline.sh
#   NARRATION_LANG=en SCENARIO=email-tui ./demo/pipeline.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SCENARIO="${SCENARIO:-email-cli}"
LANG_CHOICE="${NARRATION_LANG:-hu}"
TAPE_BUFFER="${TAPE_BUFFER:-0.5}"

SCENARIO_DIR="$SCRIPT_DIR/scenarios/$SCENARIO"
if [[ ! -d "$SCENARIO_DIR" ]]; then
    echo "ERROR: scenario directory not found: $SCENARIO_DIR" >&2
    exit 1
fi

TEMPLATE_FILE="$SCENARIO_DIR/tape.tape.template"
NARRATION_FILE="$SCENARIO_DIR/narration_${LANG_CHOICE}.txt"
OUTPUT_DIR="$SCENARIO_DIR/output"
TMP_DIR="$SCENARIO_DIR/tmp"

if [[ ! -f "$TEMPLATE_FILE" ]]; then
    echo "ERROR: tape template not found: $TEMPLATE_FILE" >&2
    exit 1
fi
if [[ ! -f "$NARRATION_FILE" ]]; then
    echo "ERROR: narration file not found: $NARRATION_FILE" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR" "$TMP_DIR"

# Default voice selection
if [[ -z "${EDGE_VOICE:-}" ]]; then
    if [[ "$LANG_CHOICE" == "en" ]]; then
        EDGE_VOICE="en-US-AriaNeural"
    else
        EDGE_VOICE="hu-HU-NoemiNeural"
    fi
fi

TAPE_FILE="$TMP_DIR/generated.tape"
AUDIO_DIR="$TMP_DIR/audio"
NARRATION_MP3="$TMP_DIR/narration.mp3"
VIDEO_RAW="$TMP_DIR/raw.mp4"
OUTPUT_FILE="$OUTPUT_DIR/email-cli-${SCENARIO}-demo.mp4"

echo ""
echo "=== Scenario: $SCENARIO  lang: $LANG_CHOICE  voice: $EDGE_VOICE ==="
echo ""

# ── Step 1: Generate per-section MP3s ────────────────────────────────────────
echo "=== Step 1: Generate per-section audio (edge-tts) ==="
bash "$SCRIPT_DIR/gen_audio.sh" "$NARRATION_FILE" "$EDGE_VOICE" "$AUDIO_DIR"
echo "  -> $AUDIO_DIR"
echo ""

# ── Step 2: Generate tape from template ──────────────────────────────────────
echo "=== Step 2: Generate VHS tape from template ==="
bash "$SCRIPT_DIR/gen_tape.sh" \
    "$TEMPLATE_FILE" \
    "$AUDIO_DIR/durations.sh" \
    "$TAPE_FILE" \
    "$TAPE_BUFFER"
echo "  -> $TAPE_FILE"
echo ""

# ── Step 3: Record terminal session ──────────────────────────────────────────
echo "=== Step 3: Record terminal session with VHS ==="
if ! command -v vhs &>/dev/null; then
    echo "ERROR: vhs not found." >&2
    echo "  Ubuntu: snap install vhs  OR  go install github.com/charmbracelet/vhs@latest" >&2
    echo "  macOS:  brew install vhs" >&2
    exit 1
fi
# VHS requires a relative Output path (absolute paths are rejected by the parser).
# Run vhs from TMP_DIR so "Output raw.mp4" resolves to $VIDEO_RAW.
{
    echo "Output raw.mp4"
    cat "$TAPE_FILE"
} > "$TMP_DIR/with_output.tape"
(cd "$TMP_DIR" && vhs with_output.tape)
echo "  -> $VIDEO_RAW"
echo ""

# ── Step 4: Concatenate per-section MP3s → narration.mp3 ─────────────────────
echo "=== Step 4: Concatenate audio sections ==="
if [[ ! -f "$AUDIO_DIR/concat.txt" ]]; then
    echo "ERROR: concat.txt not found in $AUDIO_DIR" >&2
    exit 1
fi
ffmpeg -y -f concat -safe 0 -i "$AUDIO_DIR/concat.txt" -c copy "$NARRATION_MP3"
AUDIO_DURATION=$(ffprobe -v error -show_entries format=duration \
    -of default=noprint_wrappers=1:nokey=1 "$NARRATION_MP3" 2>/dev/null)
echo "  -> $NARRATION_MP3  (duration: ${AUDIO_DURATION}s)"
echo ""

# ── Step 5: Merge video + audio ───────────────────────────────────────────────
echo "=== Step 5: Merge video + audio with ffmpeg ==="
# tpad clones the last video frame if video ends before audio.
# volume=2.5 boosts narration to a comfortable listening level (~8 dB).
ffmpeg -y \
    -i "$VIDEO_RAW" \
    -i "$NARRATION_MP3" \
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
