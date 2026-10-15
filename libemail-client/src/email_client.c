/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-15 */
/* The public interface of libemail-client over the mail client modules. */
#include "email-client.h"

#include "compose_service.h"
#include "config.h"
#include "mail_client.h"
#include "mime_util.h"
#include "smtp_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file email_client.c
 * @brief Facade of libemail-client: the exported emc_* functions.
 *
 * Everything else the library contains is hidden (-fvisibility=hidden); this
 * file is the interface.  It audits nothing (policies/
 * local-service-access-model.md, "A client library does not audit").
 */

#define EMC_API __attribute__((visibility("default")))

#ifndef EMC_VERSION_STRING
#define EMC_VERSION_STRING "0.0.0"
#endif

struct emc_client {
    Config      *cfg;       /* owned copy of the account */
    MailClient  *mc;        /* connected lazily; NULL until needed */
    char         err[256];
};

EMC_API const char *emc_version(void) { return EMC_VERSION_STRING; }

EMC_API const char *emc_strerror(emc_status status)
{
    switch (status) {
    case EMC_OK:           return "success";
    case EMC_ERR_ARGUMENT: return "invalid argument";
    case EMC_ERR_CONFIG:   return "the account lacks what the operation needs";
    case EMC_ERR_CONNECT:  return "cannot connect to or log in at the server";
    case EMC_ERR_SEND:     return "the server did not accept the message";
    case EMC_ERR_PROTOCOL: return "unusable answer from the server";
    case EMC_ERR_MEMORY:   return "out of memory";
    }
    return "unknown status";
}

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

static emc_status fail(emc_client *c, emc_status st, const char *what)
{
    snprintf(c->err, sizeof(c->err), "%s: %s", what, emc_strerror(st));
    return st;
}

EMC_API emc_status emc_open(emc_client **out, const emc_account *a)
{
    if (!out) return EMC_ERR_ARGUMENT;
    *out = NULL;
    if (!a) return EMC_ERR_ARGUMENT;
    if (!a->gmail && (!a->host || !a->host[0] || !a->user || !a->user[0]))
        return EMC_ERR_ARGUMENT;
    if (a->gmail && (!a->gmail_refresh_token || !a->gmail_refresh_token[0]))
        return EMC_ERR_ARGUMENT;

    emc_client *c = calloc(1, sizeof(*c));
    Config *cfg = calloc(1, sizeof(*cfg));
    if (!c || !cfg) { free(c); free(cfg); return EMC_ERR_MEMORY; }

    cfg->host        = dup_or_null(a->host);
    cfg->user        = dup_or_null(a->user);
    cfg->pass        = dup_or_null(a->pass);
    cfg->folder      = strdup(a->folder && a->folder[0] ? a->folder : "INBOX");
    cfg->sent_folder = dup_or_null(a->sent_folder);
    cfg->smtp_host   = dup_or_null(a->smtp_host);
    cfg->smtp_port   = a->smtp_port;
    cfg->smtp_user   = dup_or_null(a->smtp_user);
    cfg->smtp_pass   = dup_or_null(a->smtp_pass);
    cfg->ssl_no_verify = a->insecure_tls ? 1 : 0;
    cfg->gmail_mode  = a->gmail ? 1 : 0;
    cfg->gmail_refresh_token  = dup_or_null(a->gmail_refresh_token);
    cfg->gmail_client_id      = dup_or_null(a->gmail_client_id);
    cfg->gmail_client_secret  = dup_or_null(a->gmail_client_secret);
    if (!cfg->folder) { config_free(cfg); free(c); return EMC_ERR_MEMORY; }

    c->cfg = cfg;
    *out = c;
    return EMC_OK;
}

EMC_API void emc_close(emc_client *c)
{
    if (!c) return;
    mail_client_free(c->mc);
    config_free(c->cfg);
    free(c);
}

EMC_API const char *emc_last_error(const emc_client *c)
{
    return c ? c->err : "";
}

static emc_status connect_now(emc_client *c)
{
    if (c->mc) return EMC_OK;
    c->mc = mail_client_connect(c->cfg);
    if (!c->mc) return fail(c, EMC_ERR_CONNECT, "connect");
    return EMC_OK;
}

static int blank(const char *s) { return !s || !s[0]; }

