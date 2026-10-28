/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-15 */
/* The public interface of libemail-client over the mail client modules. */
#include "email-client.h"

#include "compose_service.h"
#include "config.h"
#include "config_store.h"
#include "gmail_auth.h"
#include "gmail_sync.h"
#include "local_store.h"
#include "logger.h"
#include "mail_client.h"
#include "mime_util.h"
#include "smtp_adapter.h"
#include "tls_revocation.h"

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
    char         err[512];
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
    case EMC_ERR_NO_ACCOUNT: return "no such account";
    case EMC_ERR_CREDENTIAL: return "a stored credential cannot be decrypted";
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

/* The same with the reason the protocol layer gave ("send: Could not connect
 * to server"); the plain form when it gave none. */
static emc_status fail_why(emc_client *c, emc_status st, const char *what, const char *why)
{
    if (!why || !why[0]) return fail(c, st, what);
    snprintf(c->err, sizeof(c->err), "%s: %s", what, why);
    return st;
}

static void set_err(char *err, size_t len, const char *fmt, const char *name,
                    const char *why)
{
    if (err && len) snprintf(err, len, fmt, name, why);
}

EMC_API void emc_account_free(emc_account *a)
{
    if (!a) return;
    free((void *)a->host);
    free((void *)a->user);
    if (a->pass) { memset((void *)a->pass, 0, strlen(a->pass)); free((void *)a->pass); }
    free((void *)a->folder);
    free((void *)a->sent_folder);
    free((void *)a->smtp_host);
    free((void *)a->smtp_user);
    if (a->smtp_pass) { memset((void *)a->smtp_pass, 0, strlen(a->smtp_pass)); free((void *)a->smtp_pass); }
    if (a->gmail_refresh_token) {
        memset((void *)a->gmail_refresh_token, 0, strlen(a->gmail_refresh_token));
        free((void *)a->gmail_refresh_token);
    }
    free((void *)a->gmail_client_id);
    free((void *)a->gmail_client_secret);
    memset(a, 0, sizeof(*a));
}

