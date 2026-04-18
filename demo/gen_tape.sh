#!/usr/bin/env bash
# demo/gen_tape.sh — Replace __SECTION_NAME__ placeholders in a tape template.
#
# Usage:
#   gen_tape.sh <template_file> <durations_file> <output_tape> [buffer_seconds=0.5]
#
# Reads durations.sh (lines like DURATION_SECTION_NAME=4.73) and replaces every
# occurrence of __SECTION_NAME__ in the template with (duration + buffer)s.
# Example: __INTRO__ with duration 8.42 and buffer 0.5 becomes "8.92s".

set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "Usage: $0 <template_file> <durations_file> <output_tape> [buffer_seconds=0.5]" >&2
    exit 1
fi

TEMPLATE_FILE="$1"
DURATIONS_FILE="$2"
OUTPUT_TAPE="$3"
BUFFER="${4:-0.5}"

if [[ ! -f "$TEMPLATE_FILE" ]]; then
    echo "ERROR: template file not found: $TEMPLATE_FILE" >&2
    exit 1
fi

if [[ ! -f "$DURATIONS_FILE" ]]; then
    echo "ERROR: durations file not found: $DURATIONS_FILE" >&2
    exit 1
fi

# Build a sed script from the durations file.
SED_SCRIPT=$(mktemp /tmp/gen_tape_sed_XXXXXX.sed)

while IFS='=' read -r key value; do
    [[ "$key" =~ ^DURATION_([A-Z_]+)$ ]] || continue
    section="${BASH_REMATCH[1]}"
    # Compute duration + buffer using awk for portable floating-point arithmetic.
    sleep_val=$(awk -v d="$value" -v b="$BUFFER" 'BEGIN { printf "%.2f", d + b }')
    # Escape forward slashes in sleep_val (shouldn't occur, but be safe).
    safe_val="${sleep_val//\//\\/}"
    echo "s/__${section}__/${safe_val}s/g" >> "$SED_SCRIPT"
done < "$DURATIONS_FILE"

mkdir -p "$(dirname "$OUTPUT_TAPE")"
sed -f "$SED_SCRIPT" "$TEMPLATE_FILE" > "$OUTPUT_TAPE"
rm -f "$SED_SCRIPT"

# Warn about any remaining unreplaced placeholders.
if grep -qE '__[A-Z_]+__' "$OUTPUT_TAPE" 2>/dev/null; then
    echo "WARNING: unreplaced placeholders remain in $OUTPUT_TAPE:" >&2
    grep -oE '__[A-Z_]+__' "$OUTPUT_TAPE" | sort -u >&2
fi

echo "  [gen_tape] Generated tape: $OUTPUT_TAPE"