EMC_API emc_status emc_send(emc_client *c, const emc_message *m,
                            emc_send_result *res)
{
    emc_send_result local;
    if (!res) res = &local;
    memset(res, 0, sizeof(*res));
    if (!c || !m) { res->status = EMC_ERR_ARGUMENT; return res->status; }
    c->err[0] = '\0';

    if (blank(m->to) || !m->subject || !m->body ||
        m->attachment_count < 0 || (m->attachment_count > 0 && !m->attachments)) {
        res->status = fail(c, EMC_ERR_ARGUMENT, "send");
        return res->status;
    }
    const char *from = m->from ? m->from
                     : c->cfg->smtp_user ? c->cfg->smtp_user : c->cfg->user;
    if (blank(from)) {
        res->status = fail(c, EMC_ERR_CONFIG, "send (no sender)");
        return res->status;
    }

    ComposeParams p = {
        .from = from, .to = m->to, .cc = m->cc, .bcc = m->bcc,
        .subject = m->subject, .body = m->body,
        .reply_to_msg_id = m->in_reply_to,
        .attachments = m->attachment_count > 0 ? (const char **)m->attachments : NULL,
        .attach_count = m->attachment_count,
    };
    char *wire = NULL;
    size_t wire_len = 0;
    if (compose_build_message(&p, &wire, &wire_len) != 0) {
        res->status = fail(c, EMC_ERR_ARGUMENT, "send (cannot build the message)");
        return res->status;
    }

    int rc;
    if (c->cfg->gmail_mode) {
        emc_status st = connect_now(c);
        if (st != EMC_OK) { free(wire); res->status = st; return st; }
        rc = mail_client_append(c->mc, NULL, wire, wire_len);
    } else {
        rc = smtp_send(c->cfg, from, m->to, wire, wire_len);
    }
    if (rc != 0) {
        free(wire);
        res->status = fail(c, EMC_ERR_SEND, "send");
        return res->status;
    }

    char *id = mime_get_header(wire, "Message-ID");
    if (id) {
        snprintf(res->message_id, sizeof(res->message_id), "%s", id);
        free(id);
    }
    if (!c->cfg->gmail_mode && !blank(c->cfg->sent_folder) &&
        connect_now(c) == EMC_OK &&
        mail_client_append(c->mc, c->cfg->sent_folder, wire, wire_len) == 0)
        res->saved_copy = 1;
    free(wire);
    res->status = EMC_OK;
    return EMC_OK;
}

EMC_API void emc_free_list(char **items, int count)
{
    if (!items) return;
    for (int i = 0; i < count; i++) free(items[i]);
    free(items);
}

EMC_API emc_status emc_list_folders(emc_client *c, char ***names, int *count)
{
    if (!c || !names || !count) return EMC_ERR_ARGUMENT;
    *names = NULL;
    *count = 0;
    c->err[0] = '\0';
    emc_status st = connect_now(c);
    if (st != EMC_OK) return st;
    if (mail_client_list(c->mc, names, count, NULL) != 0) {
        *names = NULL;
        *count = 0;
        return fail(c, EMC_ERR_PROTOCOL, "list folders");
    }
    return EMC_OK;
}

EMC_API emc_status emc_list_messages(emc_client *c, const char *folder,
                                     int unseen_only, char ***ids, int *count)
{
    if (!c || !ids || !count) return EMC_ERR_ARGUMENT;
    *ids = NULL;
    *count = 0;
    c->err[0] = '\0';
    emc_status st = connect_now(c);
    if (st != EMC_OK) return st;
    if (mail_client_select(c->mc, blank(folder) ? c->cfg->folder : folder) != 0)
        return fail(c, EMC_ERR_PROTOCOL, "select folder");

    char (*uids)[17] = NULL;
    int n = 0;
    if (mail_client_search(c->mc, unseen_only ? MAIL_SEARCH_UNREAD : MAIL_SEARCH_ALL,
                           &uids, &n) != 0)
        return fail(c, EMC_ERR_PROTOCOL, "search");

    char **out = calloc((size_t)(n > 0 ? n : 1), sizeof(*out));
    if (!out) { free(uids); return fail(c, EMC_ERR_MEMORY, "list messages"); }
    for (int i = 0; i < n; i++) {
        out[i] = strdup(uids[i]);
        if (!out[i]) {
            emc_free_list(out, i);
            free(uids);
            return fail(c, EMC_ERR_MEMORY, "list messages");
        }
    }
    free(uids);
    *ids = out;
    *count = n;
    return EMC_OK;
}

EMC_API emc_status emc_fetch_message(emc_client *c, const char *folder,
                                     const char *id, char **raw)
{
    if (!c || !raw) return EMC_ERR_ARGUMENT;
    *raw = NULL;
    if (blank(id)) return fail(c, EMC_ERR_ARGUMENT, "fetch");
    c->err[0] = '\0';
    emc_status st = connect_now(c);
    if (st != EMC_OK) return st;
    if (mail_client_select(c->mc, blank(folder) ? c->cfg->folder : folder) != 0)
        return fail(c, EMC_ERR_PROTOCOL, "select folder");
    char *msg = mail_client_fetch_body(c->mc, id);
    if (!msg) return fail(c, EMC_ERR_PROTOCOL, "fetch");
    *raw = msg;
    return EMC_OK;
}
