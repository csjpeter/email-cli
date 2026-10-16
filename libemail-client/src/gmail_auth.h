/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-04-17 */
/* Interface of the Gmail OAuth2 authentication. */
#ifndef GMAIL_AUTH_H
#define GMAIL_AUTH_H

#include "config.h"

/**
 * @file gmail_auth.h
 * @brief OAuth2 authentication for the Gmail REST API.
 *
 * Provides two flows:
 *   1. Authorization Code Grant with a loopback redirect (RFC 8252, the
 *      "installed application" flow) — interactive first-time login.  It is
 *      not the device flow (RFC 8628): Google's device flow admits only a
 *      short list of scopes, and the full Gmail scope (https://mail.google.com/)
 *      is not among them (docs/spec/gmail-api.md, "Authorization flow").
 *   2. Token refresh — non-interactive, uses stored refresh_token.
 *
 * The library never writes to the terminal and never starts a browser: the
 * program supplies the callbacks in GmailAuthUi and does both itself.
 */

/** What the program supplies to an authorization, and where the reason lands. */
typedef struct GmailAuthUi {
    /** 0 = listen on 127.0.0.1 (ports 8089-8099) for the redirect;
     *  1 = paste mode: no listener, the person pastes the redirect address. */
    int   paste;
    /** Show @p url to the person (required).  @p port is the redirect port
     *  (the one an SSH forward must carry in loopback mode). */
    void (*show_url)(const char *url, int port, void *user);
    /** Paste mode: the text the person pasted (redirect URL, query or bare
     *  code), heap allocated, or NULL when none was given. */
    char *(*read_redirect)(void *user);
    void *user;
    /** Out: why the authorization failed (empty on success). */
    char  error[512];
} GmailAuthUi;

/**
 * @brief Authorize the account: URL out through @p ui->show_url, code back by
 *        loopback redirect or pasted text, exchanged for tokens.
 *
 * On success sets cfg->gmail_refresh_token (heap-allocated; the caller
 * persists the config).  Uses cfg->gmail_client_id / gmail_client_secret if
 * set, otherwise the compiled-in defaults.
 *
 * @return 0 on success, -1 on error or denial with the reason in ui->error.
 */
int gmail_auth_authorize(Config *cfg, GmailAuthUi *ui);

/**
 * @brief Obtain a fresh access token using the stored refresh token.
 *
 * @param cfg  Config with gmail_refresh_token set.
 * @return Heap-allocated access token string, or NULL on failure.
 *         Caller must free().
 */
char *gmail_auth_refresh(const Config *cfg);

#endif /* GMAIL_AUTH_H */
