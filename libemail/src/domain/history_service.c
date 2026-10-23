/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-22 */
/* Recording and reverting the mail operations (policies/undo-model.md). */

#include "history_service.h"
#include "email_service.h"
#include "config_store.h"
#include "local_store.h"
#include "mail_account.h"
#include "mail_rules.h"
#include "mime_util.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── The server calls of a revert ─────────────────────────────────────── */

/** Writes the folder list of the server into the local cache. */
static void refresh_folders(emc_client *mc, const Config *cfg) {
    char **names = NULL;
    int n = 0;
    char sep = '.';
    local_store_init(cfg->host, cfg->user);
    if (emc_list_names(mc, &names, &n, &sep) != 0) return;
    if (n > 0) local_folder_list_save((const char **)names, n, sep);
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

/** Message-ID of the raw headers @p hdrs, trimmed; heap string or NULL. */
static char *msgid_of(const char *hdrs) {
    char *v = mime_get_header(hdrs, "Message-ID");
    if (!v) return NULL;
    char *s = v;
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' ||
                   s[len - 1] == '\n')) len--;
    char *out = strndup(s, len);
    free(v);
    return out;
}

static int default_move_back(const Config *cfg, const char *msgid, const char *from,
                             const char *to, char *refusal, size_t size) {
    emc_client *mc = mail_connect(cfg);
    if (!mc) {
        snprintf(refusal, size, "could not connect to the server");
        return -1;
    }
    int rc = -1;
    char (*uids)[17] = NULL;
    int n = 0;
    if (emc_select(mc, from) != 0) {
        snprintf(refusal, size, "the server has no folder '%s' any more", from);
        goto out;
    }
    if (emc_search(mc, EMC_SEARCH_ALL, &uids, &n) != 0) {
        snprintf(refusal, size, "the server could not list folder '%s'", from);
        goto out;
    }
    /* A moved mail is the newest in its folder: scan from the end. */
    int found = -1;
    for (int i = n - 1; i >= 0 && i >= n - 500 && found < 0; i--) {
        char *h = emc_fetch_headers(mc, uids[i]);
        if (!h) continue;
        char *id = msgid_of(h);
        if (id && strcmp(id, msgid) == 0) found = i;
        free(id);
        free(h);
    }
    if (found < 0) {
        snprintf(refusal, size, "the mail %s is not in '%s' any more", msgid, from);
        goto out;
    }
    if (emc_move_to_folder(mc, uids[found], to) != 0) {
        snprintf(refusal, size, "the server refused to move the mail to '%s'", to);
        goto out;
    }
    rc = 0;
out:
    free(uids);
    emc_close(mc);
    return rc;
}

static int default_label(const Config *cfg, const char *uid, const char *label,
                         int add) {
    emc_client *mc = mail_connect(cfg);
    if (!mc) return -1;
    int rc = emc_modify_label(mc, uid, label, add);
    emc_close(mc);
    return rc == 0 ? 0 : -1;
}

static int default_folder(const Config *cfg, const char *name, const char *id,
                          int create) {
    emc_client *mc = mail_connect(cfg);
    if (!mc) return -1;
    int rc;
    if (cfg->gmail_mode) {
        if (create) { char *nid = NULL; rc = emc_create_label(mc, name, &nid); free(nid); }
        else rc = emc_delete_label(mc, id && id[0] ? id : name);
    } else {
        rc = create ? emc_create_folder(mc, name) : emc_delete_folder(mc, name);
        if (rc == 0) refresh_folders(mc, cfg);
    }
    emc_close(mc);
    return rc == 0 ? 0 : -1;
}

static const HistoryBackend g_default = { default_move_back, default_label,
                                          default_folder };
static const HistoryBackend *g_backend = &g_default;

void history_set_backend(const HistoryBackend *b) {
    g_backend = b ? b : &g_default;
}

/* ── Recording ────────────────────────────────────────────────────────── */

static const char *acct(const Config *cfg) {
    return cfg && cfg->user ? cfg->user : "";
}

char *history_message_id(const char *folder, const char *uid) {
    char *raw = local_msg_load(folder ? folder : "", uid);
    if (!raw) return NULL;
    char *id = msgid_of(raw);
    free(raw);
    return id;
}

