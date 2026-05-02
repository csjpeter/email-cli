#include "mail_rules.h"
#include "fs_util.h"
#include "platform/path.h"
#include "raii.h"
#include "logger.h"
#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_APP_DIR "email-cli"

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

MailRules *mail_rules_load(const char *account_name) {
    RAII_STRING char *path = rules_path(account_name);
    if (!path) return NULL;

    RAII_FILE FILE *fp = fopen(path, "r");
    if (!fp) return NULL;   /* no rules file — not an error */

    MailRules *r = calloc(1, sizeof(MailRules));
    if (!r) return NULL;

    char line[1024];
    MailRule *cur = NULL;

    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;

        /* Section header: [rule "name"] */
        if (*p == '[') {
            /* Commit previous rule; start new one */
            cur = rules_grow(r);
            if (!cur) { mail_rules_free(r); return NULL; }
            char *qs = strchr(p, '"');
            char *qe = qs ? strchr(qs + 1, '"') : NULL;
            if (qs && qe)
                cur->name = strndup(qs + 1, (size_t)(qe - qs - 1));
            else
                cur->name = strdup("(unnamed)");
            continue;
        }

        if (!cur) continue;   /* key-value before any section — skip */

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if      (strcmp(key, "if-from")    == 0) { free(cur->if_from);    cur->if_from    = strdup(val); }
        else if (strcmp(key, "if-subject") == 0) { free(cur->if_subject); cur->if_subject = strdup(val); }
        else if (strcmp(key, "if-to")      == 0) { free(cur->if_to);      cur->if_to      = strdup(val); }
        else if (strcmp(key, "if-label")   == 0) { free(cur->if_label);   cur->if_label   = strdup(val); }
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
            cur->then_move_folder = strdup(val);
        }
    }

    logger_log(LOG_INFO, "mail_rules_load: loaded %d rule(s) from %s", r->count, path);
    return r;
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

    RAII_FILE FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    for (int i = 0; i < rules->count; i++) {
        const MailRule *r = &rules->rules[i];
        fprintf(fp, "[rule \"%s\"]\n", r->name ? r->name : "");
        if (r->if_from)    fprintf(fp, "if-from    = %s\n", r->if_from);
        if (r->if_subject) fprintf(fp, "if-subject = %s\n", r->if_subject);
        if (r->if_to)      fprintf(fp, "if-to      = %s\n", r->if_to);
        if (r->if_label)   fprintf(fp, "if-label   = %s\n", r->if_label);
        for (int j = 0; j < r->then_add_count; j++)
            fprintf(fp, "then-add-label    = %s\n", r->then_add_label[j]);
        for (int j = 0; j < r->then_rm_count; j++)
            fprintf(fp, "then-remove-label = %s\n", r->then_rm_label[j]);
        if (r->then_move_folder)
            fprintf(fp, "then-move-folder  = %s\n", r->then_move_folder);
        fprintf(fp, "\n");
    }
    return 0;
}

void mail_rules_free(MailRules *rules) {
    if (!rules) return;
    for (int i = 0; i < rules->count; i++) {
        MailRule *r = &rules->rules[i];
        free(r->name);
        free(r->if_from);
        free(r->if_subject);
        free(r->if_to);
        free(r->if_label);
        for (int j = 0; j < r->then_add_count; j++) free(r->then_add_label[j]);
        for (int j = 0; j < r->then_rm_count;  j++) free(r->then_rm_label[j]);
        free(r->then_move_folder);
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
                      const char *to, const char *labels_csv)
{
    if (!rule) return 0;
    if (!glob_match(rule->if_from,    from))    return 0;
    if (!glob_match(rule->if_subject, subject)) return 0;
    if (!glob_match(rule->if_to,      to))      return 0;
    if (rule->if_label && !csv_label_match(rule->if_label, labels_csv)) return 0;
    return 1;
}

int mail_rules_apply(const MailRules *rules,
                     const char *from, const char *subject,
                     const char *to, const char *labels_csv,
                     char ***add_out, int *add_count,
                     char ***rm_out,  int *rm_count)
{
    *add_out = NULL; *add_count = 0;
    *rm_out  = NULL; *rm_count  = 0;
    if (!rules || rules->count == 0) return 0;

    /* Working copy of labels for incremental if-label checks */
    char *working_labels = labels_csv ? strdup(labels_csv) : strdup("");
    if (!working_labels) return -1;

    int fired = 0;

    for (int i = 0; i < rules->count; i++) {
        const MailRule *r = &rules->rules[i];

        /* Evaluate conditions (all must match) */
        if (!glob_match(r->if_from,    from))    continue;
        if (!glob_match(r->if_subject, subject)) continue;
        if (!glob_match(r->if_to,      to))      continue;
        if (r->if_label && !csv_label_match(r->if_label, working_labels)) continue;

        fired++;

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
