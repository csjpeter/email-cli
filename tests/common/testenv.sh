#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-13
# Test environment: one LXC container per run, behind five functions.

# Sourced (". tests/common/testenv.sh") by tests that need a container; the
# interface is the one of policies/testing-model.md, "The test environment
# script".  This is email-cli's own copy.  Which tool does the work (lxc-*)
# stays behind the functions; a missing tool is a failure, not a skip.
#
#   testenv_create PURPOSE   create and start <project>-PURPOSE-<pid>, wait for
#                            its network; records the name in TESTENV_NAME
#   testenv_install PKG...   install packages inside it; an argument ending in
#                            .deb is a file of this tree: pushed, then installed
#   testenv_push SRC DEST    copy a file into it
#   testenv_exec CMD...      run a command inside it; status and output are the
#                            command's
#   testenv_destroy          remove exactly what this run created (idempotent;
#                            call it from the test's trap, also on failure)
#
# sudo is used for lxc-* and only on the container this run created
# (policies/testing-model.md, "sudo for the containers").  lxc-attach takes over
# the descriptors it inherits and changes a regular file on them to root:0600, so
# every call gets a pipe for output and /dev/null (or a pipe) for input.
#
# Paths: everything is under build/tests/e2e/; the system temp directory is not used.

TESTENV_PROJECT="email-cli"
TESTENV_ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
TESTENV_WORK="$TESTENV_ROOT/build/tests/e2e/testenv-$$"
TESTENV_NAME=""
TESTENV_RC="$TESTENV_WORK/rc"

testenv__die() { echo "testenv: $*" >&2; return 1; }

# testenv__lxc ARGS...: sudo lxc-* with the caller's stdin closed; output
# through a pipe; the command's own status is returned.
testenv__lxc() {
    ( sudo "$@" </dev/null 2>&1; echo $? > "$TESTENV_RC" ) | cat
    return "$(cat "$TESTENV_RC")"
}

testenv_create() {
    purpose="${1:?testenv_create PURPOSE}"
    command -v lxc-create >/dev/null || { testenv__die "lxc-create not found (apt-get install lxc)"; return 1; }
    mkdir -p "$TESTENV_WORK" || return 1
    TESTENV_NAME="$TESTENV_PROJECT-$purpose-$$"
    echo "testenv: creating container $TESTENV_NAME (Debian trixie)"
    testenv__lxc lxc-create -n "$TESTENV_NAME" -t download -- -d debian -r trixie -a amd64 || return 1
    testenv__lxc lxc-start -n "$TESTENV_NAME" || return 1
    i=0
    while [ "$i" -lt 60 ]; do
        testenv__lxc lxc-attach -n "$TESTENV_NAME" -- getent hosts deb.debian.org >/dev/null && return 0
        i=$((i + 1)); sleep 2
    done
    testenv__die "container $TESTENV_NAME has no network"
}

testenv_exec() {
    [ -n "$TESTENV_NAME" ] || { testenv__die "no container"; return 1; }
    testenv__lxc lxc-attach -n "$TESTENV_NAME" -- "$@"
}

testenv_push() {
    src="${1:?testenv_push SRC DEST}"; dest="${2:?testenv_push SRC DEST}"
    [ -n "$TESTENV_NAME" ] || { testenv__die "no container"; return 1; }
    [ -f "$src" ] || { testenv__die "no such file: $src"; return 1; }
    ( cat "$src" | sudo lxc-attach -n "$TESTENV_NAME" -- sh -c 'cat > "$1"' sh "$dest" 2>&1
      echo $? > "$TESTENV_RC" ) | cat
    return "$(cat "$TESTENV_RC")"
}

testenv_install() {
    [ "$#" -gt 0 ] || { testenv__die "testenv_install PKG..."; return 1; }
    args=""
    for p in "$@"; do
        case "$p" in
            *.deb)
                base="$(basename "$p")"
                testenv_push "$p" "/root/$base" || return 1
                args="$args /root/$base" ;;
            *) args="$args $p" ;;
        esac
    done
    testenv_exec apt-get update -qq || return 1
    # shellcheck disable=SC2086  # $args is a list of names without blanks
    testenv_exec env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $args
}

testenv_destroy() {
    [ -n "$TESTENV_NAME" ] || return 0
    case "$TESTENV_NAME" in
        "$TESTENV_PROJECT"-*-*) ;;
        *) echo "testenv: refusing to destroy '$TESTENV_NAME'" >&2; return 1 ;;
    esac
    name="$TESTENV_NAME"
    TESTENV_NAME=""
    echo "testenv: destroying $name"
    testenv__lxc lxc-stop -n "$name" -k || echo "testenv: lxc-stop $name failed" >&2
    testenv__lxc lxc-destroy -n "$name" || echo "testenv: lxc-destroy $name failed" >&2
    rm -f "$TESTENV_RC"
    rmdir "$TESTENV_WORK"
}
