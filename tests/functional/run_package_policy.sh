#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-11
# Checks the Debian package against the packaging policy.

# Checks the Debian package against the packaging policy (EMAIL-16): lintian
# clean, dependencies discovered by shlibdeps, gzipped man pages, changelog and
# copyright present, directory modes independent of the caller's umask, and a
# lintian finding makes `manage.sh package deb` fail and leave nothing behind.
# Run from anywhere; it does its own release build.
set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT="$PWD"
PKG="$ROOT/build/packages"
STUBS="$ROOT/build/package-policy-stubs-$$"
PASSED=0; FAILED=0

ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

# 1. Build under a group-writable umask: the package must not inherit it.
out=$( umask 002; "$ROOT/manage.sh" package deb 2>&1 ); rc=$?
[ "$rc" -eq 0 ] && ok "1.1 package deb exits 0" || { bad "1.1 package deb exits $rc"; echo "$out" | tail -8; }
deb=$(ls "$PKG"/email-cli_*_amd64.deb 2>&1 | head -1)
[ -f "$deb" ] || { bad "1.2 no deb built: $deb"; echo "Passed: $PASSED / $((PASSED + FAILED))"; exit 1; }

lintian "$deb"; rc=$?
[ "$rc" -eq 0 ] && ok "1.2 lintian reports nothing" || bad "1.2 lintian exit $rc"

deps=$(dpkg-deb -f "$deb" Depends)
case "$deps" in *libc6*) ok "1.3 Depends names libc6" ;; *) bad "1.3 no libc6 in: $deps" ;; esac
case "$deps" in *"libssl3 "*|*"libcurl4 "*) bad "1.4 hand-written package names in: $deps" ;; *) ok "1.4 no hand-written libssl3/libcurl4" ;; esac
case "$deps" in *"(="*|*"(<<"*|*"(<="*) bad "1.5 exact or upper-bound dependency: $deps" ;; *) ok "1.5 dependencies only look upward" ;; esac

list=$(dpkg-deb -c "$deb")
echo "$list" | grep -q 'usr/share/doc/email-cli/changelog.gz$' && ok "1.6 changelog.gz installed" || bad "1.6 no changelog.gz"
echo "$list" | grep -q 'usr/share/doc/email-cli/copyright$'    && ok "1.7 copyright installed"    || bad "1.7 no copyright"
echo "$list" | grep '^-.*usr/share/man/man1/' | grep -qv '\.1\.gz$' && bad "1.8 an uncompressed man page" || ok "1.8 every man page is gzipped"
[ "$(echo "$list" | grep -c 'usr/share/man/man1/.*\.1\.gz$')" -eq 5 ] && ok "1.9 five man pages" || bad "1.9 man page count"
echo "$list" | grep '^d' | grep -qv '^drwxr-xr-x' && bad "1.10 a directory is not 0755" || ok "1.10 every directory is 0755"

# 1b. The package carries its commit and build date as XB- control fields
# (EMAIL-75).  A tree with uncommitted changes adds "+dirty" to the commit.
head=$(git rev-parse --short=12 HEAD)
commit=$(dpkg-deb -f "$deb" XB-Git-Commit)
case "$commit" in "$head"|"$head+dirty") ok "1.11 XB-Git-Commit is $commit" ;; *) bad "1.11 XB-Git-Commit '$commit', HEAD is $head" ;; esac
bdate=$(dpkg-deb -f "$deb" XB-Build-Date)
echo "$bdate" | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$' && ok "1.12 XB-Build-Date is $bdate" || bad "1.12 XB-Build-Date '$bdate'"
[ "$(dpkg-deb -f "$deb" Version)" = "$(dpkg-deb -f "$deb" Version | tr -d '+ ')" ] && ok "1.13 Version carries no build metadata" || bad "1.13 Version polluted"

# 2. A lintian finding fails the run and leaves build/packages/ empty.
mkdir -p "$STUBS" || exit 1
printf '#!/bin/sh\necho "stub lintian: E: invented-finding" >&2\nexit 1\n' > "$STUBS/lintian"
chmod 755 "$STUBS/lintian"
out=$(PATH="$STUBS:$PATH" "$ROOT/manage.sh" package deb 2>&1); rc=$?
[ "$rc" -ne 0 ] && ok "2.1 a lintian finding exits non-zero" || bad "2.1 exited 0 despite a finding"
[ "$(find "$PKG" -mindepth 1 | wc -l)" -eq 0 ] && ok "2.2 build/packages is empty" || bad "2.2 left: $(ls "$PKG" | tr '\n' ' ')"
echo "$out" | grep -q "lintian findings" && ok "2.3 the failure is explained" || bad "2.3 no explanation"

( cd "$STUBS" && rm -f lintian ) && rmdir "$STUBS"

echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
