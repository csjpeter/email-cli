#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-11-28
# The one wait of the shell tests: poll a condition, end when it holds.

# Sourced by the shell suites (". tests/common/wait.sh").  A wait checks its
# condition every WAIT_POLL_S seconds and returns the moment it holds; the
# limit is only a generous ceiling (policies/testing-model.md, "Wait for a
# condition, not for a clock"; EMAIL-324).  The C tests have the same in
# tests/common/test_scratch.c (test_wait_until).

WAIT_POLL_S=0.05

# wait_for SECONDS CMD [ARG...]: run CMD until it succeeds, at most SECONDS
# (a whole number).  Returns 0 when it did, 1 on timeout.  CMD's own output
# is not hidden.
wait_for() {
    local limit="$1" end
    shift
    end=$(( $(date +%s%N) + limit * 1000000000 ))
    while :; do
        "$@" && return 0
        [ "$(date +%s%N)" -lt "$end" ] || return 1
        sleep "$WAIT_POLL_S"   # the poll interval, not a guess at a duration
    done
}

# port_listens PORT: something listens on TCP PORT (ss, so no connection is
# made: a mock that counts its connections sees none).
port_listens() {
    ss -ltnH "sport = :$1" | grep -q .
}

# port_closed PORT: nothing listens on TCP PORT.
port_closed() {
    ! port_listens "$1"
}
