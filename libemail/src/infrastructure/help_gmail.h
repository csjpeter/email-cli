#ifndef HELP_GMAIL_H
#define HELP_GMAIL_H

#include <stdio.h>

/**
 * @file help_gmail.h
 * @brief Built-in Gmail OAuth2 setup guide, accessible via 'help gmail'.
 *
 * Inline header so both email-cli and email-tui can use it without
 * adding a separate .c file to the build.
 */

static inline void help_gmail(void) {
    printf(
        "Gmail OAuth2 Setup Guide\n"
        "========================\n"
        "\n"
        "email-cli uses the Gmail REST API (not IMAP) for Gmail accounts.\n"
        "You need OAuth2 credentials (client_id and client_secret) from a\n"
        "Google Cloud project.\n"
        "\n"
        "Step 1: Create a Google Cloud Project\n"
        "  1. Go to https://console.cloud.google.com/\n"
        "  2. Click 'Select a project' → 'New Project'\n"
        "  3. Name it (e.g. 'email-cli') → Create\n"
        "\n"
        "Step 2: Enable the Gmail API\n"
        "  1. Go to APIs & Services → Library\n"
        "  2. Search 'Gmail API' → Enable\n"
        "\n"
        "Step 3: Configure OAuth Consent Screen\n"
        "  1. Go to APIs & Services → OAuth consent screen\n"
        "  2. Select 'External' → Create\n"
        "  3. Fill in app name, support email, developer email\n"
        "  4. Add scope: https://mail.google.com/\n"
        "  5. Add your Gmail address as a test user\n"
        "\n"
        "Step 4: Create Credentials\n"
        "  1. Go to APIs & Services → Credentials\n"
        "  2. Create Credentials → OAuth client ID\n"
        "  3. Application type: 'Desktop app'\n"
        "  4. Copy the Client ID and Client Secret\n"
        "\n"
        "Step 5: Add Account\n"
        "  Run the setup wizard (press 'n' in email-tui, or run email-cli\n"
        "  without config). Select account type [2] Gmail. The wizard will\n"
        "  ask for your Client ID and Client Secret, then guide you through\n"
        "  the authorization flow.\n"
        "\n"
        "  Alternatively, create the config file manually:\n"
        "    ~/.config/email-cli/accounts/<email>/config.ini\n"
        "  with:\n"
        "    GMAIL_MODE=1\n"
        "    EMAIL_USER=you@gmail.com\n"
        "    GMAIL_CLIENT_ID=<your-client-id>.apps.googleusercontent.com\n"
        "    GMAIL_CLIENT_SECRET=<your-client-secret>\n"
        "\n"
        "Security notes:\n"
        "  - client_id/secret identify the app, not the user (not truly secret\n"
        "    for native apps — this follows Google's own guidelines)\n"
        "  - refresh_token IS sensitive — stored in config.ini with mode 0600\n"
        "  - Scope grants full Gmail access (read, send, modify, delete)\n"
    );
}

#endif /* HELP_GMAIL_H */