void history_mail_object(const Config *cfg, const char *folder, const char *uid,
                         char *buf, size_t size) {
    if (cfg && cfg->gmail_mode) snprintf(buf, size, "mail %s", uid);
    else snprintf(buf, size, "mail %s/%s", folder ? folder : "", uid);
}

static const char *flag_thing(int bit) {
    switch (bit) {
    case MSG_FLAG_UNSEEN:    return "unread";
    case MSG_FLAG_FLAGGED:   return "starred";
    case MSG_FLAG_DONE:      return "done";
    case MSG_FLAG_FORWARDED: return "forwarded";
    case MSG_FLAG_ANSWERED:  return "answered";
    default:                 return NULL;
    }
}

static int flag_bit_of(const char *thing) {
    if (strcmp(thing, "unread") == 0)    return MSG_FLAG_UNSEEN;
    if (strcmp(thing, "starred") == 0)   return MSG_FLAG_FLAGGED;
    if (strcmp(thing, "done") == 0)      return MSG_FLAG_DONE;
    if (strcmp(thing, "forwarded") == 0) return MSG_FLAG_FORWARDED;
    if (strcmp(thing, "answered") == 0)  return MSG_FLAG_ANSWERED;
    return 0;
}

int history_flag_state(const Config *cfg, const char *folder, const char *uid,
                       int bit) {
    if (cfg && cfg->gmail_mode && (bit == MSG_FLAG_UNSEEN || bit == MSG_FLAG_FLAGGED)) {
        if (!local_hdr_exists("", uid) && !local_msg_exists("", uid)) return -1;
        return label_idx_contains(bit == MSG_FLAG_UNSEEN ? "UNREAD" : "STARRED", uid) > 0;
    }
    Manifest *m = manifest_load(folder ? folder : "");
    if (!m) return -1;
    int r = -1;
    ManifestEntry *e = manifest_find(m, uid);
    if (e) r = (e->flags & bit) != 0;
    manifest_free(m);
    return r;
}

static void rec(const Config *cfg, const char *op, const char *object,
                const char *thing, const char *o, const char *n, const char *ref,
                OpKind kind) {
    OpRecord r = { acct(cfg), op, object, thing, o, n, ref, kind, 0 };
    op_history_record(&r);
}

static const char *yesno(int v) { return v ? "yes" : "no"; }

void history_note_flag(const Config *cfg, const char *folder, const char *uid,
                       int bit, int old_state, int new_state) {
    const char *thing = flag_thing(bit);
    if (!thing) return;
    if (old_state < 0) old_state = !new_state;
    char obj[512];
    history_mail_object(cfg, folder, uid, obj, sizeof(obj));
    rec(cfg, "flag", obj, thing, yesno(old_state), yesno(new_state), folder ? folder : "",
        OPH_REVERTABLE);
}

void history_note_move(const Config *cfg, const char *op, const char *folder,
                       const char *uid, const char *target, const char *msgid) {
    if (!folder || !target || strcmp(folder, target) == 0) return;
    char obj[512];
    history_mail_object(cfg, folder, uid, obj, sizeof(obj));
    rec(cfg, op, obj, "folder", folder, target, msgid ? msgid : "", OPH_REVERTABLE);
}

void history_note_gmail_move(const Config *cfg, const char *uid, const char *src,
                             const char *target) {
    if (!src || !target || strcmp(src, target) == 0) return;
    char obj[512];
    history_mail_object(cfg, "", uid, obj, sizeof(obj));
    rec(cfg, "move", obj, "labels", src, target, "", OPH_REVERTABLE);
}

void history_note_label(const Config *cfg, const char *uid, const char *label,
                        int add) {
    char obj[512], thing[300];
    history_mail_object(cfg, "", uid, obj, sizeof(obj));
    snprintf(thing, sizeof(thing), "label %s", label);
    rec(cfg, "label", obj, thing, yesno(!add), yesno(add), "", OPH_REVERTABLE);
}

