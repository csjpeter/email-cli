#include "mail_client.h"
#include "imap_client.h"
#include "gmail_client.h"
#include "gmail_sync.h"
#include "local_store.h"
#include "logger.h"
#include "raii.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Client struct ────────────────────────────────────────────────── */

struct MailClient {
    int is_gmail;
    Config *cfg;              /* borrowed */
    ImapClient  *imap;        /* non-NULL if IMAP */
    GmailClient *gmail;       /* non-NULL if Gmail */
    char *selected;           /* currently selected folder/label */
};

/* ── Connect / Free ───────────────────────────────────────────────── */

MailClient *mail_client_connect(Config *cfg) {
    if (!cfg) return NULL;

    MailClient *mc = calloc(1, sizeof(*mc));
    if (!mc) return NULL;
    mc->cfg = cfg;

    if (cfg->gmail_mode) {
        mc->is_gmail = 1;
        mc->gmail = gmail_connect(cfg);
        if (!mc->gmail) { free(mc); return NULL; }
    } else {
        if (!cfg->host) { free(mc); return NULL; }
        mc->imap = imap_connect(cfg->host, cfg->user, cfg->pass,
                                cfg->ssl_no_verify ? 0 : 1);
        if (!mc->imap) { free(mc); return NULL; }
    }

    return mc;
}

void mail_client_free(MailClient *c) {
    if (!c) return;
    if (c->imap)    imap_disconnect(c->imap);
    if (c->gmail)   gmail_disconnect(c->gmail);
    free(c->selected);
    free(c);
}

int mail_client_uses_labels(const MailClient *c) {
    return c ? c->is_gmail : 0;
}

/* ── List ─────────────────────────────────────────────────────────── */

int mail_client_list(MailClient *c, char ***names_out, int *count_out, char *sep_out) {
    if (c->is_gmail) {
        char **ids = NULL;
        int rc = gmail_list_labels(c->gmail, names_out, &ids, count_out);
        /* Filter out IMPORTANT and CATEGORY_* labels (opaque ML/tab labels). */
        if (rc == 0 && ids && *names_out) {
            int n = 0;
            for (int i = 0; i < *count_out; i++) {
                const char *id = ids[i];
                int filtered = id && (strcmp(id, "IMPORTANT") == 0 ||
                                      strncmp(id, "CATEGORY_", 9) == 0);
                if (filtered) {
                    free((*names_out)[i]);
                    free(ids[i]);
                } else {
                    (*names_out)[n] = (*names_out)[i];
                    ids[n]          = ids[i];
                    n++;
                }
            }
            *count_out = n;
        }
        if (ids) {
            for (int i = 0; i < *count_out; i++) free(ids[i]);
            free(ids);
        }
        if (sep_out) *sep_out = '/';
        return rc;
    }
    return imap_list(c->imap, names_out, count_out, sep_out);
}

/* ── Select ───────────────────────────────────────────────────────── */

int mail_client_select(MailClient *c, const char *name) {
    free(c->selected);
    c->selected = name ? strdup(name) : NULL;

    if (c->is_gmail) {
        /* Gmail: no server-side SELECT needed; just remember the label. */
        return 0;
    }
    return imap_select(c->imap, name);
}

/* ── Search ───────────────────────────────────────────────────────── */

int mail_client_search(MailClient *c, MailSearchCriteria criteria,
                       char (**uids_out)[17], int *count_out) {
    if (c->is_gmail) {
        /* For Gmail: filter by selected label + additional criteria via query */
        const char *query = NULL;
        switch (criteria) {
            case MAIL_SEARCH_UNREAD:  query = "is:unread"; break;
            case MAIL_SEARCH_FLAGGED: query = "is:starred"; break;
            case MAIL_SEARCH_DONE:    query = NULL; break;  /* Not yet supported */
            case MAIL_SEARCH_ALL:     break;
        }
        return gmail_list_messages(c->gmail, c->selected, query, uids_out, count_out);
    }

    /* IMAP: map criteria to IMAP search string */
    const char *imap_criteria = "ALL";
    switch (criteria) {
        case MAIL_SEARCH_UNREAD:  imap_criteria = "UNSEEN"; break;
        case MAIL_SEARCH_FLAGGED: imap_criteria = "FLAGGED"; break;
        case MAIL_SEARCH_DONE:    imap_criteria = "KEYWORD $Done"; break;
        case MAIL_SEARCH_ALL:     break;
    }
    return imap_uid_search(c->imap, imap_criteria, uids_out, count_out);
}

