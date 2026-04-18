# Gmail OAuth2 credentials for the email-cli project.
#
# Per Google's OAuth2 documentation, native/desktop app credentials
# are not secret — they identify the application, not any user:
#
#   "Installed apps are distributed to individual devices, and it is
#    assumed that these apps cannot keep secrets."
#
# Reference: https://developers.google.com/identity/protocols/oauth2
#
# Project-wide credentials (Desktop app, published, 100 user cap).
# Users can override with their own credentials in config.ini.
# Run 'email-cli help gmail' for the setup guide.
set(GMAIL_DEFAULT_CLIENT_ID "326922622254-hdcevm96tkcltqfvaamtsiucd3h21fib.apps.googleusercontent.com" CACHE STRING "Gmail OAuth2 client ID")
set(GMAIL_DEFAULT_CLIENT_SECRET "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" CACHE STRING "Gmail OAuth2 client secret")