void history_note_gmail_trash(const Config *cfg, const char *uid, const char *labels) {
    char obj[512];
    history_mail_object(cfg, "", uid, obj, sizeof(obj));
    rec(cfg, "trash", obj, "labels", labels ? labels : "", "TRASH", "", OPH_REVERTABLE);
}

void history_note_gmail_archive(const Config *cfg, const char *uid,
                                const char *removed, int was_unread) {
    if ((!removed || !removed[0]) && !was_unread) return;
    char obj[512];
    history_mail_object(cfg, "", uid, obj, sizeof(obj));
    rec(cfg, "archive", obj, "labels", removed ? removed : "", "_nolabel",
        was_unread ? "unread" : "", OPH_REVERTABLE);
}

void history_note_junk(const Config *cfg, const char *uid, int junk) {
    char obj[512];
    history_mail_object(cfg, "", uid, obj, sizeof(obj));
    rec(cfg, "junk", obj, "junk", yesno(!junk), yesno(junk), "", OPH_REVERTABLE);
}

void history_note_delete(const Config *cfg, const char *folder, const char *uid,
                         const char *msgid) {
    char obj[512];
    history_mail_object(cfg, folder, uid, obj, sizeof(obj));
    rec(cfg, "delete", obj, "mail", folder ? folder : "", "deleted",
        msgid ? msgid : "", OPH_IRREVERSIBLE);
}

void history_note_folder(const Config *cfg, const char *name, const char *id,
                         int create) {
    char obj[512];
    snprintf(obj, sizeof(obj), "%s %s", cfg && cfg->gmail_mode ? "label" : "folder", name);
    rec(cfg, "folder", obj, "exists", yesno(!create), yesno(create), id ? id : "",
        create ? OPH_REVERTABLE : OPH_IRREVERSIBLE);
}

/* ── Reading ──────────────────────────────────────────────────────────── */

int history_load(const char *account, const char *object, OpHistory *out) {
    OpHistory all = {0};
    if (op_history_load(account, object, &all) != 0) return -1;
    /* Newest first: reverse in place. */
    for (int i = 0, j = all.count - 1; i < j; i++, j--) {
        OpStep t = all.steps[i];
        all.steps[i] = all.steps[j];
        all.steps[j] = t;
    }
    *out = all;
    return 0;
}

int history_shown(const OpStep *s, int recent) {
    return !recent || op_history_is_admin() ||
           strcmp(s->who, op_history_actor()) == 0;
}

void history_describe(const OpStep *s, char *buf, size_t size) {
    if (s->kind == OPH_IRREVERSIBLE && strcmp(s->old_value, "changed") == 0)
        snprintf(buf, size, "%s: changed", s->thing);
    else
        snprintf(buf, size, "%s: %s -> %s", s->thing, s->old_value, s->new_value);
}

int history_revertable(const OpHistory *h, const OpStep *s) {
    return s->kind == OPH_REVERTABLE && op_history_reverted_by(h, s->id) == 0;
}

/* ── Reverting ────────────────────────────────────────────────────────── */

/** Splits "mail FOLDER/UID" or "mail UID" into folder and uid (heap). */
static int parse_mail_object(const char *object, char **folder, char **uid) {
    if (strncmp(object, "mail ", 5) != 0) return -1;
    const char *p = object + 5;
    const char *slash = strrchr(p, '/');
    if (slash) {
        *folder = strndup(p, (size_t)(slash - p));
        *uid = strdup(slash + 1);
    } else {
        *folder = strdup("");
        *uid = strdup(p);
    }
    return *folder && *uid ? 0 : -1;
}

/** The value the history says @p s's thing has now: the new value of the
 *  last step on the same object and thing; NULL when none. */
static const char *value_now(const OpHistory *h, const OpStep *s) {
    const char *v = NULL;
    for (int i = 0; i < h->count; i++)
        if (strcmp(h->steps[i].object, s->object) == 0 &&
            strcmp(h->steps[i].thing, s->thing) == 0)
            v = h->steps[i].new_value;
    return v;
}

static int overtaken(const OpStep *s, const char *now, char *refusal, size_t size) {
    snprintf(refusal, size, "step %lu was overtaken: %s of %s is now '%s', the "
             "step set it to '%s'", s->id, s->thing, s->object, now, s->new_value);
    return -1;
}