/* ── Fetch ────────────────────────────────────────────────────────── */

char *mail_client_fetch_headers(MailClient *c, const char *uid) {
    if (c->is_gmail) {
        /* Fetch full message and return only the header portion */
        char *raw = gmail_fetch_message(c->gmail, uid, NULL, NULL);
        if (!raw) return NULL;
        /* Split at \r\n\r\n or \n\n boundary */
        const char *sep = strstr(raw, "\r\n\r\n");
        size_t hdr_len;
        if (sep) {
            hdr_len = (size_t)(sep - raw) + 4;
        } else {
            sep = strstr(raw, "\n\n");
            hdr_len = sep ? (size_t)(sep - raw) + 2 : strlen(raw);
        }
        char *headers = malloc(hdr_len + 1);
        if (!headers) { free(raw); return NULL; }
        memcpy(headers, raw, hdr_len);
        headers[hdr_len] = '\0';
        free(raw);
        return headers;
    }
    return imap_uid_fetch_headers(c->imap, uid);
}

char *mail_client_fetch_body(MailClient *c, const char *uid) {
    if (c->is_gmail)
        return gmail_fetch_message(c->gmail, uid, NULL, NULL);
    return imap_uid_fetch_body(c->imap, uid);
}

int mail_client_fetch_flags(MailClient *c, const char *uid) {
    if (c->is_gmail) {
        /* Fetch message to get labels, convert to flags bitmask */
        char **labels = NULL;
        int label_count = 0;
        char *raw = gmail_fetch_message(c->gmail, uid, &labels, &label_count);
        free(raw);

        int flags = 0;
        for (int i = 0; i < label_count; i++) {
            if (strcmp(labels[i], "UNREAD") == 0)  flags |= MSG_FLAG_UNSEEN;
            if (strcmp(labels[i], "STARRED") == 0) flags |= MSG_FLAG_FLAGGED;
            free(labels[i]);
        }
        free(labels);
        return flags;
    }
    return imap_uid_fetch_flags(c->imap, uid);
}

/* ── Flags / Labels ───────────────────────────────────────────────── */

int mail_client_set_flag(MailClient *c, const char *uid,
                         const char *flag, int add) {
    if (c->is_gmail) {
        /* Translate IMAP flag names to Gmail label operations */
        if (strcmp(flag, "\\Seen") == 0) {
            /* \Seen add → remove UNREAD; \Seen remove → add UNREAD */
            const char *label = "UNREAD";
            if (add) {
                const char *rm[] = { label };
                return gmail_modify_labels(c->gmail, uid, NULL, 0, rm, 1);
            } else {
                const char *ad[] = { label };
                return gmail_modify_labels(c->gmail, uid, ad, 1, NULL, 0);
            }
        }
        if (strcmp(flag, "\\Flagged") == 0) {
            const char *label = "STARRED";
            if (add) {
                const char *ad[] = { label };
                return gmail_modify_labels(c->gmail, uid, ad, 1, NULL, 0);
            } else {
                const char *rm[] = { label };
                return gmail_modify_labels(c->gmail, uid, NULL, 0, rm, 1);
            }
        }
        /* Unknown flag — ignore for Gmail */
        logger_log(LOG_DEBUG, "mail_client: Gmail ignoring flag '%s'", flag);
        return 0;
    }
    return imap_uid_set_flag(c->imap, uid, flag, add);
}

