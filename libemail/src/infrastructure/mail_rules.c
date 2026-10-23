/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-04-23 */
/* The per-account mail sorting rules engine. */
#include "mail_rules.h"
#include "op_history.h"
#include "when_expr.h"
#include "fs_util.h"
#include "imap_util.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_APP_DIR "email-cli"

static void rule_free_members(MailRule *r);

/* ── Path helpers ────────────────────────────────────────────────────── */

static char *rules_path(const char *account_name) {
    const char *cfg = platform_config_dir();
    if (!cfg || !account_name) return NULL;
    char *path = NULL;
    if (asprintf(&path, "%s/%s/accounts/%s/rules.ini",
                 cfg, CONFIG_APP_DIR, account_name) == -1)
        return NULL;
    return path;
}

/* ── Parsing helpers ─────────────────────────────────────────────────── */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e-1))) e--;
    *e = '\0';
    return s;
}

static MailRule *rules_grow(MailRules *r) {
    if (r->count >= r->cap) {
        int nc = r->cap ? r->cap * 2 : 8;
        MailRule *tmp = realloc(r->rules, (size_t)nc * sizeof(MailRule));
        if (!tmp) return NULL;
        r->rules = tmp;
        r->cap = nc;
    }
    MailRule *rule = &r->rules[r->count++];
    memset(rule, 0, sizeof(*rule));
    return rule;
}

/* ── Public API ──────────────────────────────────────────────────────── */

/** Parses the rules of an open ini stream.  NULL on allocation error. */
static MailRules *rules_parse_fp(FILE *fp) {
    MailRules *r = calloc(1, sizeof(MailRules));
    if (!r) return NULL;

    char line[1024];
    MailRule *cur = NULL;

    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;

        /* Section header: [rule "name"] or [rule "name" action N] */
        if (*p == '[') {
            cur = rules_grow(r);
            if (!cur) { mail_rules_free(r); return NULL; }
            char *qs = strchr(p, '"');
            char *qe = qs ? strchr(qs + 1, '"') : NULL;
            if (qs && qe)
                cur->name = strndup(qs + 1, (size_t)(qe - qs - 1));
            else
                cur->name = strdup("(unnamed)");
            /* Parse optional "action N" suffix */
            char *act = qe ? strstr(qe + 1, "action ") : NULL;
            cur->action_index = act ? atoi(act + 7) : 0;
            continue;
        }

        if (!cur) continue;   /* key-value before any section — skip */

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        /* New boolean expression field (US-81) */
        if (strcmp(key, "when") == 0) {
            free(cur->when);
            cur->when = strdup(val);
        }
        /* Legacy flat fields — still loaded for in-memory backward compat */
        else if (strcmp(key, "if-from")        == 0) { free(cur->if_from);        cur->if_from        = strdup(val); }
        else if (strcmp(key, "if-subject")     == 0) { free(cur->if_subject);     cur->if_subject     = strdup(val); }
        else if (strcmp(key, "if-to")          == 0) { free(cur->if_to);          cur->if_to          = strdup(val); }
        else if (strcmp(key, "if-label")       == 0) { free(cur->if_label);       cur->if_label       = strdup(val); }
        else if (strcmp(key, "if-not-from")    == 0) { free(cur->if_not_from);    cur->if_not_from    = strdup(val); }
        else if (strcmp(key, "if-not-subject") == 0) { free(cur->if_not_subject); cur->if_not_subject = strdup(val); }
        else if (strcmp(key, "if-not-to")      == 0) { free(cur->if_not_to);      cur->if_not_to      = strdup(val); }
        else if (strcmp(key, "if-body")        == 0) { free(cur->if_body);        cur->if_body        = strdup(val); }
        else if (strcmp(key, "if-age-gt")      == 0) { cur->if_age_gt = atoi(val); }
        else if (strcmp(key, "if-age-lt")      == 0) { cur->if_age_lt = atoi(val); }
        else if (strcmp(key, "then-add-label") == 0) {
            if (cur->then_add_count < MAIL_RULE_MAX_LABELS)
                cur->then_add_label[cur->then_add_count++] = strdup(val);
        }
        else if (strcmp(key, "then-remove-label") == 0) {
            if (cur->then_rm_count < MAIL_RULE_MAX_LABELS)
                cur->then_rm_label[cur->then_rm_count++] = strdup(val);
        }
        else if (strcmp(key, "then-move-folder") == 0) {
            free(cur->then_move_folder);
            cur->then_move_folder = imap_utf7_decode(val);
            if (!cur->then_move_folder) cur->then_move_folder = strdup(val);
        }
        else if (strcmp(key, "then-forward-to") == 0) {
            free(cur->then_forward_to);
            cur->then_forward_to = strdup(val);
        }
    }

    /* Convert legacy flat fields → when expression for rules that have no when yet */
    for (int i = 0; i < r->count; i++) {
        MailRule *rule = &r->rules[i];
        if (!rule->when) {
            rule->when = when_from_flat(
                rule->if_from, rule->if_subject, rule->if_to, rule->if_label,
                rule->if_not_from, rule->if_not_subject, rule->if_not_to,
                rule->if_body, rule->if_age_gt, rule->if_age_lt);
        }
    }

    return r;
}