static void record_revert(const Config *cfg, const OpStep *s) {
    OpRecord r = { acct(cfg), s->op, s->object, s->thing, s->new_value, s->old_value,
                   s->ref, OPH_REVERTABLE, 0 };
    op_history_record(&r);
}

/** Calls @p fn on every token of the CSV @p csv. */
static int csv_each(const char *csv, int (*fn)(const char *tok, void *ctx), void *ctx) {
    char *copy = strdup(csv ? csv : "");
    if (!copy) return -1;
    int rc = 0;
    char *save = NULL;
    for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (!t[0]) continue;
        if (fn(t, ctx) != 0) rc = -1;
    }
    free(copy);
    return rc;
}

typedef struct { const Config *cfg; const char *uid; } LabelCtx;

static int gmail_label_set(const Config *cfg, const char *uid, const char *label,
                           int add, int server) {
    /* The pseudo labels "_trash", "_nolabel", "_spam" are views of the local
     * index only; "_trash" is TRASH on the server. */
    if (server && label[0] == '_') {
        if (strcmp(label, "_trash") == 0) {
            if (g_backend->label(cfg, uid, "TRASH", add) != 0) return -1;
        }
        server = 0;
    } else if (server && g_backend->label(cfg, uid, label, add) != 0) {
        return -1;
    }
    if (label[0] == '_') {
        if (add) label_idx_add(label, uid); else label_idx_remove(label, uid);
        return 0;
    }
    if (add) {
        label_idx_add(label, uid);
        local_hdr_update_labels("", uid, &label, 1, NULL, 0);
    } else {
        label_idx_remove(label, uid);
        local_hdr_update_labels("", uid, NULL, 0, &label, 1);
    }
    return 0;
}

static int tok_add_back(const char *tok, void *ctx) {
    LabelCtx *c = ctx;
    /* IMPORTANT and CATEGORY_* were never taken off on the server. */
    int server = strcmp(tok, "IMPORTANT") != 0 && strncmp(tok, "CATEGORY_", 9) != 0;
    return gmail_label_set(c->cfg, c->uid, tok, 1, server);
}

static int tok_take_off(const char *tok, void *ctx) {
    LabelCtx *c = ctx;
    int server = strcmp(tok, "IMPORTANT") != 0 && strncmp(tok, "CATEGORY_", 9) != 0;
    return gmail_label_set(c->cfg, c->uid, tok, 0, server);
}

static int tok_local_add(const char *tok, void *ctx) {
    LabelCtx *c = ctx;
    label_idx_add(tok, c->uid);
    return 0;
}

static int revert_flag(const Config *cfg, const OpStep *s, char *refusal, size_t size) {
    char *folder = NULL, *uid = NULL;
    if (parse_mail_object(s->object, &folder, &uid) != 0) {
        free(folder); free(uid);
        snprintf(refusal, size, "step %lu names no mail", s->id);
        return -1;
    }
    int rc = -1;
    int bit = flag_bit_of(s->thing);
    const char *use = s->ref[0] ? s->ref : folder;
    int now = bit ? history_flag_state(cfg, use, uid, bit) : -1;
    if (now < 0) {
        snprintf(refusal, size, "the local store has not the mail %s any more", uid);
    } else if (now != (strcmp(s->new_value, "yes") == 0)) {
        overtaken(s, yesno(now), refusal, size);
    } else if (email_service_set_flag(cfg, uid, use, bit,
                                      strcmp(s->old_value, "yes") == 0) != 0) {
        snprintf(refusal, size, "the change of %s could not be made", s->thing);
    } else {
        rc = 0;
    }
    free(folder); free(uid);
    return rc;
}

static int revert_imap_move(const Config *cfg, const OpStep *s, char *refusal,
                            size_t size) {
    char *folder = NULL, *uid = NULL;
    if (parse_mail_object(s->object, &folder, &uid) != 0) {
        free(folder); free(uid);
        snprintf(refusal, size, "step %lu names no mail", s->id);
        return -1;
    }
    int rc = -1;
    if (!s->ref[0]) {
        snprintf(refusal, size, "the local store had not the mail %s, so it cannot "
                 "be found on the server by its Message-ID", uid);
    } else if (g_backend->move_back(cfg, s->ref, s->new_value, s->old_value,
                                    refusal, size) == 0) {
        record_revert(cfg, s);
        rc = 0;
    }
    free(folder); free(uid);
    return rc;
}

