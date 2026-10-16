/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* The programs' side of the Gmail authorization: what is shown and asked. */
#ifndef GMAIL_LOGIN_H
#define GMAIL_LOGIN_H

#include <stdio.h>
#include "config.h"

/**
 * @file gmail_login.h
 * @brief Terminal part of the Gmail authorization (EMAIL-113).
 *
 * libemail-client prints nothing and starts no browser; this module is the
 * program that does.  Three ways to obtain the refresh token:
 *
 *  - loopback (default): the URL is shown and a browser is started; the
 *    library listens on 127.0.0.1:8089..8099 for the redirect.  On a headless
 *    host the same works through an SSH forward of that port.
 *  - paste: the URL is shown, the person opens it on any machine and pastes
 *    the address the browser was redirected to (or just the code).
 *  - import: a refresh token obtained on another machine is read from the
 *    input and checked.
 */

/** How gmail_login() gets the authorization code. */
typedef enum {
    GMAIL_LOGIN_BROWSER,    /**< loopback listener, start xdg-open */
    GMAIL_LOGIN_LOOPBACK,   /**< loopback listener, only show the URL */
    GMAIL_LOGIN_PASTE       /**< show the URL, read the pasted redirect */
} GmailLoginMode;

/**
 * @brief Authorize @p cfg's account and store the new refresh token in it.
 *
 * Messages for the person go to stderr; @p in is where a pasted redirect is
 * read from.  A failure is reported on stderr as "Error: <reason>".
 * @return 0 on success, -1 on failure or denial.
 */
int gmail_login(Config *cfg, GmailLoginMode mode, FILE *in);

/**
 * @brief Take over a refresh token obtained on another machine.
 *
 * Reads one line from @p in (the token; surrounding blanks are dropped),
 * and puts it into @p cfg->gmail_refresh_token.  Nothing is checked against
 * Google here: the caller tests the account before saving.
 * @return 0 on success, -1 when the line is empty or holds blanks inside.
 */
int gmail_login_import_token(Config *cfg, FILE *in);

#endif /* GMAIL_LOGIN_H */
