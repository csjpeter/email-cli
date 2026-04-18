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
# Currently no project-wide credentials are configured. Each user
# must register their own OAuth2 app in Google Cloud Console and
# enter their client_id/secret during the Gmail setup wizard, or
# add them to their account config.ini file.
#
# Run 'email-cli help gmail' for the step-by-step setup guide.
#
# To add project-wide credentials in the future, uncomment and fill:
# set(GMAIL_DEFAULT_CLIENT_ID "" CACHE STRING "Gmail OAuth2 client ID")
# set(GMAIL_DEFAULT_CLIENT_SECRET "" CACHE STRING "Gmail OAuth2 client secret")
