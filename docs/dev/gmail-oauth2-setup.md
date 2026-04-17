# Gmail OAuth2 Credentials Setup

This guide walks you through creating Google OAuth2 credentials
for email-cli's Gmail support. You need a `client_id` and
`client_secret` — without these, the Gmail setup wizard cannot
authorize your account.

---

## Step 1: Create a Google Cloud Project

1. Go to [Google Cloud Console](https://console.cloud.google.com/)
2. Sign in with your Google account
3. Click **Select a project** (top bar) → **New Project**
4. Enter a project name (e.g. `email-cli`) → **Create**
5. Wait for the project to be created, then select it

---

## Step 2: Enable the Gmail API

1. Go to **APIs & Services** → **Library** (left sidebar)
   - Or visit: https://console.cloud.google.com/apis/library
2. Search for **Gmail API**
3. Click **Gmail API** → **Enable**

---

## Step 3: Configure the OAuth Consent Screen

1. Go to **APIs & Services** → **OAuth consent screen**
   - Or visit: https://console.cloud.google.com/apis/credentials/consent
2. Select **External** (unless you have a Google Workspace org) → **Create**
3. Fill in the required fields:
   - **App name**: `email-cli`
   - **User support email**: your email address
   - **Developer contact information**: your email address
4. Click **Save and Continue**
5. On the **Scopes** page:
   - Click **Add or Remove Scopes**
   - Search for `https://mail.google.com/`
   - Check the box next to it → **Update** → **Save and Continue**
6. On the **Test users** page:
   - Click **Add Users**
   - Enter your Gmail address (the one you want to use with email-cli)
   - Click **Add** → **Save and Continue**
7. Click **Back to Dashboard**

> **Note:** While the app is in "Testing" status, only the listed test
> users can authorize. This is fine for personal use. Publishing the app
> requires Google's review process.

---

## Step 4: Create OAuth2 Credentials

1. Go to **APIs & Services** → **Credentials**
   - Or visit: https://console.cloud.google.com/apis/credentials
2. Click **+ Create Credentials** → **OAuth client ID**
3. Application type: **Desktop app** (or **TV and Limited Input devices**)
4. Name: `email-cli` (any name is fine)
5. Click **Create**
6. A dialog shows your **Client ID** and **Client secret** — copy both

---

## Step 5: Configure email-cli

Create a config file for your Gmail account with the credentials:

```bash
mkdir -p ~/.config/email-cli/accounts/user@gmail.com
cat > ~/.config/email-cli/accounts/user@gmail.com/config.ini << 'EOF'
GMAIL_CLIENT_ID=your-client-id.apps.googleusercontent.com
GMAIL_CLIENT_SECRET=your-client-secret
EOF
chmod 600 ~/.config/email-cli/accounts/user@gmail.com/config.ini
```

Replace `user@gmail.com` with your actual email address, and paste
your real client_id and client_secret from Step 4.

If the account already exists (e.g. from a failed wizard attempt),
just add the two lines to the existing config file.

---

## Step 6: Run the Gmail Setup Wizard

```bash
bin/email-tui
```

1. Press `n` to add a new account
2. Select `[2] Gmail`
3. Enter your Gmail address
4. The wizard displays a URL and a code:
   ```
   Go to: https://accounts.google.com/device
   Enter code: XXXX-XXXX
   (Waiting for authorization... ^C to cancel)
   ```
5. Open the URL in a browser, enter the code, and authorize
6. The wizard saves the refresh token to your config

---

## Troubleshooting

| Error | Cause | Fix |
|-------|-------|-----|
| `No Gmail OAuth2 client_id configured` | No client_id compiled in or in config | Follow Step 5 |
| `Failed to start Gmail authorization (HTTP 401)` | Invalid client_id | Verify the credential in Google Cloud Console |
| `Authorization failed (invalid_client)` | Invalid client_secret | Verify the secret matches |
| `Authorization timed out` | User didn't complete browser auth within 5 minutes | Try again, authorize faster |
| `Access denied` (in browser) | Email not in test users list | Add your email in Step 3.6 |

---

## Security Notes

- **client_id / client_secret**: These are *not* secret for native/desktop
  apps (Google's own documentation states this). They identify the app,
  not the user. Open-source CLI clients (aerc, himalaya, mutt) all ship
  their client_id in source code.
- **refresh_token**: This IS sensitive. It's stored in `config.ini` with
  mode `0600`. Do not share it.
- **Scope**: `https://mail.google.com/` grants full Gmail access (read,
  send, modify, delete). This is the only scope that covers all email-cli
  operations. There is no narrower scope for IMAP-equivalent access.
