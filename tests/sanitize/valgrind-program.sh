#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-22
# Runs one program of the project under Valgrind, for `manage.sh valgrind-all`.

# manage.sh copies this script into the directory of the instrumented programs
# once for each program, under the program's own name; the real executable sits
# beside it as NAME.bin.  A test starts the script exactly as it would start
# the program (a PTY child, a daemon the TUI forks), so every process of the run
# is a Valgrind process of its own with its own log:
#     $VG_LOG_DIR/$VG_TEST_NAME-NAME.PID.log
# VG_LOG_DIR is required (it is under build/, never /tmp); VG_TEST_NAME names
# the suite or test that started the process and defaults to "run".
# The script `exec`s Valgrind, so the process id and the exit status are the
# program's (status 1 when Valgrind found an error, --error-exitcode).
here=$(dirname "$(readlink -f "$0")")
name=$(basename "$0")
: "${VG_LOG_DIR:?VG_LOG_DIR is not set}"
supp=""
if [ -n "${VG_SUPPRESSIONS:-}" ]; then
    supp="--suppressions=$VG_SUPPRESSIONS"
fi
# shellcheck disable=SC2086  # $supp is empty or one word
exec valgrind --leak-check=full --error-exitcode=1 --track-origins=yes \
    --log-file="$VG_LOG_DIR/${VG_TEST_NAME:-run}-$name.%p.log" $supp \
    "$here/$name.bin" "$@"
