#ifndef GMAIL_AUTH_H
#define GMAIL_AUTH_H

#include "config.h"

/**
 * @file gmail_auth.h
 * @brief OAuth2 authentication for the Gmail REST API.
 *
 * Provides two flows:
 *   1. Device Authorization Grant (RFC 8628) — interactive first-time login.
 *   2. Token refresh — non-interactive, uses stored refresh_token.
 */

/**
 * @brief Run the OAuth2 Device Authorization Grant flow.
 *
 * Displays a user code and verification URL, then polls Google's token
 * endpoint until the user authorises.  On success sets
 * cfg->gmail_refresh_token (heap-allocated; caller should persist config).
 *
 * Uses cfg->gmail_client_id / gmail_client_secret if set, otherwise
 * falls back to compiled-in defaults.
 *
 * @param cfg  Config with optional client_id/secret overrides.
 * @return 0 on success, -1 on error or user cancel.
 */
int gmail_auth_device_flow(Config *cfg);

/**
 * @brief Obtain a fresh access token using the stored refresh token.
 *
 * @param cfg  Config with gmail_refresh_token set.
 * @return Heap-allocated access token string, or NULL on failure.
 *         Caller must free().
 */
char *gmail_auth_refresh(const Config *cfg);

#endif /* GMAIL_AUTH_H */
