/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-22 */
/* The operation history: an append-only file of recorded steps. */
#include "op_history.h"
#include "fs_util.h"
#include "platform/path.h"
#include "logger.h"
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OPH_FIELDS 12

static char g_actor[128];
static int  g_actor_set;
static int  g_admin_set;
static int  g_admin;
static unsigned long g_reverting;

static char *history_path(void) {
    const char *base = platform_data_dir();
    if (!base) return NULL;
    char *p = NULL;
    if (asprintf(&p, "%s/email-cli/history.tsv", base) == -1) return NULL;
    return p;
}

const char *op_history_actor(void) {
    if (g_actor_set) return g_actor;
    struct passwd *pw = getpwuid(geteuid());
    snprintf(g_actor, sizeof(g_actor), "%s", pw && pw->pw_name ? pw->pw_name : "unknown");
    g_actor_set = 1;
    return g_actor;
}

void op_history_set_actor(const char *who, int admin) {
    if (!who) { g_actor_set = 0; g_admin_set = 0; return; }
    snprintf(g_actor, sizeof(g_actor), "%s", who);
    g_actor_set = 1;
    g_admin = admin;
    g_admin_set = 1;
}

int op_history_is_admin(void) {
    return g_admin_set ? g_admin : geteuid() == 0;
}

void op_history_revert_begin(unsigned long id) { g_reverting = id; }
void op_history_revert_end(void) { g_reverting = 0; }

char *op_history_escape(const char *s) {
    if (!s) s = "";
    char *out = malloc(strlen(s) * 2 + 1);
    if (!out) return NULL;
    char *o = out;
    for (; *s; s++) {
        if (*s == '\t') { *o++ = '\\'; *o++ = 't'; }
        else if (*s == '\n') { *o++ = '\\'; *o++ = 'n'; }
        else if (*s == '\r') { *o++ = '\\'; *o++ = 'r'; }
        else if (*s == '\\') { *o++ = '\\'; *o++ = '\\'; }
        else *o++ = *s;
    }
    *o = '\0';
    return out;
}

static char *unescape(const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) return NULL;
    char *o = out;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            *o++ = s[i] == 't' ? '\t' : s[i] == 'n' ? '\n' : s[i] == 'r' ? '\r' : s[i];
        } else {
            *o++ = s[i];
        }
    }
    *o = '\0';
    return out;
}

static const char *nz(const char *s) { return s ? s : ""; }

/* The highest id in the (locked) file open as @p fd. */
static unsigned long last_id(int fd) {
    unsigned long max = 0;
    FILE *fp = fdopen(dup(fd), "r");
    if (!fp) return 0;
    rewind(fp);
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, fp) > 0) {
        unsigned long id = strtoul(line, NULL, 10);
        if (id > max) max = id;
    }
    free(line);
    fclose(fp);
    return max;
}

long op_history_record(const OpRecord *rec) {
    if (!rec || !rec->op) return -1;
    const char *oldv = nz(rec->old_value), *newv = nz(rec->new_value);
    if (strcmp(oldv, newv) == 0) return 0;     /* nothing changed: nothing recorded */

    char *path = history_path();
    if (!path) return -1;
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        if (fs_mkdir_p(path, 0700) != 0) { free(path); return -1; }
        *slash = '/';
    }
    int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        logger_log(LOG_WARN, "history: cannot open %s: %s", path, strerror(errno));
        free(path);
        return -1;
    }
    free(path);
    if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }

    unsigned long id = last_id(fd) + 1;
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);

    const char *o_v = rec->secret ? "changed" : oldv;
    const char *n_v = rec->secret ? "changed" : newv;
    OpKind kind = rec->secret ? OPH_IRREVERSIBLE : rec->kind;
    char *f[8];
    const char *src[8] = { op_history_actor(), nz(rec->account), rec->op, nz(rec->object),
                           nz(rec->thing), o_v, n_v, nz(rec->ref) };
    int ok = 1;
    for (int i = 0; i < 8; i++) { f[i] = op_history_escape(src[i]); if (!f[i]) ok = 0; }
    long ret = -1;
    if (ok) {
        char *line = NULL;
        int n = asprintf(&line, "%lu\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%lu\t%d\n",
                         id, ts, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                         g_reverting, (int)kind);
        if (n > 0 && write(fd, line, (size_t)n) == n) ret = (long)id;
        else logger_log(LOG_WARN, "history: write failed: %s", strerror(errno));
        free(line);
    }
    for (int i = 0; i < 8; i++) free(f[i]);
    flock(fd, LOCK_UN);
    close(fd);
    return ret;
}