static int revert_gmail(const Config *cfg, const OpStep *s, char *refusal, size_t size) {
    char *folder = NULL, *uid = NULL;
    if (parse_mail_object(s->object, &folder, &uid) != 0) {
        free(folder); free(uid);
        snprintf(refusal, size, "step %lu names no mail", s->id);
        return -1;
    }
    int rc = -1;
    LabelCtx lc = { cfg, uid };
    if (!local_hdr_exists("", uid) && !local_msg_exists("", uid)) {
        snprintf(refusal, size, "the local store has not the mail %s any more", uid);
    } else if (strcmp(s->op, "move") == 0) {
        int in_t = label_idx_contains(s->new_value, uid) > 0;
        int in_s = label_idx_contains(s->old_value, uid) > 0;
        if (!in_t || in_s) {
            snprintf(refusal, size, "step %lu was overtaken: the mail is %s '%s'",
                     s->id, in_t ? "still also in" : "not in", in_t ? s->old_value : s->new_value);
        } else if (gmail_label_set(cfg, uid, s->old_value, 1, 1) == 0 &&
                   gmail_label_set(cfg, uid, s->new_value, 0, 1) == 0) {
            record_revert(cfg, s);
            rc = 0;
        } else {
            snprintf(refusal, size, "the server refused the label change");
        }
    } else if (strcmp(s->op, "label") == 0) {
        const char *label = s->thing + 6;
        int now = label_idx_contains(label, uid) > 0;
        if (now != (strcmp(s->new_value, "yes") == 0)) {
            overtaken(s, yesno(now), refusal, size);
        } else if (gmail_label_set(cfg, uid, label, strcmp(s->old_value, "yes") == 0, 1) == 0) {
            record_revert(cfg, s);
            rc = 0;
        } else {
            snprintf(refusal, size, "the server refused the label change");
        }
    } else if (strcmp(s->op, "trash") == 0 && strcmp(s->old_value, "TRASH") == 0) {
        /* The revert of a revert: the mail goes to the trash again. */
        if (label_idx_contains("_trash", uid) > 0) {
            snprintf(refusal, size, "step %lu was overtaken: the mail is in the "
                     "trash already", s->id);
        } else if (csv_each(s->new_value, tok_take_off, &lc) != 0 ||
                   g_backend->label(cfg, uid, "TRASH", 1) != 0) {
            snprintf(refusal, size, "the server refused to put the mail into the trash");
        } else {
            label_idx_add("_trash", uid);
            record_revert(cfg, s);
            rc = 0;
        }
    } else if (strcmp(s->op, "trash") == 0) {
        if (label_idx_contains("_trash", uid) <= 0) {
            snprintf(refusal, size, "step %lu was overtaken: the mail is not in the "
                     "trash any more", s->id);
        } else if (g_backend->label(cfg, uid, "TRASH", 0) != 0) {
            snprintf(refusal, size, "the server refused to take the mail out of the trash");
        } else {
            csv_each(s->old_value, tok_local_add, &lc);
            label_idx_remove("_trash", uid);
            record_revert(cfg, s);
            rc = 0;
        }
    } else if (strcmp(s->op, "archive") == 0 && strcmp(s->old_value, "_nolabel") == 0) {
        /* The revert of a revert: the mail is archived again. */
        if (label_idx_contains("_nolabel", uid) > 0) {
            snprintf(refusal, size, "step %lu was overtaken: the mail is in the "
                     "archive already", s->id);
        } else if (csv_each(s->new_value, tok_take_off, &lc) != 0) {
            snprintf(refusal, size, "the server refused the label change");
        } else {
            if (strcmp(s->ref, "unread") == 0 &&
                g_backend->label(cfg, uid, "UNREAD", 0) == 0)
                label_idx_remove("UNREAD", uid);
            label_idx_add("_nolabel", uid);
            record_revert(cfg, s);
            rc = 0;
        }
    } else if (strcmp(s->op, "archive") == 0) {
        if (label_idx_contains("_nolabel", uid) <= 0) {
            snprintf(refusal, size, "step %lu was overtaken: the mail is not in the "
                     "archive any more", s->id);
        } else if (csv_each(s->old_value, tok_add_back, &lc) != 0) {
            snprintf(refusal, size, "the server refused the label change");
        } else {
            if (strcmp(s->ref, "unread") == 0 &&
                g_backend->label(cfg, uid, "UNREAD", 1) == 0)
                label_idx_add("UNREAD", uid);
            label_idx_remove("_nolabel", uid);
            record_revert(cfg, s);
            rc = 0;
        }
    }
    free(folder); free(uid);
    return rc;
}

