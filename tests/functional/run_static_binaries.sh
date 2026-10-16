#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-16
# Checks that the programs are fully static and run in an empty root.

# EMAIL-103: email-cli and email-tui are one self-contained binary each: no
# shared library, no dynamic loader.  Proof that it runs on a system with
# nothing installed: the binary is copied alone into an otherwise empty root
# directory (only /etc/hosts, /etc/resolv.conf and a CA bundle, what any
# distribution has) and run there, in a user namespace, against the mock IMAP
# and SMTP servers with certificate verification on.  Name resolution, the CA
# bundle lookup and TLS therefore work with nothing but the binary.
# Needs the release build (bin/) and user namespaces.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
BIN="$ROOT/bin"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }

WORK="$ROOT/build/static-binaries-$$"
mkdir -p "$WORK" || exit 1
PIDS=""
cleanup() {
    local p
    for p in $PIDS; do
        pkill -P "$p"; [ "$?" -gt 1 ] && echo "pkill -P $p failed" >&2
        [ -d "/proc/$p" ] && kill "$p"
    done
    [ -n "$WORK" ] && [ -d "$WORK" ] && ( cd "$WORK" && rm -rf root mocks ports ) && rmdir "$WORK"
}
trap cleanup EXIT

# 1. Every program is statically linked.
for b in email-cli email-cli-ro email-sync email-tui email-import-rules; do
    [ -x "$BIN/$b" ] || { bad "1.0 $BIN/$b is not built"; continue; }
    interp=$(readelf -l "$BIN/$b" | grep -c INTERP)
    needed=$(readelf -d "$BIN/$b" 2>&1 | grep -c "Shared library")
    [ "$interp" = "0" ] && [ "$needed" = "0" ] \
        && ok "1.1 $b: no dynamic loader and no shared library" \
        || bad "1.1 $b: interpreter=$interp shared libraries=$needed"
done

# 2. An empty root with the binary alone.
mkdir -p "$WORK/root/etc/ssl/certs" "$WORK/root/home/u/.config/email-cli/accounts/testuser" \
         "$WORK/root/tmp" "$WORK/mocks" "$WORK/ports" || exit 1
cp "$BIN/email-cli" "$WORK/root/email-cli" || exit 1
printf '127.0.0.1 localhost\n' > "$WORK/root/etc/hosts"
printf 'nameserver 127.0.0.1\n' > "$WORK/root/etc/resolv.conf"
cp "$ROOT/build/tests/certs/test.crt" "$WORK/root/etc/ssl/certs/ca-certificates.crt" || exit 1

if unshare -Urm true; then
    ok "2.0 user namespaces are available"
else
    bad "2.0 unshare -Urm does not work: the empty-root run needs user namespaces"
    echo "Passed: $PASSED / $((PASSED + FAILED))"
    exit 1
fi
# inroot <command...>: run in the empty root with a clean environment (env is
# outside the root, so it runs before the chroot)
inroot() {
    ( cd "$WORK" && env -i HOME=/home/u PATH=/usr/sbin:/usr/bin:/sbin:/bin unshare -Urm chroot "$WORK/root" "$@" 2>&1 )
}

out=$(inroot /email-cli --version)
case "$out" in email-cli\ *) ok "2.1 the binary alone runs in an empty root: $out" ;; *) bad "2.1 --version in the empty root: $out" ;; esac
ls "$WORK/root" | tr '\n' ' ' | grep -q '^email-cli etc home tmp $' \
    && ok "2.2 the root holds the binary, /etc, /home and /tmp, nothing else" || bad "2.2 root: $(ls "$WORK/root" | tr '\n' ' ')"

# 3. Name resolution, the CA bundle and TLS in that root, against mock servers.
gcc "$ROOT/tests/functional/mock_imap_server.c" -o "$WORK/mocks/imap" -lssl -lcrypto || exit 1
gcc "$ROOT/tests/pty/mock_smtp_server.c"        -o "$WORK/mocks/smtp" -lssl -lcrypto || exit 1
gcc "$ROOT/tests/functional/free_port.c"        -o "$WORK/mocks/free_port" || exit 1
IMAP_PORT=$("$WORK/mocks/free_port") || exit 1
SMTP_PORT=$("$WORK/mocks/free_port") || exit 1
( cd "$ROOT/build" && MOCK_IMAP_PORT=$IMAP_PORT MOCK_IMAP_SUBJECT="=?ISO-8859-2?Q?Static=E1?= RootMsg" "$WORK/mocks/imap" ) >"$WORK/ports/imap.log" 2>&1 &
PIDS="$PIDS $!"
( cd "$ROOT/build" && MOCK_SMTP_PORT=$SMTP_PORT "$WORK/mocks/smtp" ) >"$WORK/ports/smtp.log" 2>&1 &
PIDS="$PIDS $!"
sleep 1

cat > "$WORK/root/home/u/.config/email-cli/accounts/testuser/config.ini" <<INI
EMAIL_HOST=imaps://localhost:$IMAP_PORT
EMAIL_USER=testuser@example.com
EMAIL_PASS=testpass
EMAIL_FOLDER=INBOX
SMTP_HOST=smtps://localhost:$SMTP_PORT
SMTP_PORT=$SMTP_PORT
SMTP_USER=testuser@example.com
SMTP_PASS=testpass
INI
chmod 0600 "$WORK/root/home/u/.config/email-cli/accounts/testuser/config.ini"

out=$(inroot /email-cli list)
case "$out" in *"Staticá RootMsg"*) ok "3.1 IMAP over verified TLS works in the empty root (resolver, CA bundle, TLS), and the ISO-8859-2 subject is decoded without iconv modules" ;;
               *) bad "3.1 list in the empty root: $out" ;; esac
out=$(inroot /email-cli send --to alice@example.com --subject "Static" --body "from an empty root")
case "$out" in *[Ss]ent*|*queued*) ok "3.2 SMTP over verified TLS works in the empty root" ;;
               *) bad "3.2 send in the empty root: $out" ;; esac

# 4. With no CA bundle at all the certificate is not trusted: verification is
# real, not skipped.
rm -f "$WORK/root/etc/ssl/certs/ca-certificates.crt"
out=$(inroot /email-cli list)
case "$out" in *RootMsg*) bad "3.3 a server was trusted without any CA bundle" ;;
               *) ok "3.3 without a CA bundle the server is not trusted" ;; esac

echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
