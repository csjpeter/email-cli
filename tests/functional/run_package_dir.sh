#!/bin/bash
# Checks that `manage.sh package` leaves exactly one run's packages in
# build/packages/ and nothing there when the run fails (EMAIL-18).
# Run from anywhere after a build; it does its own release build.
set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT="$PWD"
PKG="$ROOT/build/packages"
STUBS="$ROOT/build/package-test-stubs-$$"
PASSED=0; FAILED=0

ok()   { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad()  { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }
count() { find "$PKG" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.rpm' \) | wc -l; }

mkdir -p "$PKG" || exit 1

# 1. A stale package from an older run is removed, the new one is the only one.
STALE="$PKG/email-cli_0.0.1_amd64.deb"
echo stale > "$STALE" || { bad "1.0 stale fixture could not be created"; exit 1; }
out=$("$ROOT/manage.sh" package deb 2>&1); rc=$?
[ "$rc" -eq 0 ]            && ok "1.1 package deb exits 0"            || { bad "1.1 package deb exits $rc"; echo "$out" | tail -5; }
[ ! -e "$STALE" ]          && ok "1.2 stale package is gone"          || bad "1.2 stale package survived"
[ "$(count)" -eq 1 ]       && ok "1.3 exactly one package present"    || bad "1.3 package count is $(count)"
[ "$(find "$PKG" -mindepth 1 | wc -l)" -eq 1 ] && ok "1.5 nothing but the package is there" || bad "1.5 extra entries: $(ls "$PKG" | tr '\n' ' ')"
ls "$PKG"/email-cli_*_amd64.deb > /dev/null && ok "1.4 it is the email-cli deb" || bad "1.4 no email-cli deb"

# 2. A failing cpack leaves the directory empty and the run non-zero.
mkdir -p "$STUBS" || exit 1
printf '#!/bin/sh\necho "stub cpack: refusing" >&2\nexit 1\n' > "$STUBS/cpack"
chmod 755 "$STUBS/cpack"
out=$(PATH="$STUBS:$PATH" "$ROOT/manage.sh" package deb 2>&1); rc=$?
[ "$rc" -ne 0 ]            && ok "2.1 failed packaging exits non-zero" || bad "2.1 exited 0 although cpack failed"
[ "$(find "$PKG" -mindepth 1 | wc -l)" -eq 0 ] && ok "2.2 build/packages is empty" || bad "2.2 left after failure: $(ls "$PKG" | tr '\n' ' ')"
echo "$out" | grep -q "packaging failed" && ok "2.3 the failure is explained" || bad "2.3 no explanation in: $(echo "$out" | tail -2)"

( cd "$STUBS" && rm -f cpack ) && rmdir "$STUBS"

echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
