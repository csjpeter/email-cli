#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-03-25
# Seeds the Dovecot test container's Maildir on first start.

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