static int revert_junk(const Config *cfg, const OpHistory *h, const OpStep *s,
                       char *refusal, size_t size) {
    char *folder = NULL, *uid = NULL;
    if (parse_mail_object(s->object, &folder, &uid) != 0) {
        free(folder); free(uid);
        snprintf(refusal, size, "step %lu names no mail", s->id);
        return -1;
    }
    int rc = -1;
    const char *now = value_now(h, s);
    if (now && strcmp(now, s->new_value) != 0)
        overtaken(s, now, refusal, size);
    else if ((strcmp(s->old_value, "yes") == 0 ? email_service_mark_junk(cfg, uid)
                                               : email_service_mark_notjunk(cfg, uid)) != 0)
        snprintf(refusal, size, "the server refused the junk mark");
    else
        rc = 0;
    free(folder); free(uid);
    return rc;
}

static int revert_folder(const Config *cfg, const OpHistory *h, const OpStep *s,
                         char *refusal, size_t size) {
    const char *name = strchr(s->object, ' ');
    name = name ? name + 1 : s->object;
    const char *now = value_now(h, s);
    if (now && strcmp(now, s->new_value) != 0) return overtaken(s, now, refusal, size);
    const int create = strcmp(s->old_value, "yes") == 0;
    if (g_backend->folder(cfg, name, s->ref, create) != 0) {
        if (create)
            snprintf(refusal, size, "the server refused to create '%s'", name);
        else
            snprintf(refusal, size, "the server refused to delete '%s' (it may not be empty)",
                     name);
        return -1;
    }
    record_revert(cfg, s);
    return 0;
}

int history_revert(unsigned long id, char *refusal, size_t size) {
    refusal[0] = '\0';
    OpHistory h = {0};
    if (op_history_load(NULL, NULL, &h) != 0) {
        snprintf(refusal, size, "the history could not be read");
        return -1;
    }
    const OpStep *s = op_history_step(&h, id);
    int rc = -1;
    if (!s) {
        snprintf(refusal, size, "there is no step %lu in the history", id);
        goto out;
    }
    if (op_history_may_revert(&h, s, refusal, size) != 0) goto out;

    op_history_revert_begin(id);
    if (strcmp(s->op, "rule") == 0) {
        rc = mail_rules_revert_step(s, refusal, size);
    } else if (strcmp(s->op, "account") == 0 || strcmp(s->op, "setting") == 0) {
        rc = config_revert_step(s, refusal, size);
    } else {
        Config *cfg = s->account[0] ? config_load_account(s->account) : NULL;
        if (!cfg) {
            snprintf(refusal, size, "the account '%s' of step %lu is not configured",
                     s->account, id);
        } else {
            local_store_init(cfg->host, cfg->user);
            if (strcmp(s->op, "flag") == 0)
                rc = revert_flag(cfg, s, refusal, size);
            else if (strcmp(s->op, "junk") == 0)
                rc = revert_junk(cfg, &h, s, refusal, size);
            else if (strcmp(s->op, "folder") == 0)
                rc = revert_folder(cfg, &h, s, refusal, size);
            else if (strcmp(s->thing, "folder") == 0)
                rc = revert_imap_move(cfg, s, refusal, size);
            else
                rc = revert_gmail(cfg, s, refusal, size);
            config_free(cfg);
        }
    }
    op_history_revert_end();
out:
    op_history_free(&h);
    return rc;
}