MailRules *mail_rules_load_path(const char *path) {
    if (!path) return NULL;

    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;   /* no rules file — not an error */

    MailRules *r = rules_parse_fp(fp);
    if (r)
        logger_log(LOG_INFO, "mail_rules_load: loaded %d rule(s) from %s", r->count, path);
    return r;
}

MailRules *mail_rules_parse_text(const char *text) {
    if (!text) return NULL;
    FILE *fp = fmemopen((void *)text, strlen(text), "r");
    if (!fp) return NULL;
    MailRules *r = rules_parse_fp(fp);
    fclose(fp);
    return r;
}

MailRules *mail_rules_load(const char *account_name) {
    RAII_STRING char *path = rules_path(account_name);
    return mail_rules_load_path(path);
}

/* ── Serialisation and the operation history ─────────────────────────── */

char *mail_rule_text(const MailRule *r) {
    char *buf = NULL;
    size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    if (!fp) return NULL;
    /* US-82: write [rule "name" action N] header for N >= 2 */
    if (r->action_index >= 2)
        fprintf(fp, "[rule \"%s\" action %d]\n", r->name ? r->name : "", r->action_index);
    else
        fprintf(fp, "[rule \"%s\"]\n", r->name ? r->name : "");
    /* US-81: write when expression; skip old flat if-* fields */
    if (r->when && r->when[0])
        fprintf(fp, "when = %s\n", r->when);
    for (int j = 0; j < r->then_add_count; j++)
        fprintf(fp, "then-add-label    = %s\n", r->then_add_label[j]);
    for (int j = 0; j < r->then_rm_count; j++)
        fprintf(fp, "then-remove-label = %s\n", r->then_rm_label[j]);
    if (r->then_move_folder)
        fprintf(fp, "then-move-folder  = %s\n", r->then_move_folder);
    if (r->then_forward_to)
        fprintf(fp, "then-forward-to   = %s\n", r->then_forward_to);
    fclose(fp);
    return buf;
}

/** The history object of a rule: "rule NAME" or "rule NAME action N". */
static void rule_object(const MailRule *r, char *buf, size_t size) {
    if (r->action_index >= 2)
        snprintf(buf, size, "rule %s action %d", r->name ? r->name : "", r->action_index);
    else
        snprintf(buf, size, "rule %s", r->name ? r->name : "");
}

static int rules_find(const MailRules *rs, const char *object) {
    char o[1024];
    for (int i = 0; rs && i < rs->count; i++) {
        rule_object(&rs->rules[i], o, sizeof(o));
        if (strcmp(o, object) == 0) return i;
    }
    return -1;
}

