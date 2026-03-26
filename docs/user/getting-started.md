# Getting Started

## Requirements

- Linux (Ubuntu 24.04 or Rocky Linux 9 tested)
- GCC, CMake
- libcurl with OpenSSL backend
- An IMAP email account (Gmail, Fastmail, self-hosted, etc.)

## Install Dependencies

```bash
./manage.sh deps
```

This installs all required packages for Ubuntu 24.04 or Rocky Linux 9.
For other distributions, install manually:

```bash
# Debian/Ubuntu
sudo apt-get install build-essential cmake libcurl4-openssl-dev libssl-dev

# Fedora/RHEL/Rocky
sudo dnf install gcc cmake libcurl-devel openssl-devel
```

## Build

```bash
./manage.sh build
```

The binary is placed at `./bin/email-cli`.

## First Run

```bash
./bin/email-cli
```

On first run, the setup wizard starts automatically:

```
--- email-cli Configuration Wizard ---
Please enter your email server details.

IMAP Host (e.g., imaps://imap.example.com): imaps://imap.gmail.com
Email Username: you@gmail.com
Email Password: ****************
Default Folder [INBOX]:
```

Configuration is saved to `~/.config/email-cli/config.ini` with mode `0600`
(readable only by you).

## Typical Output

```
--- Fetching recent emails from imaps://imap.gmail.com/INBOX ---
Showing 3 most recent of 47 message(s).

══════════════════════════════════════════
 Message 1/3  (UID 147)
══════════════════════════════════════════
Subject: Weekly report
...

══════════════════════════════════════════
 Message 2/3  (UID 146)
══════════════════════════════════════════
...

Fetch complete. Success.

Success: Fetch complete.
```

Up to 10 most recent messages are shown per run.

## Provider-Specific Notes

| Provider | IMAP host | Notes |
|----------|-----------|-------|
| Gmail | `imaps://imap.gmail.com` | Enable IMAP in settings; use an App Password if 2FA is on |
| Outlook/Hotmail | `imaps://outlook.office365.com` | Use your full email as username |
| Fastmail | `imaps://imap.fastmail.com` | Standard credentials |
| Self-hosted | `imaps://mail.yourdomain.com` | May need `SSL_NO_VERIFY=1` if self-signed cert |