EMC_API emc_status emc_account_load(const char *name, emc_account *out,
                                    char *err, size_t err_len)
{
    if (err && err_len) err[0] = '\0';
    if (!out) return EMC_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!name || !name[0]) {
        set_err(err, err_len, "%s%s", "", "account name missing");
        return EMC_ERR_ARGUMENT;
    }
    int reason = 0;
    Config *cfg = config_load_account_quiet(name, &reason);
    if (!cfg) {
        switch (reason) {
        case CFG_LOAD_UNDECRYPTABLE:
            set_err(err, err_len, "account '%s': %s", name,
                    "a stored credential cannot be decrypted (the installation key changed?)");
            return EMC_ERR_CREDENTIAL;
        case CFG_LOAD_UNREADABLE:
            set_err(err, err_len, "account '%s': %s", name,
                    "the configuration cannot be read, is incomplete or names a server without TLS");
            return EMC_ERR_CONFIG;
        default:
            set_err(err, err_len, "account '%s': %s", name, "no such account");
            return EMC_ERR_NO_ACCOUNT;
        }
    }
    out->host = dup_or_null(cfg->host);
    out->user = dup_or_null(cfg->user);
    out->pass = dup_or_null(cfg->pass);
    out->folder = dup_or_null(cfg->folder);
    out->sent_folder = dup_or_null(cfg->sent_folder);
    out->smtp_host = dup_or_null(cfg->smtp_host);
    out->smtp_port = cfg->smtp_port;
    out->smtp_user = dup_or_null(cfg->smtp_user);
    out->smtp_pass = dup_or_null(cfg->smtp_pass);
    out->gmail = cfg->gmail_mode;
    out->gmail_refresh_token = dup_or_null(cfg->gmail_refresh_token);
    out->gmail_client_id = dup_or_null(cfg->gmail_client_id);
    out->gmail_client_secret = dup_or_null(cfg->gmail_client_secret);
    out->insecure_tls = cfg->ssl_no_verify;
    int oom = (cfg->host && !out->host) || (cfg->user && !out->user) ||
              (cfg->pass && !out->pass) || (cfg->folder && !out->folder) ||
              (cfg->smtp_pass && !out->smtp_pass);
    config_free(cfg);
    if (oom) {
        emc_account_free(out);
        set_err(err, err_len, "%s%s", "", "out of memory");
        return EMC_ERR_MEMORY;
    }
    return EMC_OK;
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
    char why[640] = "";
    if (c->cfg->gmail_mode) {
        emc_status st = connect_now(c);
        if (st != EMC_OK) { free(wire); res->status = st; return st; }
        rc = mail_client_append(c->mc, NULL, wire, wire_len);
    } else {
        rc = smtp_send(c->cfg, from, m->to, wire, wire_len, why, sizeof(why));
    }
    if (rc != 0) {
        free(wire);
        res->status = fail_why(c, EMC_ERR_SEND, "send", why);
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

/* ── Mail access: the full operation set ───────────────────────────────
 * Thin wrappers over the dispatch layer (mail_client.c): the same
 * behaviour, one status vocabulary. */

static emc_status ok_or(emc_client *c, int rc, emc_status st, const char *what)
{
    return rc == 0 ? EMC_OK : fail(c, st, what);
}

/* Common prologue: a connected client, the error text cleared. */
#define EMC_NEED(c, what)                                                   \
    do {                                                                    \
        if (!(c)) return EMC_ERR_ARGUMENT;                                  \
        (c)->err[0] = '\0';                                                 \
        emc_status _st = connect_now(c);                                    \
        if (_st != EMC_OK) return _st;                                      \
        (void)(what);                                                       \
    } while (0)

EMC_API emc_status emc_connect(emc_client *c)
{
    if (!c) return EMC_ERR_ARGUMENT;
    c->err[0] = '\0';
    return connect_now(c);
}

EMC_API int emc_uses_labels(const emc_client *c)
{
    return c && c->mc ? mail_client_uses_labels(c->mc) : (c ? c->cfg->gmail_mode : 0);
}

EMC_API void emc_set_progress(emc_client *c, emc_progress_fn fn, void *ctx)
{
    if (c && c->mc) mail_client_set_progress(c->mc, fn, ctx);
}

EMC_API emc_status emc_list_names(emc_client *c, char ***names, int *count, char *sep)
{
    if (!names || !count) return EMC_ERR_ARGUMENT;
    *names = NULL;
    *count = 0;
    EMC_NEED(c, "list");
    if (mail_client_list(c->mc, names, count, sep) != 0) {
        *names = NULL;
        *count = 0;
        return fail(c, EMC_ERR_PROTOCOL, "list folders");
    }
    return EMC_OK;
}

EMC_API emc_status emc_list_with_ids(emc_client *c, char ***names, char ***ids,
                                     int *count)
{
    if (!names || !ids || !count) return EMC_ERR_ARGUMENT;
    *names = NULL;
    *ids = NULL;
    *count = 0;
    EMC_NEED(c, "list");
    if (mail_client_list_with_ids(c->mc, names, ids, count) != 0) {
        *names = NULL;
        *ids = NULL;
        *count = 0;
        return fail(c, EMC_ERR_PROTOCOL, "list folders");
    }
    return EMC_OK;
}

EMC_API emc_status emc_select(emc_client *c, const char *name)
{
    EMC_NEED(c, "select");
    return ok_or(c, mail_client_select(c->mc, name), EMC_ERR_PROTOCOL, "select");
}

EMC_API emc_status emc_select_ext(emc_client *c, const char *folder,
                                  uint32_t known_uidval, uint64_t known_modseq,
                                  emc_select_result *res)
{
    if (!res || !folder) return EMC_ERR_ARGUMENT;
    memset(res, 0, sizeof(*res));
    EMC_NEED(c, "select");
    ImapSelectResult r;
    memset(&r, 0, sizeof(r));
    if (mail_client_select_ext(c->mc, folder, known_uidval, known_modseq, &r) != 0)
        return fail(c, EMC_ERR_PROTOCOL, "select");
    res->uidvalidity = r.uidvalidity;
    res->highestmodseq = r.highestmodseq;
    res->vanished_uids = r.vanished_uids;
    res->vanished_count = r.vanished_count;
    return EMC_OK;
}

EMC_API emc_status emc_search(emc_client *c, emc_search_kind kind,
                              char (**uids)[17], int *count)
{
    if (!uids || !count) return EMC_ERR_ARGUMENT;
    *uids = NULL;
    *count = 0;
    EMC_NEED(c, "search");
    MailSearchCriteria crit = kind == EMC_SEARCH_UNREAD  ? MAIL_SEARCH_UNREAD
                            : kind == EMC_SEARCH_FLAGGED ? MAIL_SEARCH_FLAGGED
                            : kind == EMC_SEARCH_DONE    ? MAIL_SEARCH_DONE
                                                         : MAIL_SEARCH_ALL;
    if (mail_client_search(c->mc, crit, uids, count) != 0) {
        *uids = NULL;
        *count = 0;
        return fail(c, EMC_ERR_PROTOCOL, "search");
    }
    return EMC_OK;
}

EMC_API char *emc_fetch_headers(emc_client *c, const char *uid)
{
    if (!c || blank(uid) || connect_now(c) != EMC_OK) return NULL;
    char *h = mail_client_fetch_headers(c->mc, uid);
    if (!h) fail(c, EMC_ERR_PROTOCOL, "fetch headers");
    return h;
}

EMC_API char *emc_fetch_body(emc_client *c, const char *uid)
{
    if (!c || blank(uid) || connect_now(c) != EMC_OK) return NULL;
    char *b = mail_client_fetch_body(c->mc, uid);
    if (!b) fail(c, EMC_ERR_PROTOCOL, "fetch message");
    return b;
}

EMC_API int emc_fetch_flags(emc_client *c, const char *uid)
{
    if (!c || blank(uid) || connect_now(c) != EMC_OK) return -1;
    return mail_client_fetch_flags(c->mc, uid);
}

EMC_API emc_status emc_fetch_flags_changedsince(emc_client *c, uint64_t modseq,
                                                emc_flag_update **updates, int *count)
{
    if (!updates || !count) return EMC_ERR_ARGUMENT;
    *updates = NULL;
    *count = 0;
    EMC_NEED(c, "flags");
    ImapFlagUpdate *u = NULL;
    int n = 0;
    if (mail_client_fetch_flags_changedsince(c->mc, modseq, &u, &n) != 0)
        return fail(c, EMC_ERR_PROTOCOL, "fetch flag changes");
    /* Same layout (uid[17], int flags): handed on as it is. */
    _Static_assert(sizeof(ImapFlagUpdate) == sizeof(emc_flag_update),
                   "flag update layouts differ");
    *updates = (emc_flag_update *)u;
    *count = n;
    return EMC_OK;
}

EMC_API emc_status emc_set_flag(emc_client *c, const char *uid, const char *flag,
                                int add)
{
    if (blank(uid) || blank(flag)) return EMC_ERR_ARGUMENT;
    EMC_NEED(c, "flag");
    return ok_or(c, mail_client_set_flag(c->mc, uid, flag, add), EMC_ERR_PROTOCOL,
                 "set flag");
}

#define EMC_UID_OP(name, call, what)                                        \
    EMC_API emc_status name(emc_client *c, const char *uid)                 \
    {                                                                       \
        if (blank(uid)) return EMC_ERR_ARGUMENT;                            \
        EMC_NEED(c, what);                                                  \
        return ok_or(c, call(c->mc, uid), EMC_ERR_PROTOCOL, what);          \
    }
EMC_UID_OP(emc_mark_junk, mail_client_mark_junk, "mark junk")
EMC_UID_OP(emc_mark_notjunk, mail_client_mark_notjunk, "mark not junk")
EMC_UID_OP(emc_trash, mail_client_trash, "trash")
EMC_UID_OP(emc_delete, mail_client_delete, "delete")

EMC_API emc_status emc_move_to_folder(emc_client *c, const char *uid,
                                      const char *target)
{
    if (blank(uid) || blank(target)) return EMC_ERR_ARGUMENT;
    EMC_NEED(c, "move");
    return ok_or(c, mail_client_move_to_folder(c->mc, uid, target),
                 EMC_ERR_PROTOCOL, "move");
}

EMC_API emc_status emc_modify_label(emc_client *c, const char *uid,
                                    const char *label_id, int add)
{
    if (blank(uid) || blank(label_id)) return EMC_ERR_ARGUMENT;
    EMC_NEED(c, "label");
    return ok_or(c, mail_client_modify_label(c->mc, uid, label_id, add),
                 EMC_ERR_PROTOCOL, "modify label");
}

EMC_API emc_status emc_create_label(emc_client *c, const char *name, char **id_out)
{
    if (id_out) *id_out = NULL;
    if (blank(name)) return EMC_ERR_ARGUMENT;
    EMC_NEED(c, "label");
    return ok_or(c, mail_client_create_label(c->mc, name, id_out),
                 EMC_ERR_PROTOCOL, "create label");
}

EMC_API emc_status emc_delete_label(emc_client *c, const char *label_id)
{
    if (blank(label_id)) return EMC_ERR_ARGUMENT;
    EMC_NEED(c, "label");
    return ok_or(c, mail_client_delete_label(c->mc, label_id), EMC_ERR_PROTOCOL,
                 "delete label");
}

EMC_API emc_status emc_create_folder(emc_client *c, const char *name)
{
    if (blank(name)) return EMC_ERR_ARGUMENT;
    EMC_NEED(c, "folder");
    return ok_or(c, mail_client_create_folder(c->mc, name), EMC_ERR_PROTOCOL,
                 "create folder");
}

EMC_API emc_status emc_delete_folder(emc_client *c, const char *name)
{
    if (blank(name)) return EMC_ERR_ARGUMENT;
    EMC_NEED(c, "folder");
    return ok_or(c, mail_client_delete_folder(c->mc, name), EMC_ERR_PROTOCOL,
                 "delete folder");
}

EMC_API emc_status emc_append(emc_client *c, const char *folder, const char *msg,
                              size_t msg_len)
{
    if (!msg || msg_len == 0) return EMC_ERR_ARGUMENT;
    EMC_NEED(c, "append");
    return ok_or(c, mail_client_append(c->mc, folder, msg, msg_len),
                 EMC_ERR_SEND, "append");
}

EMC_API emc_status emc_sync(emc_client *c, int full)
{
    EMC_NEED(c, "sync");
    if (!c->cfg->gmail_mode) return EMC_OK;   /* IMAP: the caller, folder by folder */
    /* The library's own copy of the local store state names the account. */
    if (local_store_init(c->cfg->host, c->cfg->user) != 0)
        return fail(c, EMC_ERR_CONFIG, "sync (local store)");
    int rc = full ? mail_client_sync_full(c->mc) : mail_client_sync(c->mc);
    return ok_or(c, rc, EMC_ERR_PROTOCOL, "sync");
}

EMC_API emc_status emc_rebuild_indexes(const emc_account *a)
{
    if (!a) return EMC_ERR_ARGUMENT;
    if (local_store_init(a->host, a->user) != 0) return EMC_ERR_CONFIG;
    return gmail_sync_rebuild_indexes() < 0 ? EMC_ERR_PROTOCOL : EMC_OK;
}

EMC_API emc_status emc_gmail_authorize_with(const emc_account *a, emc_gmail_auth *auth,
                                            char **refresh_token)
{
    if (!a || !auth || !refresh_token) return EMC_ERR_ARGUMENT;
    *refresh_token = NULL;
    auth->error[0] = '\0';
    if (!auth->show_url || (auth->mode == EMC_GMAIL_AUTH_PASTE && !auth->read_redirect)) {
        snprintf(auth->error, sizeof(auth->error),
                 "the authorization needs show_url%s",
                 auth->mode == EMC_GMAIL_AUTH_PASTE ? " and read_redirect" : "");
        return EMC_ERR_ARGUMENT;
    }
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.gmail_mode = 1;
    cfg.user = (char *)a->user;
    cfg.gmail_client_id = (char *)a->gmail_client_id;
    cfg.gmail_client_secret = (char *)a->gmail_client_secret;
    cfg.ssl_no_verify = a->insecure_tls ? 1 : 0;
    GmailAuthUi ui;
    memset(&ui, 0, sizeof(ui));
    ui.paste = auth->mode == EMC_GMAIL_AUTH_PASTE;
    ui.show_url = auth->show_url;
    ui.read_redirect = auth->read_redirect;
    ui.user = auth->user;
    int rc = gmail_auth_authorize(&cfg, &ui);
    snprintf(auth->error, sizeof(auth->error), "%s", ui.error);
    if (rc != 0) return EMC_ERR_CONNECT;
    /* The flow wrote the token into cfg; the borrowed fields stay the caller's. */
    *refresh_token = cfg.gmail_refresh_token;
    return *refresh_token ? EMC_OK : EMC_ERR_PROTOCOL;
}

EMC_API emc_status emc_gmail_authorize(const emc_account *a, char **refresh_token)
{
    /* Kept for binary compatibility.  It used to print the URL and start a
     * browser itself; the library does neither now, and without callbacks
     * there is no way to show the URL, so it only refuses. */
    if (!a || !refresh_token) return EMC_ERR_ARGUMENT;
    *refresh_token = NULL;
    return EMC_ERR_ARGUMENT;
}

static void to_compose(const emc_message *m, const char *from, ComposeParams *p)
{
    memset(p, 0, sizeof(*p));
    p->from = from;
    p->to = m->to;
    p->cc = m->cc;
    p->bcc = m->bcc;
    p->subject = m->subject;
    p->body = m->body;
    p->reply_to_msg_id = m->in_reply_to;
    p->attachments = m->attachment_count > 0 ? (const char **)m->attachments : NULL;
    p->attach_count = m->attachment_count;
}

EMC_API emc_status emc_compose(const emc_message *m, char **wire, size_t *wire_len)
{
    if (!m || !wire || !wire_len) return EMC_ERR_ARGUMENT;
    *wire = NULL;
    *wire_len = 0;
    if (blank(m->from) || blank(m->to) || !m->subject || !m->body ||
        m->attachment_count < 0 || (m->attachment_count > 0 && !m->attachments))
        return EMC_ERR_ARGUMENT;
    ComposeParams p;
    to_compose(m, m->from, &p);
    return compose_build_message(&p, wire, wire_len) == 0 ? EMC_OK : EMC_ERR_ARGUMENT;
}

EMC_API emc_status emc_send_wire(emc_client *c, const char *from, const char *to,
                                 const char *wire, size_t wire_len,
                                 emc_send_result *res)
{
    emc_send_result local;
    if (!res) res = &local;
    memset(res, 0, sizeof(*res));
    if (!c || blank(from) || blank(to) || !wire || wire_len == 0) {
        res->status = EMC_ERR_ARGUMENT;
        return res->status;
    }
    c->err[0] = '\0';
    int rc;
    char why[640] = "";
    if (c->cfg->gmail_mode) {
        emc_status st = connect_now(c);
        if (st != EMC_OK) { res->status = st; return st; }
        rc = mail_client_append(c->mc, NULL, wire, wire_len);
    } else {
        rc = smtp_send(c->cfg, from, to, wire, wire_len, why, sizeof(why));
    }
    if (rc != 0) {
        res->status = fail_why(c, EMC_ERR_SEND, "send", why);
        return res->status;
    }
    char *id = mime_get_header(wire, "Message-ID");
    if (id) {
        snprintf(res->message_id, sizeof(res->message_id), "%s", id);
        free(id);
    }
    res->status = EMC_OK;
    return EMC_OK;
}

EMC_API emc_status emc_reply_meta(const char *raw, char **reply_to, char **subject,
                                  char **message_id)
{
    if (!raw || !reply_to || !subject || !message_id) return EMC_ERR_ARGUMENT;
    return compose_extract_reply_meta(raw, reply_to, subject, message_id) == 0
               ? EMC_OK : EMC_ERR_PROTOCOL;
}

EMC_API void emc_log_open(const char *path, int verbose)
{
    if (blank(path)) return;
    /* Appends to the file; errors are mirrored on stderr as the program's own
     * logger does, so the user sees the same messages whichever side logs. */
    logger_init(path, verbose ? LOG_DEBUG : LOG_INFO);
}

EMC_API void emc_log_close(void)
{
    logger_close();
}

EMC_API int emc_take_security_notice(char *buf, size_t size)
{
    return tls_revocation_take_notice(buf, size);
}