/** Records one step per rule that was added, deleted or edited. */
static void rules_record_diff(const char *account, const MailRules *before,
                              const MailRules *after) {
    char obj[1024], idx[32];
    for (int i = 0; before && i < before->count; i++) {
        const MailRule *o = &before->rules[i];
        rule_object(o, obj, sizeof(obj));
        int j = rules_find(after, obj);
        char *ot = mail_rule_text(o);
        char *nt = j >= 0 ? mail_rule_text(&after->rules[j]) : NULL;
        if (ot && j < 0) {
            snprintf(idx, sizeof(idx), "%d", i);
            OpRecord rec = { account, "rule", obj, "delete", ot, "", idx, OPH_REVERTABLE, 0 };
            op_history_record(&rec);
        } else if (ot && nt) {
            snprintf(idx, sizeof(idx), "%d", j);
            OpRecord rec = { account, "rule", obj, "edit", ot, nt, idx, OPH_REVERTABLE, 0 };
            op_history_record(&rec);
        }
        free(ot);
        free(nt);
    }
    for (int j = 0; j < after->count; j++) {
        const MailRule *n = &after->rules[j];
        rule_object(n, obj, sizeof(obj));
        if (rules_find(before, obj) >= 0) continue;
        char *nt = mail_rule_text(n);
        snprintf(idx, sizeof(idx), "%d", j);
        OpRecord rec = { account, "rule", obj, "add", "", nt, idx, OPH_REVERTABLE, 0 };
        if (nt) op_history_record(&rec);
        free(nt);
    }
}

int mail_rules_revert_step(const OpStep *st, char *refusal, size_t size) {
    MailRules *rs = mail_rules_load(st->account);
    if (!rs) rs = calloc(1, sizeof(MailRules));
    if (!rs) { snprintf(refusal, size, "out of memory"); return -1; }
    int cur = rules_find(rs, st->object);
    char *cur_text = cur >= 0 ? mail_rule_text(&rs->rules[cur]) : NULL;
    int rc = -1;
    int is_add = strcmp(st->thing, "add") == 0, is_del = strcmp(st->thing, "delete") == 0;

    /* The current state must still be the step's new value (undo-model rule 3). */
    if (is_del ? cur >= 0 : (cur < 0 || !cur_text || strcmp(cur_text, st->new_value) != 0)) {
        snprintf(refusal, size, "the rule '%s' was changed after step %lu; "
                 "the current value is not the one the step set",
                 st->object + 5, st->id);
        goto out;
    }
    if (is_add) {
        MailRule *r = &rs->rules[cur];
        rule_free_members(r);
        memmove(r, r + 1, (size_t)(rs->count - cur - 1) * sizeof(MailRule));
        rs->count--;
    } else {
        MailRules *one = mail_rules_parse_text(st->old_value);
        if (!one || one->count != 1) {
            snprintf(refusal, size, "step %lu holds no rule text", st->id);
            mail_rules_free(one);
            goto out;
        }
        if (is_del) {
            int at = atoi(st->ref);
            if (at < 0 || at > rs->count) at = rs->count;
            if (rs->count >= rs->cap) {
                int nc = rs->cap ? rs->cap * 2 : 8;
                MailRule *t = realloc(rs->rules, (size_t)nc * sizeof(MailRule));
                if (!t) { mail_rules_free(one); snprintf(refusal, size, "out of memory"); goto out; }
                rs->rules = t;
                rs->cap = nc;
            }
            memmove(&rs->rules[at + 1], &rs->rules[at], (size_t)(rs->count - at) * sizeof(MailRule));
            rs->rules[at] = one->rules[0];
            rs->count++;
        } else {
            MailRule keep = one->rules[0];
            rule_free_members(&rs->rules[cur]);
            rs->rules[cur] = keep;
        }
        free(one->rules);
        free(one);
    }
    op_history_revert_begin(st->id);
    rc = mail_rules_save(st->account, rs);
    op_history_revert_end();
    if (rc != 0) snprintf(refusal, size, "the rules file could not be written");
out:
    free(cur_text);
    mail_rules_free(rs);
    return rc;
}