int mail_client_trash(MailClient *c, const char *uid) {
    if (c->is_gmail)
        return gmail_trash(c->gmail, uid);

    /* IMAP: set \Deleted flag */
    return imap_uid_set_flag(c->imap, uid, "\\Deleted", 1);
}

/* ── List with IDs ────────────────────────────────────────────────── */

int mail_client_list_with_ids(MailClient *c, char ***names_out,
                              char ***ids_out, int *count_out) {
    *names_out = NULL;
    *ids_out   = NULL;
    *count_out = 0;

    if (c->is_gmail) {
        return gmail_list_labels(c->gmail, names_out, ids_out, count_out);
    }

    /* IMAP: list folders, then duplicate names as IDs */
    int rc = imap_list(c->imap, names_out, count_out, NULL);
    if (rc != 0 || *count_out == 0) return rc;

    char **ids = calloc((size_t)*count_out, sizeof(char *));
    if (!ids) {
        for (int i = 0; i < *count_out; i++) free((*names_out)[i]);
        free(*names_out);
        *names_out = NULL;
        *count_out = 0;
        return -1;
    }
    for (int i = 0; i < *count_out; i++) {
        ids[i] = strdup((*names_out)[i]);
        if (!ids[i]) {
            /* clean up on alloc failure */
            for (int j = 0; j < i; j++) free(ids[j]);
            free(ids);
            for (int j = 0; j < *count_out; j++) free((*names_out)[j]);
            free(*names_out);
            *names_out = NULL;
            *count_out = 0;
            return -1;
        }
    }
    *ids_out = ids;
    return 0;
}

/* ── Create / delete label or folder ─────────────────────────────── */

int mail_client_create_label(MailClient *c, const char *name, char **id_out) {
    if (id_out) *id_out = NULL;

    if (c->is_gmail) {
        return gmail_create_label(c->gmail, name, id_out);
    }

    /* IMAP: create folder */
    int rc = imap_create_folder(c->imap, name);
    if (rc == 0 && id_out) {
        *id_out = strdup(name);
    }
    return rc;
}

int mail_client_delete_label(MailClient *c, const char *label_id) {
    if (c->is_gmail) {
        return gmail_delete_label(c->gmail, label_id);
    }
    return imap_delete_folder(c->imap, label_id);
}

/* ── Label modify (Gmail only) ────────────────────────────────────── */

int mail_client_modify_label(MailClient *c, const char *uid,
                             const char *label_id, int add) {
    if (!c->is_gmail) return 0; /* no-op for IMAP */

    if (add) {
        const char *add_arr[] = {label_id};
        return gmail_modify_labels(c->gmail, uid, add_arr, 1, NULL, 0);
    } else {
        const char *rm_arr[] = {label_id};
        return gmail_modify_labels(c->gmail, uid, NULL, 0, rm_arr, 1);
    }
}

/* ── Append / Send ────────────────────────────────────────────────── */

int mail_client_append(MailClient *c, const char *folder,
                       const char *msg, size_t msg_len) {
    if (c->is_gmail) {
        /* Gmail: send via REST API (folder is ignored) */
        (void)folder;
        return gmail_send(c->gmail, msg, msg_len);
    }
    return imap_append(c->imap, folder, msg, msg_len);
}

/* ── Progress ─────────────────────────────────────────────────────── */

void mail_client_set_progress(MailClient *c, ImapProgressFn fn, void *ctx) {
    if (!c) return;
    if (c->imap)
        imap_set_progress(c->imap, fn, ctx);
    /* Gmail progress is handled separately via gmail_set_progress */
}

/* ── Sync ─────────────────────────────────────────────────────────── */

int mail_client_sync(MailClient *c) {
    if (c->is_gmail)
        return gmail_sync(c->gmail);
    /* IMAP sync is handled by email_service_sync, not here */
    return 0;
}
