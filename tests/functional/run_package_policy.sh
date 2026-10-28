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
export MANAGE_NO_TAG=1   # EMAIL-229: a test build is not a release, no tag
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
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
[ -z "$deps" ] && ok "1.3 the programs are static: the package has no Depends" || bad "1.3 Depends in a static package: $deps"
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

# 1c. The programs are fully static (EMAIL-103): the shipped binaries need no
# shared library and no dynamic loader, so one binary copied to another Linux
# distribution runs with nothing installed beside it.
tmp="$ROOT/build/package-policy-extract-$$"
mkdir -p "$tmp" && dpkg-deb -x "$deb" "$tmp" || { bad "1.14 cannot unpack $deb"; }
needed=$(for b in "$tmp"/usr/bin/*; do readelf -d "$b" 2>&1 | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'; done | sort -u)
[ -z "$needed" ] && ok "1.14 the binaries need no shared library" || bad "1.14 shared libraries needed: $needed"
interp=$(for b in "$tmp"/usr/bin/*; do readelf -l "$b" | grep -c INTERP; done | sort -u | tr -d '\n')
[ "$interp" = "0" ] && ok "1.15 no binary asks for a dynamic loader" || bad "1.15 a binary has an interpreter ($interp)"
[ -z "$deps" ] && ok "1.16 Depends is empty (libc, OpenSSL and libtuikit are inside)" || bad "1.16 Depends: $deps"
for b in "$tmp"/usr/bin/*; do
    n=$(basename "$b")
    out=$("$b" --version 2>&1 | head -1); rcv=$?
    [ -n "$out" ] && ok "1.16c $n runs ($out)" || bad "1.16c $n printed nothing for --version"
done
( cd "$tmp" && rm -rf usr ) && rmdir "$tmp"

# 1d. libemail-client (EMAIL-101): the same build ships the shared library and
# its development files as two more packages, each lintian-clean (checked by
# `package deb`), with dependencies that only look upward.
rt=$(ls "$PKG"/libemail-client0_*_amd64.deb 2>&1 | head -1)
dev=$(ls "$PKG"/libemail-client-dev_*_amd64.deb 2>&1 | head -1)
[ -f "$rt" ] && ok "1.17 libemail-client0 is built" || bad "1.17 no runtime package: $rt"
[ -f "$dev" ] && ok "1.18 libemail-client-dev is built" || bad "1.18 no dev package: $dev"
if [ -f "$rt" ] && [ -f "$dev" ]; then
    [ "$(dpkg-deb -f "$dev" Depends)" = "libemail-client0 (= $(dpkg-deb -f "$rt" Version))" ] \
        && ok "1.19 -dev depends exactly on the runtime package of the same build" \
        || bad "1.19 -dev Depends: $(dpkg-deb -f "$dev" Depends)"
    rtdeps=$(dpkg-deb -f "$rt" Depends)
    case "$rtdeps" in *"(="*|*"(<<"*|*"(<="*) bad "1.20 runtime exact or upper-bound: $rtdeps" ;; *) ok "1.20 runtime dependencies only look upward" ;; esac
    lib="$ROOT/build/package-policy-lib-$$"
    mkdir -p "$lib" && dpkg-deb -x "$rt" "$lib" && dpkg-deb -x "$dev" "$lib" && dpkg-deb -e "$rt" "$lib/ctl" \
        || bad "1.21 cannot unpack the library packages"
    grep -q '^libemail-client 0 libemail-client0 (>= ' "$lib/ctl/shlibs" \
        && ok "1.21 shlibs asks for at least the version" || bad "1.21 shlibs: $(cat "$lib/ctl/shlibs")"
    [ -f "$lib/ctl/triggers" ] && [ ! -e "$lib/ctl/postinst" ] \
        && ok "1.22 ldconfig by trigger, not by maintainer script" || bad "1.22 ldconfig handling"
    so=$(ls "$lib"/usr/lib/*/libemail-client.so.0.0.0 2>&1 | head -1)
    exported=$(nm -D --defined-only "$so" | awk '$2 ~ /^[TDB]$/ {print $3}' | grep -v -E '^(_init|_fini|__bss_start|_edata|_end)$' | sort -u)
    [ -n "$exported" ] && [ -z "$(echo "$exported" | grep -v '^emc_')" ] \
        && ok "1.23 only emc_* symbols are exported" || bad "1.23 exported: $exported"
    needed=$(readelf -d "$so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort -u)
    extra=$(echo "$needed" | grep -v -E '^(libc\.so\.6|libm\.so\.6|libssl\.so\.3|libcrypto\.so\.3)$')
    [ -z "$extra" ] && ok "1.24 the library needs only libc and OpenSSL (no libcurl)" || bad "1.24 extra: $extra"
    # A program written against the installed header links and runs.
    printf '#include <email-client.h>\n#include <stdio.h>\nint main(void){puts(emc_version());return 0;}\n' > "$lib/use.c"
    out=$(gcc -o "$lib/use" "$lib/use.c" -I"$lib/usr/include" -L"$(dirname "$so")" -lemail-client 2>&1) \
        && ver=$(LD_LIBRARY_PATH="$(dirname "$so")" "$lib/use") \
        && [ "$ver" = "$(dpkg-deb -f "$rt" Version)" ] \
        && ok "1.25 a program links the packaged library and gets $ver" \
        || bad "1.25 link or run failed: $out"
    [ -f "$lib/usr/lib/$(basename "$(dirname "$so")")/pkgconfig/libemail-client.pc" ] \
        && ok "1.26 the pkg-config file is in -dev" || bad "1.26 no .pc file"
    [ -f "$lib/usr/share/man/man3/libemail-client.3.gz" ] \
        && ok "1.27 the man page is in -dev" || bad "1.27 no man page"
    [ -f "$lib/usr/lib/$(basename "$(dirname "$so")")/libemail-client.a" ] \
        && ok "1.28 the static form of the library is in -dev" || bad "1.28 no libemail-client.a"
    ( cd "$lib" && rm -rf usr ctl && rm -f use use.c ) && rmdir "$lib"
fi

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