int mail_rules_save(const char *account_name, const MailRules *rules) {
    RAII_STRING char *path = rules_path(account_name);
    if (!path) return -1;

    /* Ensure directory exists */
    {
        char *slash = strrchr(path, '/');
        if (slash) {
            char dir[4096];
            size_t dl = (size_t)(slash - path);
            if (dl >= sizeof(dir)) return -1;
            memcpy(dir, path, dl); dir[dl] = '\0';
            if (fs_mkdir_p(dir, 0700) != 0) return -1;
        }
    }

    /* What the file held before: the history records the difference. */
    MailRules *before = mail_rules_load_path(path);

    {
        RAII_FILE FILE *fp = fopen(path, "w");
        if (!fp) { mail_rules_free(before); return -1; }
        for (int i = 0; i < rules->count; i++) {
            char *t = mail_rule_text(&rules->rules[i]);
            if (!t) { mail_rules_free(before); return -1; }
            fprintf(fp, "%s\n", t);
            free(t);
        }
    }
    rules_record_diff(account_name, before, rules);
    mail_rules_free(before);
    return 0;
}

/** Releases the strings of one rule (not the rule itself). */
static void rule_free_members(MailRule *r) {
    free(r->name);
    free(r->when);
    free(r->if_from);
    free(r->if_not_from);
    free(r->if_subject);
    free(r->if_not_subject);
    free(r->if_to);
    free(r->if_not_to);
    free(r->if_label);
    free(r->if_body);
    for (int j = 0; j < r->then_add_count; j++) free(r->then_add_label[j]);
    for (int j = 0; j < r->then_rm_count;  j++) free(r->then_rm_label[j]);
    free(r->then_move_folder);
    free(r->then_forward_to);
}

void mail_rules_free(MailRules *rules) {
    if (!rules) return;
    for (int i = 0; i < rules->count; i++) {
        MailRule *r = &rules->rules[i];
        free(r->name);
        free(r->when);
        free(r->if_from);
        free(r->if_not_from);
        free(r->if_subject);
        free(r->if_not_subject);
        free(r->if_to);
        free(r->if_not_to);
        free(r->if_label);
        free(r->if_body);
        for (int j = 0; j < r->then_add_count; j++) free(r->then_add_label[j]);
        for (int j = 0; j < r->then_rm_count;  j++) free(r->then_rm_label[j]);
        free(r->then_move_folder);
        free(r->then_forward_to);
    }
    free(rules->rules);
    free(rules);
}

/* Check if val matches glob pattern (case-insensitive, NULL pattern = always match) */
static int glob_match(const char *pattern, const char *val) {
    if (!pattern) return 1;      /* no condition → always matches */
    if (!val || !val[0]) return 0;
    return fnmatch(pattern, val, FNM_CASEFOLD) == 0;
}

/* Check if labels_csv contains a label matching glob pattern */
static int csv_label_match(const char *pattern, const char *csv) {
    if (!pattern) return 1;
    if (!csv || !csv[0]) return 0;
    char *copy = strdup(csv);
    if (!copy) return 0;
    int found = 0;
    char *tok = copy, *s;
    while (tok && *tok) {
        s = strchr(tok, ',');
        if (s) *s = '\0';
        if (fnmatch(pattern, tok, FNM_CASEFOLD) == 0) { found = 1; break; }
        tok = s ? s + 1 : NULL;
    }
    free(copy);
    return found;
}

/* Append a string to a dynamic array; skips duplicates */
static int str_array_add(char ***arr, int *count, const char *s) {
    for (int i = 0; i < *count; i++)
        if (strcmp((*arr)[i], s) == 0) return 0;   /* already present */
    char **tmp = realloc(*arr, (size_t)(*count + 1) * sizeof(char *));
    if (!tmp) return -1;
    *arr = tmp;
    (*arr)[(*count)++] = strdup(s);
    return 0;
}

