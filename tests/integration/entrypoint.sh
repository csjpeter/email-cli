#!/bin/bash
# Seed emails into Maildir on first start (volume is empty)
if [ -z "$(ls -A /home/testuser/Maildir/new 2>/dev/null)" ]; then
    echo "[entrypoint] Seeding test emails into Maildir..."
    cp /seed/* /home/testuser/Maildir/new/
    chown testuser:testuser /home/testuser/Maildir/new/*
    echo "[entrypoint] Seeded $(ls /home/testuser/Maildir/new | wc -l) email(s)."
else
    echo "[entrypoint] Maildir already has emails, skipping seed."
fi

exec "$@"
