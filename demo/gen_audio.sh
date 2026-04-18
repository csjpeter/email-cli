#!/usr/bin/env bash
# demo/gen_audio.sh — Parse narration file into per-section MP3s + durations.
#
# Usage:
#   gen_audio.sh <narration_file> <voice> <output_dir>
#
# Narration format:
#   Sections start with "## SECTION_NAME" (uppercase letters and underscores).
#   All subsequent non-empty lines until the next section header are the spoken text.
#
# Output:
#   <output_dir>/SECTION_NAME.mp3   — one MP3 per section
#   <output_dir>/durations.sh       — lines like: DURATION_SECTION_NAME=4.73
#   <output_dir>/concat.txt         — ffmpeg concat demuxer format

set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "Usage: $0 <narration_file> <voice> <output_dir>" >&2
    exit 1
fi

NARRATION_FILE="$1"
VOICE="$2"
OUTPUT_DIR="$3"

if [[ ! -f "$NARRATION_FILE" ]]; then
    echo "ERROR: narration file not found: $NARRATION_FILE" >&2
    exit 1
fi

if ! command -v edge-tts &>/dev/null; then
    echo "Installing edge-tts..."
    pip install --quiet edge-tts
fi

if ! command -v ffprobe &>/dev/null; then
    echo "ERROR: ffprobe not found (install ffmpeg)" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

DURATIONS_FILE="$OUTPUT_DIR/durations.sh"
CONCAT_FILE="$OUTPUT_DIR/concat.txt"

> "$DURATIONS_FILE"
> "$CONCAT_FILE"

current_section=""
current_text=""

flush_section() {
    local section="$1"
    local text="$2"

    [[ -z "$section" ]] && return
    [[ -z "$(echo "$text" | tr -d '[:space:]')" ]] && return

    local mp3="$OUTPUT_DIR/${section}.mp3"
    local tmp_text
    tmp_text=$(mktemp /tmp/gen_audio_XXXXXX.txt)
    printf '%s\n' "$text" > "$tmp_text"

    echo "  [gen_audio] TTS: $section -> $mp3"
    edge-tts --voice "$VOICE" --file "$tmp_text" --write-media "$mp3"
    rm -f "$tmp_text"

    if [[ ! -s "$mp3" ]]; then
        echo "ERROR: TTS produced empty file for section $section" >&2
        exit 1
    fi

    local duration
    duration=$(ffprobe -v error -show_entries format=duration \
        -of default=noprint_wrappers=1:nokey=1 "$mp3" 2>/dev/null)

    if [[ -z "$duration" ]]; then
        echo "ERROR: could not determine duration for $mp3" >&2
        exit 1
    fi

    echo "DURATION_${section}=${duration}" >> "$DURATIONS_FILE"
    echo "file '${mp3}'" >> "$CONCAT_FILE"
}

while IFS= read -r line; do
    if [[ "$line" =~ ^##[[:space:]]+([A-Z_]+)[[:space:]]*$ ]]; then
        flush_section "$current_section" "$current_text"
        current_section="${BASH_REMATCH[1]}"
        current_text=""
    else
        if [[ -n "$current_section" ]]; then
            if [[ -n "$(echo "$line" | tr -d '[:space:]')" ]]; then
                if [[ -n "$current_text" ]]; then
                    current_text="${current_text}
${line}"
                else
                    current_text="$line"
                fi
            fi
        fi
    fi
done < "$NARRATION_FILE"

flush_section "$current_section" "$current_text"

echo "  [gen_audio] Durations written to: $DURATIONS_FILE"
echo "  [gen_audio] Concat list written to: $CONCAT_FILE"