int mail_rule_matches(const MailRule *rule,
                      const char *from, const char *subject,
                      const char *to, const char *labels_csv,
                      const char *body, time_t message_date)
{
    if (!rule) return 0;

    /* US-81: evaluate boolean when expression when present */
    if (rule->when && rule->when[0]) {
        WhenNode *tree = when_parse(rule->when);
        if (!tree) return 0;   /* syntax error → rule skipped */
        int result = when_eval(tree, from, subject, to, labels_csv, body, message_date);
        when_node_free(tree);
        return result;
    }

    /* Fallback: legacy flat-field AND chain (for in-memory-only rules) */
    if (!glob_match(rule->if_from,    from))    return 0;
    if (!glob_match(rule->if_subject, subject)) return 0;
    if (!glob_match(rule->if_to,      to))      return 0;
    if (rule->if_label && !csv_label_match(rule->if_label, labels_csv)) return 0;
    if (rule->if_not_from    && glob_match(rule->if_not_from,    from))    return 0;
    if (rule->if_not_subject && glob_match(rule->if_not_subject, subject)) return 0;
    if (rule->if_not_to      && glob_match(rule->if_not_to,      to))      return 0;
    if (rule->if_body) {
        if (!body) return 0;
        if (!glob_match(rule->if_body, body)) return 0;
    }
    if (message_date > 0) {
        int age = (int)((time(NULL) - message_date) / 86400);
        if (rule->if_age_gt > 0 && age <= rule->if_age_gt) return 0;
        if (rule->if_age_lt > 0 && age >= rule->if_age_lt) return 0;
    }
    return 1;
}

int mail_rules_apply_ex(const MailRules *rules,
                        const char *from, const char *subject,
                        const char *to, const char *labels_csv,
                        const char *body, time_t message_date,
                        char ***add_out, int *add_count,
                        char ***rm_out,  int *rm_count,
                        char **move_folder_out)
{
    *add_out = NULL; *add_count = 0;
    *rm_out  = NULL; *rm_count  = 0;
    if (move_folder_out) *move_folder_out = NULL;
    if (!rules || rules->count == 0) return 0;

    /* Working copy of labels for incremental if-label checks */
    char *working_labels = labels_csv ? strdup(labels_csv) : strdup("");
    if (!working_labels) return -1;

    int fired = 0;

    for (int i = 0; i < rules->count; i++) {
        const MailRule *r = &rules->rules[i];

        if (!mail_rule_matches(r, from, subject, to, working_labels, body, message_date))
            continue;

        fired++;

        /* Capture first fired rule's then_move_folder */
        if (move_folder_out && !*move_folder_out && r->then_move_folder)
            *move_folder_out = strdup(r->then_move_folder);

        /* Accumulate add/remove actions */
        for (int j = 0; j < r->then_add_count; j++)
            str_array_add(add_out, add_count, r->then_add_label[j]);
        for (int j = 0; j < r->then_rm_count; j++)
            str_array_add(rm_out, rm_count, r->then_rm_label[j]);

        /* Update working labels so subsequent if-label checks see the new state */
        if (r->then_add_count > 0 || r->then_rm_count > 0) {
            /* Rebuild working_labels from current add/rm state */
            size_t need = strlen(labels_csv ? labels_csv : "") + 1;
            for (int j = 0; j < *add_count; j++) need += strlen((*add_out)[j]) + 2;
            char *nb = malloc(need);
            if (nb) {
                nb[0] = '\0';
                /* Start from original + all added so far */
                if (labels_csv && labels_csv[0]) strcpy(nb, labels_csv);
                for (int j = 0; j < *add_count; j++) {
                    if (nb[0]) strcat(nb, ",");
                    strcat(nb, (*add_out)[j]);
                }
                /* Remove entries in rm_out */
                for (int j = 0; j < *rm_count; j++) {
                    char *copy2 = strdup(nb);
                    if (!copy2) continue;
                    nb[0] = '\0';
                    char *tok2 = copy2, *s2;
                    while (tok2 && *tok2) {
                        s2 = strchr(tok2, ',');
                        if (s2) *s2 = '\0';
                        if (strcmp(tok2, (*rm_out)[j]) != 0) {
                            if (nb[0]) strcat(nb, ",");
                            strcat(nb, tok2);
                        }
                        tok2 = s2 ? s2 + 1 : NULL;
                    }
                    free(copy2);
                }
                free(working_labels);
                working_labels = nb;
            }
        }
    }

    free(working_labels);
    return fired;
}

int mail_rules_apply(const MailRules *rules,
                     const char *from, const char *subject,
                     const char *to, const char *labels_csv,
                     const char *body, time_t message_date,
                     char ***add_out, int *add_count,
                     char ***rm_out,  int *rm_count)
{
    return mail_rules_apply_ex(rules, from, subject, to, labels_csv, body,
                               message_date, add_out, add_count, rm_out, rm_count,
                               NULL);
}
