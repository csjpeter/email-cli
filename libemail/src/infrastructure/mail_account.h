/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-15 */
/* Bridge between the program's Config and libemail-client's account. */
#ifndef MAIL_ACCOUNT_H
#define MAIL_ACCOUNT_H

#include <stdio.h>
#include "config.h"
#include "email-client.h"
#include "gmail_login.h"
#include "op_history.h"

#include <stdlib.h>
#include <string.h>

/**
 * @file mail_account.h
 * @brief All contact of the programs with a mail server goes through
 *        libemail-client (EMAIL-99); this header turns the program's Config
 *        into the library's account and opens the client.
 *
 * The library copies what it is given, so the account may point into the
 * Config for the time of the call.
 */

/** @brief Fill @p a from @p cfg (borrowed strings; valid while cfg is). */
static inline void mail_account_from_config(const Config *cfg, emc_account *a)
{
    memset(a, 0, sizeof(*a));
    a->host = cfg->host;
    a->user = cfg->user;
    a->pass = cfg->pass;
    a->folder = cfg->folder;
    a->sent_folder = cfg->sent_folder;
    a->smtp_host = cfg->smtp_host;
    a->smtp_port = cfg->smtp_port;
    a->smtp_user = cfg->smtp_user;
    a->smtp_pass = cfg->smtp_pass;
    a->insecure_tls = cfg->ssl_no_verify;
    a->gmail = cfg->gmail_mode;
    a->gmail_refresh_token = cfg->gmail_refresh_token;
    a->gmail_client_id = cfg->gmail_client_id;
    a->gmail_client_secret = cfg->gmail_client_secret;
}

/**
 * @brief Open a client for @p cfg without connecting (for sending).
 * @return the handle, or NULL when the account is unusable.
 */
static inline emc_client *mail_open(const Config *cfg)
{
    emc_account a;
    emc_client *c = NULL;
    if (!cfg) return NULL;
    mail_account_from_config(cfg, &a);
    return emc_open(&c, &a) == EMC_OK ? c : NULL;
}

/**
 * @brief Open a client for @p cfg and connect it.
 * @return the connected handle, or NULL (the reason is on stderr/log, as the
 *         protocol layer reports it).
 */
static inline emc_client *mail_connect(const Config *cfg)
{
    emc_client *c = mail_open(cfg);
    if (c && emc_connect(c) != EMC_OK) {
        emc_close(c);
        return NULL;
    }
    return c;
}

/**
 * @brief Build the wire form of a message (Date, Message-ID, MIME).
 * @return 0 on success, -1 on failure.
 */
static inline int mail_compose(const emc_message *m, char **wire, size_t *wire_len)
{
    return emc_compose(m, wire, wire_len) == EMC_OK ? 0 : -1;
}

/**
 * @brief Send a message in wire form with the account of @p cfg: SMTP over TLS
 *        for IMAP accounts, the Gmail API for Gmail.
 * @return 0 on success, -1 on failure (the reason is written to stderr as
 *         "Error: send: <reason>").
 */
static inline int mail_send_wire(const Config *cfg, const char *from,
                                 const char *to, const char *wire, size_t len)
{
    emc_client *c = mail_open(cfg);
    if (!c) return -1;
    emc_status st = emc_send_wire(c, from, to, wire, len, NULL);
    /* The library prints nothing (EMAIL-112): the program tells the user why. */
    if (st != EMC_OK) fprintf(stderr, "Error: %s\n", emc_last_error(c));
    emc_close(c);
    /* A sent mail cannot be taken back: recorded, as not revertable. */
    if (st == EMC_OK) op_history_record_send(cfg->user, to, wire);
    return st == EMC_OK ? 0 : -1;
}

/**
 * @brief Authorize a Gmail account (browser, loopback redirect) and store the
 *        new refresh token in @p cfg.  See gmail_login.h for the other ways.
 * @return 0 on success, -1 on failure or when the user gave up.
 */
static inline int mail_gmail_authorize(Config *cfg)
{
    return gmail_login(cfg, GMAIL_LOGIN_BROWSER, stdin);
}

#endif /* MAIL_ACCOUNT_H */
