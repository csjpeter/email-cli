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
set(GMAIL_DEFAULT_CLIENT_ID "326922622254-hdcevm96tkcltqfvaamtsiucd3h21fib.apps.googleusercontent.com" CACHE STRING "Gmail OAuth2 client ID")
set(GMAIL_DEFAULT_CLIENT_SECRET "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" CACHE STRING "Gmail OAuth2 client secret")
