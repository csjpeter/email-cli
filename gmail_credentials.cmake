# Gmail OAuth2 credentials for the email-cli project.
#
# These identify the application, not any user account. Per Google's
# OAuth2 documentation, native/desktop app credentials are not secret:
#
#   "Installed apps are distributed to individual devices, and it is
#    assumed that these apps cannot keep secrets."
#
#   "In this context, the client secret is obviously not treated as
#    a secret."
#
# Reference: https://developers.google.com/identity/protocols/oauth2
#
# User accounts are protected by individual refresh tokens stored in
# ~/.config/email-cli/accounts/<email>/config.ini (mode 0600).
# See 'email-cli help gmail' for setup details.
set(GMAIL_DEFAULT_CLIENT_ID "326922622254-hd0b765n60j4kfhfi0bviqj9g2qqebs2.apps.googleusercontent.com" CACHE STRING "Gmail OAuth2 client ID")
set(GMAIL_DEFAULT_CLIENT_SECRET "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" CACHE STRING "Gmail OAuth2 client secret")
