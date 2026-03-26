FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    dovecot-imapd \
    netcat-openbsd \
    openssl \
    && rm -rf /var/lib/apt/lists/*

# Create test user (uid/gid 1000)
RUN useradd -m -u 1000 -s /bin/bash testuser

# Generate self-signed certificate for IMAPS (TLS 1.2+)
RUN mkdir -p /etc/dovecot/private && \
    openssl req -x509 -newkey rsa:2048 \
        -keyout /etc/dovecot/private/dovecot.pem \
        -out /etc/dovecot/dovecot.pem \
        -days 3650 -nodes \
        -subj "/CN=mail.test.local"

# Dovecot configuration
COPY dovecot/dovecot.conf /etc/dovecot/dovecot.conf
COPY dovecot/users        /etc/dovecot/users
RUN chmod 640 /etc/dovecot/users && chown root:dovecot /etc/dovecot/users

# Maildir skeleton
RUN mkdir -p /home/testuser/Maildir/{new,cur,tmp} \
    && chown -R testuser:testuser /home/testuser/Maildir

# Seed emails baked into the image
COPY seed/ /seed/

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 143 993

ENTRYPOINT ["/entrypoint.sh"]
CMD ["dovecot", "-F"]
