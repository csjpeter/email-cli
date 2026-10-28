#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-22
# Turns the logs of a sanitizer or Valgrind run into one verdict.

# Usage: scan_logs.sh sanitizer DIR | scan_logs.sh valgrind DIR
#
# sanitizer: ASan and UBSan write a log file (ASAN_OPTIONS=log_path=DIR/asan,
#            UBSAN_OPTIONS=log_path=DIR/ubsan) only when they have something to
#            say, so every file in DIR is a finding.
# valgrind:  every process has a log; the ones whose "ERROR SUMMARY" is not
#            "0 errors" are findings.
# Each finding log is named and its first lines are shown.  Exit status 1 when
# there is a finding, 2 on a usage error, 0 when DIR holds none.
mode=${1:-}
dir=${2:-}
if [ -z "$dir" ] || [ ! -d "$dir" ]; then
    echo "usage: scan_logs.sh sanitizer|valgrind DIR" >&2
    exit 2
fi
found=0
for f in "$dir"/*; do
    [ -f "$f" ] || continue
    case "$mode" in
        sanitizer) bad=1 ;;
        valgrind)
            if grep -q 'ERROR SUMMARY: 0 errors' "$f"; then bad=0; else
                # A process killed before it could print the summary has no
                # verdict of its own; an error line in it still counts.
                if grep -q 'ERROR SUMMARY' "$f" || grep -q '^==[0-9]*== \(Invalid\|Conditional\|Use of\|Syscall param\|[0-9,]* bytes in\)' "$f"; then bad=1; else bad=0; fi
            fi ;;
        *) echo "usage: scan_logs.sh sanitizer|valgrind DIR" >&2; exit 2 ;;
    esac
    if [ "$bad" = 1 ]; then
        found=$((found + 1))
        echo "FINDING: $f"
        head -n 25 "$f"
        echo "-----"
    fi
done
if [ "$found" -gt 0 ]; then
    echo "$mode: $found log(s) with findings in $dir" >&2
    exit 1
fi
echo "$mode: no findings in $dir"
