#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-17
# Finds coverage counter files that are not valid gcov data.

# EMAIL-119: a program killed while it writes its .gcda at exit leaves a
# truncated or empty file, and lcov then fails the whole report with "not a
# gcov data file".  This names every such file (stderr), removes it so the
# report can still be made from the rest, and exits 1 so the run does not
# pass silently.  Usage: gcda_check.sh DIR
set -u
dir="${1:-}"
[ -n "$dir" ] && [ -d "$dir" ] || { echo "usage: gcda_check.sh DIR (a directory)" >&2; exit 2; }
bad=0
while IFS= read -r -d '' f; do
    # a valid file starts with the magic "adcg" (little-endian "gcda")
    magic=$(head -c 4 "$f")
    if [ "$magic" != "adcg" ] && [ "$magic" != "gcda" ]; then
        echo "ERROR: invalid gcov data file ($(stat -c %s "$f") bytes), removed: $f" >&2
        rm -f "$f"
        bad=$((bad + 1))
    fi
done < <(find "$dir" -name '*.gcda' -print0)
[ "$bad" -eq 0 ] || { echo "ERROR: $bad invalid .gcda file(s); the coverage figure lacks their objects" >&2; exit 1; }
