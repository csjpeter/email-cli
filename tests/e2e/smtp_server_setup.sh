#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-13
# Runs inside the e2e container: configures Postfix as the SMTP counterpart.

# Postfix listens for implicit TLS (smtps, port 465) on the loopback address
# with a certificate for mail.example.org that the container's own CA store
# trusts, so email-cli verifies the connection like it does in production.
# Local delivery goes to /var/mail/rcpt.  Pushed and run by smtp_delivery.sh.
set -eu

echo "127.0.0.1 mail.example.org" >> /etc/hosts

openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
    -keyout /etc/ssl/private/mail-test.key \
    -out /usr/local/share/ca-certificates/mail-test.crt \
    -subj "/CN=mail.example.org" \
    -addext "subjectAltName=DNS:mail.example.org"
chmod 0600 /etc/ssl/private/mail-test.key
update-ca-certificates

postconf -e \
    "myhostname = mail.example.org" \
    "mydestination = mail.example.org, localhost" \
    "inet_interfaces = loopback-only" \
    "inet_protocols = ipv4" \
    "smtpd_tls_cert_file = /usr/local/share/ca-certificates/mail-test.crt" \
    "smtpd_tls_key_file = /etc/ssl/private/mail-test.key"
postconf -M "smtps/inet=smtps inet n - y - - smtpd"
postconf -P "smtps/inet/smtpd_tls_wrappermode=yes"

useradd -m -s /bin/sh rcpt
useradd -m -s /bin/sh sender

systemctl restart postfix
i=0
while [ "$i" -lt 30 ]; do
    if bash -c 'exec 3<>/dev/tcp/127.0.0.1/465'; then echo "postfix: smtps is up"; exit 0; fi
    i=$((i + 1)); sleep 1
done
echo "postfix: smtps did not come up" >&2
exit 1
