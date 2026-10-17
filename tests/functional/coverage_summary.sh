#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-17
# One recorded line of coverage figures from lcov tracefiles.

# EMAIL-120: prints "DATE COMMIT unit=U% functional=F% combined=C% functions=N%"
# from the unit-only, functional+PTY and combined tracefiles, so that the
# figures of a run can be kept (build/coverage-summary.txt) and compared per
# release (docs/dev/testing.md).  Usage:
#   coverage_summary.sh DATE COMMIT UNIT.info FUNCTIONAL.info COMBINED.info
set -u
[ "$#" -eq 5 ] || { echo "usage: coverage_summary.sh DATE COMMIT UNIT.info FUNCTIONAL.info COMBINED.info" >&2; exit 2; }
date_s="$1"; commit="$2"; shift 2
for f in "$@"; do [ -s "$f" ] || { echo "ERROR: no tracefile $f" >&2; exit 1; }; done
# pct FILE KEYFOUND KEYHIT: sum of the two counters as a percentage
pct() {
    awk -F: -v f="$2" -v h="$3" '$1==f {t+=$2} $1==h {x+=$2} END { if (t>0) printf "%.1f", 100*x/t; else print "n/a" }' "$1"
}
echo "$date_s $commit unit=$(pct "$1" LF LH)% functional=$(pct "$2" LF LH)% combined=$(pct "$3" LF LH)% functions=$(pct "$3" FNF FNH)%"