static int parse_line(char *line, OpStep *s) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    char *fld[OPH_FIELDS];
    int n = 0;
    char *p = line;
    while (n < OPH_FIELDS) {
        fld[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = '\0';
        p = t + 1;
    }
    if (n != OPH_FIELDS) return -1;
    memset(s, 0, sizeof(*s));
    s->id = strtoul(fld[0], NULL, 10);
    char **dst[8] = { &s->time, &s->who, &s->account, &s->op, &s->object,
                      &s->thing, &s->old_value, &s->new_value };
    for (int i = 0; i < 8; i++) *dst[i] = unescape(fld[i + 1], strlen(fld[i + 1]));
    s->ref = unescape(fld[9], strlen(fld[9]));
    s->reverts = strtoul(fld[10], NULL, 10);
    int k = atoi(fld[11]);
    s->kind = k == OPH_COMPENSABLE ? OPH_COMPENSABLE
            : k == OPH_IRREVERSIBLE ? OPH_IRREVERSIBLE : OPH_REVERTABLE;
    return s->id && s->time && s->who && s->account && s->op && s->object && s->thing &&
           s->old_value && s->new_value && s->ref ? 0 : -1;
}

void op_history_step_free(OpStep *s) {
    free(s->time); free(s->who); free(s->account); free(s->op); free(s->object);
    free(s->thing); free(s->old_value); free(s->new_value); free(s->ref);
}

int op_history_load(const char *account, const char *object, OpHistory *out) {
    out->steps = NULL;
    out->count = 0;
    char *path = history_path();
    if (!path) return -1;
    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) return errno == ENOENT ? 0 : -1;
    flock(fileno(fp), LOCK_SH);
    int cap = 0;
    char *line = NULL;
    size_t lcap = 0;
    while (getline(&line, &lcap, fp) > 0) {
        OpStep s = {0};
        if (parse_line(line, &s) != 0) { op_history_step_free(&s); continue; }
        /* Reverts of a step are kept even when filtered by object, so that
         * "already reverted" is known: they carry the same object. */
        if ((account && s.account[0] && strcmp(s.account, account) != 0) ||
            (object && strcmp(s.object, object) != 0)) {
            op_history_step_free(&s);
            continue;
        }
        if (out->count == cap) {
            cap = cap ? cap * 2 : 32;
            OpStep *t = realloc(out->steps, (size_t)cap * sizeof(OpStep));
            if (!t) { op_history_step_free(&s); free(line); fclose(fp); op_history_free(out); return -1; }
            out->steps = t;
        }
        out->steps[out->count++] = s;
    }
    free(line);
    fclose(fp);
    return 0;
}

void op_history_free(OpHistory *h) {
    if (!h) return;
    for (int i = 0; i < h->count; i++) op_history_step_free(&h->steps[i]);
    free(h->steps);
    h->steps = NULL;
    h->count = 0;
}

const OpStep *op_history_step(const OpHistory *h, unsigned long id) {
    for (int i = 0; i < h->count; i++)
        if (h->steps[i].id == id) return &h->steps[i];
    return NULL;
}

unsigned long op_history_reverted_by(const OpHistory *h, unsigned long id) {
    unsigned long found = 0;
    for (int i = 0; i < h->count; i++) {
        const OpStep *s = &h->steps[i];
        if (s->reverts != id) continue;
        if (op_history_reverted_by(h, s->id) == 0) found = s->id;
    }
    return found;
}

const char *op_history_kind_name(OpKind kind) {
    return kind == OPH_REVERTABLE ? "revertable"
         : kind == OPH_COMPENSABLE ? "compensable" : "not revertable";
}

int op_history_may_revert(const OpHistory *h, const OpStep *step,
                          char *refusal, size_t size) {
    if (step->kind == OPH_IRREVERSIBLE) {
        snprintf(refusal, size, "step %lu (%s) is not revertable", step->id, step->op);
        return -1;
    }
    if (step->kind == OPH_COMPENSABLE) {
        snprintf(refusal, size, "step %lu (%s) is not revertable by value; "
                 "make the counter-step by hand", step->id, step->op);
        return -1;
    }
    if (!op_history_is_admin() && strcmp(step->who, op_history_actor()) != 0) {
        snprintf(refusal, size, "step %lu was made by %s; only an administrator "
                 "reverts another person's step", step->id, step->who);
        return -1;
    }
    unsigned long by = op_history_reverted_by(h, step->id);
    if (by) {
        snprintf(refusal, size, "step %lu was already reverted by step %lu",
                 step->id, by);
        return -1;
    }
    return 0;
}

void op_history_record_send(const char *account, const char *to, const char *wire) {
    char subject[256] = "";
    for (const char *p = wire; p && *p && *p != '\r' && *p != '\n'; ) {
        const char *eol = strpbrk(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 8 && strncasecmp(p, "Subject:", 8) == 0) {
            const char *v = p + 8;
            while (v < p + len && *v == ' ') v++;
            size_t n = (size_t)(p + len - v);
            if (n >= sizeof(subject)) n = sizeof(subject) - 1;
            memcpy(subject, v, n);
            subject[n] = '\0';
            break;
        }
        if (!eol) break;
        p = eol + (eol[0] == '\r' && eol[1] == '\n' ? 2 : 1);
    }
    char thing[512];
    snprintf(thing, sizeof(thing), "to %s", to ? to : "");
    OpRecord r = { account, "send", "sent mail", thing, "", subject, "",
                   OPH_IRREVERSIBLE, 0 };
    op_history_record(&r);
}
