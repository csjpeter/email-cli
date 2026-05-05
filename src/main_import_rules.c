/**
 * @file main_import_rules.c
 * @brief Import mail sorting rules from Thunderbird into email-cli rules.ini format.
 *
 * Thunderbird stores message filters in:
 *   <profile>/ImapMail/<server>/msgFilterRules.dat
 *   <profile>/Mail/<server>/msgFilterRules.dat
 *
 * Usage:
 *   email-import-rules [--thunderbird-path <dir>] [--account <email>]
 *                      [--dry-run] [--output <path>]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <dirent.h>
#include <sys/stat.h>
#include "mail_rules.h"
#include "when_expr.h"
#include "config_store.h"
#include "fs_util.h"
#include "imap_util.h"
#include "platform/path.h"
#include "logger.h"
#include "raii.h"

#ifndef EMAIL_CLI_VERSION
#define EMAIL_CLI_VERSION "unknown"
#endif

/* ── Thunderbird profile detection ───────────────────────────────── */

/* Tries to find a Thunderbird profile directory.
 * Looks for ~/.thunderbird/<profile>/ImapMail or Mail subdirectories.
 * Returns heap-allocated path of the first profile found, or NULL. */
static char *find_thunderbird_profile(void) {
    const char *home = platform_home_dir();
    if (!home) return NULL;

    char tb_dir[8192];
    snprintf(tb_dir, sizeof(tb_dir), "%s/.thunderbird", home);

    DIR *dp = opendir(tb_dir);
    if (!dp) return NULL;

    char *result = NULL;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char *candidate = NULL;
        if (asprintf(&candidate, "%s/%s", tb_dir, de->d_name) == -1) continue;

        struct stat st;
        if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode)) { free(candidate); continue; }

        /* Check for ImapMail or Mail subdirectory */
        char *sub = NULL;
        if (asprintf(&sub, "%s/ImapMail", candidate) != -1 &&
            stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
            result = candidate; candidate = NULL;
            free(sub); sub = NULL;
            break;
        }
        free(sub); sub = NULL;
        if (asprintf(&sub, "%s/Mail", candidate) != -1 &&
            stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
            result = candidate; candidate = NULL;
            free(sub); sub = NULL;
            break;
        }
        free(sub);
        free(candidate);
    }
    closedir(dp);
    return result;
}

/* ── Thunderbird filter file parser ──────────────────────────────── */

/* Thunderbird msgFilterRules.dat format (simplified):
 *   name="Rule Name"
 *   enabled="yes"
 *   condition="AND (from,contains,@github.com)"
 *   action="Move to folder"
 *   actionValue="imap://user@server/GitHub"
 *
 *   (one blank line between rules)
 */

static char *trim_quotes(char *s) {
    if (!s) return s;
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len-1] == '"') {
        s[len-1] = '\0';
        return s + 1;
    }
    return s;
}

/* Maximum number of conditions in one Thunderbird filter rule */
#define TB_MAX_CONDS 256

typedef struct {
    char field[32];    /* "from", "to", "subject", "body", "age" */
    char glob[1024];   /* glob pattern (empty for age) */
    int  is_negated;
    int  is_age_gt;    /* 1 = age-gt, 0 = age-lt */
    int  age_val;
} TBCond;

/* Grow MailRules array by one; return pointer to new zero-initialised entry. */
static MailRule *rules_append(MailRules *r) {
    if (r->count >= r->cap) {
        int nc = r->cap ? r->cap * 2 : 8;
        MailRule *tmp = realloc(r->rules, (size_t)nc * sizeof(MailRule));
        if (!tmp) return NULL;
        r->rules = tmp;
        r->cap   = nc;
    }
    MailRule *nr = &r->rules[r->count++];
    memset(nr, 0, sizeof(*nr));
    return nr;
}


/* Parse one Thunderbird filter file and append rules to *out.
 * Prints warnings to stderr for any rule elements that could not be converted.
 * OR-logic filters with multiple conditions are expanded into one rule each.
 * Returns the number of rules added (may be 0 if none could be converted). */
static int parse_tb_filter_file(const char *path, MailRules **out) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    if (!*out) {
        *out = calloc(1, sizeof(MailRules));
        if (!*out) { fclose(fp); return -1; }
    }

    /* ── Per-filter state (accumulated until blank line or next name=) ── */
    char cur_name[512]          = {0};
    TBCond conds[TB_MAX_CONDS];
    int nconds                  = 0;
    int cur_is_or               = 0;  /* 1 = OR-logic filter */
    int cur_converted_cond      = 0;
    int cur_skipped_cond        = 0;

    /* Actions */
    char then_move_folder[1024] = {0};
    int  pending_move           = 0;
    char then_add_labels[MAIL_RULE_MAX_LABELS][256];
    int  then_add_count         = 0;
    char then_rm_labels[MAIL_RULE_MAX_LABELS][256];
    int  then_rm_count          = 0;
    char then_forward_to[512]   = {0};
    int  pending_forward        = 0;
    int  pending_label          = 0;
    int  cur_converted_act      = 0;
    int  cur_skipped_act        = 0;

    int rules_added = 0;

    /* ── Flush: convert TB filter to MailRule with when expression ── */
#define FLUSH_RULE() do { \
    if (!cur_name[0]) break; \
    if (cur_converted_cond == 0 && cur_converted_act == 0 \
            && (cur_skipped_cond > 0 || cur_skipped_act > 0)) { \
        fprintf(stderr, "  [warn] Rule \"%s\": no conditions or actions could be " \
                "converted — rule will be empty\n", cur_name); \
    } \
    MailRule *_r = rules_append(*out); \
    if (!_r) break; \
    _r->name = strdup(cur_name); \
    /* Build when expression from collected conditions (US-81) */ \
    if (nconds > 0) { \
        WhenCond _wc[TB_MAX_CONDS]; \
        int _nwc = 0; \
        for (int _ci = 0; _ci < nconds; _ci++) { \
            _wc[_nwc].negated = conds[_ci].is_negated; \
            if (strcmp(conds[_ci].field, "age") == 0) { \
                _wc[_nwc].field   = conds[_ci].is_age_gt ? "age-gt" : "age-lt"; \
                char _abuf[16]; \
                snprintf(_abuf, sizeof(_abuf), "%d", conds[_ci].age_val); \
                _wc[_nwc].pattern = strdup(_abuf); \
            } else { \
                _wc[_nwc].field   = conds[_ci].field; \
                _wc[_nwc].pattern = conds[_ci].glob; \
            } \
            _nwc++; \
        } \
        _r->when = when_from_conds(_wc, _nwc, cur_is_or); \
        /* Free age pattern copies */ \
        for (int _ci = 0; _ci < nconds; _ci++) \
            if (strcmp(conds[_ci].field, "age") == 0) \
                free((char *)_wc[_ci].pattern); \
    } \
    if (then_move_folder[0]) \
        _r->then_move_folder = strdup(then_move_folder); \
    for (int _li = 0; _li < then_add_count; _li++) \
        _r->then_add_label[_r->then_add_count++] = strdup(then_add_labels[_li]); \
    for (int _li = 0; _li < then_rm_count; _li++) \
        _r->then_rm_label[_r->then_rm_count++]   = strdup(then_rm_labels[_li]); \
    if (then_forward_to[0]) \
        _r->then_forward_to = strdup(then_forward_to); \
    rules_added++; \
} while (0)

#define RESET_RULE() do { \
    cur_name[0] = '\0'; nconds = 0; cur_is_or = 0; \
    cur_converted_cond = cur_skipped_cond = 0; \
    then_move_folder[0] = '\0'; pending_move = 0; \
    then_add_count = then_rm_count = 0; \
    then_forward_to[0] = '\0'; pending_forward = pending_label = 0; \
    cur_converted_act = cur_skipped_act = 0; \
} while (0)

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (!*p) {
            FLUSH_RULE();
            RESET_RULE();
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = trim_quotes(eq + 1);

        if (strcmp(key, "name") == 0) {
            FLUSH_RULE();
            RESET_RULE();
            snprintf(cur_name, sizeof(cur_name), "%s", val);
            continue;
        }

        if (!cur_name[0]) continue;

        if (strcmp(key, "condition") == 0) {
            cur_is_or = (strncmp(val, "OR", 2) == 0);
            char *v = strdup(val);
            if (!v) continue;
            char *tok = strstr(v, "(");
            while (tok && nconds < TB_MAX_CONDS) {
                tok++;
                char *end = strchr(tok, ')');
                if (!end) break;
                *end = '\0';
                char *f1 = tok, *f2 = NULL, *f3 = NULL;
                char *c1 = strchr(f1, ',');
                if (c1) { *c1 = '\0'; f2 = c1 + 1; }
                char *c2 = f2 ? strchr(f2, ',') : NULL;
                if (c2) { *c2 = '\0'; f3 = c2 + 1; }

                if (f1 && f2 && f3) {
                    int is_body  = (strcmp(f1, "body") == 0);
                    int is_age   = (strcmp(f1, "age")  == 0);
                    int ok_field = (strcmp(f1, "from") == 0 || strcmp(f1, "subject") == 0 ||
                                    strcmp(f1, "to")   == 0 || is_body || is_age);
                    int negated  = (strcmp(f2, "doesn't contain") == 0 ||
                                    strcmp(f2, "isn't")            == 0);
                    /* BUG-001: exact comparisons prevent "isn't"/"doesn't contain"
                     * being treated as positive "is"/"contains". */
                    int ok_match = (strcmp(f2, "contains")     == 0 ||
                                    strcmp(f2, "is")            == 0 ||
                                    strcmp(f2, "begins with")   == 0 ||
                                    strcmp(f2, "ends with")     == 0 ||
                                    strcmp(f2, "greater than")  == 0 ||
                                    strcmp(f2, "less than")     == 0 || negated);

                    if (!ok_field) {
                        fprintf(stderr, "  [warn] Rule \"%s\": condition field \"%s\" "
                                "is not supported, skipping term\n", cur_name, f1);
                        cur_skipped_cond++;
                    } else if (!ok_match) {
                        fprintf(stderr, "  [warn] Rule \"%s\": match type \"%s\" "
                                "is not supported, skipping term\n", cur_name, f2);
                        cur_skipped_cond++;
                    } else {
                        TBCond *c = &conds[nconds++];
                        memset(c, 0, sizeof(*c));
                        snprintf(c->field, sizeof(c->field), "%s", f1);
                        c->is_negated = negated;
                        if (is_age) {
                            c->age_val   = atoi(f3);
                            c->is_age_gt = (strcmp(f2, "greater than") == 0);
                        } else {
                            if (strcmp(f2, "contains") == 0 || negated)
                                snprintf(c->glob, sizeof(c->glob), "*%s*", f3);
                            else if (strcmp(f2, "begins with") == 0)
                                snprintf(c->glob, sizeof(c->glob), "%s*", f3);
                            else if (strcmp(f2, "ends with") == 0)
                                snprintf(c->glob, sizeof(c->glob), "*%s", f3);
                            else
                                snprintf(c->glob, sizeof(c->glob), "%s", f3);
                        }
                        cur_converted_cond++;
                    }
                }
                tok = strstr(end + 1, "(");
            }
            free(v);
            continue;
        }

        if (strcmp(key, "action") == 0) {
            pending_label = pending_forward = 0;
            if (strstr(val, "Move")) {
                pending_move = 1;
                cur_converted_act++;
            } else if (strcmp(val, "Mark as read") == 0 || strcmp(val, "Mark read") == 0) {
                if (then_rm_count < MAIL_RULE_MAX_LABELS)
                    snprintf(then_rm_labels[then_rm_count++], 256, "UNREAD");
                cur_converted_act++;
            } else if (strcmp(val, "Mark as unread") == 0 || strcmp(val, "Mark unread") == 0) {
                if (then_add_count < MAIL_RULE_MAX_LABELS)
                    snprintf(then_add_labels[then_add_count++], 256, "UNREAD");
                cur_converted_act++;
            } else if (strcmp(val, "Mark as starred") == 0 || strcmp(val, "Mark as flagged") == 0) {
                if (then_add_count < MAIL_RULE_MAX_LABELS)
                    snprintf(then_add_labels[then_add_count++], 256, "_flagged");
                cur_converted_act++;
            } else if (strcmp(val, "Mark as junk") == 0 || strcmp(val, "JunkScore") == 0) {
                if (then_add_count < MAIL_RULE_MAX_LABELS)
                    snprintf(then_add_labels[then_add_count++], 256, "_junk");
                cur_converted_act++;
            } else if (strcmp(val, "Delete") == 0) {
                if (then_add_count < MAIL_RULE_MAX_LABELS)
                    snprintf(then_add_labels[then_add_count++], 256, "_trash");
                cur_converted_act++;
            } else if (strcmp(val, "Forward") == 0) {
                pending_forward = 1;
                cur_converted_act++;
            } else if (strcmp(val, "Label") == 0) {
                pending_label = 1;
                cur_converted_act++;
            } else {
                fprintf(stderr, "  [warn] Rule \"%s\": action \"%s\" "
                        "is not supported, skipping\n", cur_name, val);
                cur_skipped_act++;
            }
            continue;
        }

        if (strcmp(key, "actionValue") == 0) {
            if (pending_move) {
                const char *last_slash = strrchr(val, '/');
                const char *raw = last_slash ? last_slash + 1 : val;
                char *decoded = imap_utf7_decode(raw);
                snprintf(then_move_folder, sizeof(then_move_folder),
                         "%s", decoded ? decoded : raw);
                free(decoded);
                pending_move = 0;
            }
            if (pending_forward) {
                snprintf(then_forward_to, sizeof(then_forward_to), "%s", val);
                pending_forward = 0;
            }
            if (pending_label) {
                static const char *tb_labels[] = {
                    NULL, "Important", "Work", "Personal", "TODO", "Later"
                };
                const char *lname = NULL;
                if (strncmp(val, "$label", 6) == 0) {
                    int n = atoi(val + 6);
                    lname = (n >= 1 && n <= 5) ? tb_labels[n] : val;
                } else {
                    lname = val;
                }
                if (lname && then_add_count < MAIL_RULE_MAX_LABELS)
                    snprintf(then_add_labels[then_add_count++], 256, "%s", lname);
                pending_label = 0;
            }
            continue;
        }
    }

    /* EOF without trailing blank line */
    FLUSH_RULE();

#undef FLUSH_RULE
#undef RESET_RULE

    fclose(fp);
    return rules_added;
}

/* ── Thunderbird prefs.js account mapping ────────────────────────── */

#define TB_PREFS_MAX 128

typedef struct {
    char hostname[256]; /* "imap.gmail.com" */
    char username[256]; /* "csjpeter@gmail.com" */
    char dir[256];      /* dirname under ImapMail/, e.g. "imap.gmail.com" */
} TBAccountEntry;

/* Extract hostname from URL like "imaps://box.csaszar.email:993".
 * Writes into buf[buflen]; returns buf on success, NULL on failure. */
static const char *extract_hostname(const char *url, char *buf, size_t buflen) {
    if (!url || !buf || buflen == 0) return NULL;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < buflen)
        buf[i++] = *p++;
    buf[i] = '\0';
    return i > 0 ? buf : NULL;
}

/* Parse Thunderbird prefs.js: build TBAccountEntry[] from mail.server.serverN.* lines.
 * Returns number of entries filled, 0 if file not found or no entries. */
static int parse_tb_prefs(const char *profile_path,
                           TBAccountEntry *entries, int max_entries) {
    char prefs_path[8300];
    snprintf(prefs_path, sizeof(prefs_path), "%s/prefs.js", profile_path);
    FILE *fp = fopen(prefs_path, "r");
    if (!fp) return 0;

    /* Temporary storage indexed by server number (1-based) */
    static char h[TB_PREFS_MAX][256]; /* hostname */
    static char u[TB_PREFS_MAX][256]; /* userName */
    static char d[TB_PREFS_MAX][256]; /* dir name extracted from directory-rel */
    static char used[TB_PREFS_MAX];
    memset(used, 0, sizeof(used));
    for (int i = 0; i < TB_PREFS_MAX; i++) { h[i][0]=u[i][0]=d[i][0]='\0'; }

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        /* user_pref("mail.server.serverN.attr", "value"); */
        const char *prefix = "user_pref(\"mail.server.server";
        if (strncmp(line, prefix, strlen(prefix)) != 0) continue;
        const char *p = line + strlen(prefix);

        int n = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
        if (*p != '.' || n <= 0 || n >= TB_PREFS_MAX) continue;
        p++;

        /* Read attribute name up to ',' */
        char attr[64] = ""; int ai = 0;
        while (*p && *p != '"' && *p != ',' && ai + 1 < (int)sizeof(attr))
            attr[ai++] = *p++;
        attr[ai] = '\0';

        /* Skip to first '"' after ',' to find value */
        char *comma = strchr(p, ',');
        if (!comma) continue;
        char *vs = strchr(comma + 1, '"');
        if (!vs) continue;
        vs++;
        char *ve = strchr(vs, '"');
        if (!ve) continue;
        size_t vl = (size_t)(ve - vs);

        used[n] = 1;
        if (strcmp(attr, "hostname") == 0 && vl < sizeof(h[n])) {
            memcpy(h[n], vs, vl); h[n][vl] = '\0';
        } else if (strcmp(attr, "userName") == 0 && vl < sizeof(u[n])) {
            memcpy(u[n], vs, vl); u[n][vl] = '\0';
        } else if (strcmp(attr, "directory-rel") == 0 && vl < sizeof(d[n])) {
            /* "[ProfD]ImapMail/imap.gmail.com" → "imap.gmail.com"
             * "[ProfD]Mail/Local Folders"      → "Local Folders"  */
            char tmp[256]; memcpy(tmp, vs, vl); tmp[vl] = '\0';
            char *slash = NULL;
            char *im = strstr(tmp, "ImapMail/");
            if (im) slash = im + 8; /* points to '/' before dir name */
            else {
                char *m = strstr(tmp, "Mail/");
                if (m) slash = m + 4;
            }
            if (slash) {
                slash++; /* skip '/' */
                strncpy(d[n], slash, sizeof(d[n]) - 1);
            }
        }
    }
    fclose(fp);

    int count = 0;
    for (int n = 1; n < TB_PREFS_MAX && count < max_entries; n++) {
        if (!used[n] || !h[n][0]) continue;
        strncpy(entries[count].hostname, h[n], sizeof(entries[count].hostname) - 1);
        strncpy(entries[count].username, u[n], sizeof(entries[count].username) - 1);
        strncpy(entries[count].dir,      d[n], sizeof(entries[count].dir)      - 1);
        entries[count].hostname[sizeof(entries[count].hostname)-1] = '\0';
        entries[count].username[sizeof(entries[count].username)-1] = '\0';
        entries[count].dir     [sizeof(entries[count].dir)     -1] = '\0';
        count++;
    }
    return count;
}

/* Find the Thunderbird directory name for an email-cli account.
 * Matches by (hostname, email address).  email is compared to TB userName both
 * as full address and as local part (before '@'), case-insensitively.
 * Returns 1 and fills dir_out on success; returns 0 if no match. */
static int find_tb_dir_for_account(const TBAccountEntry *entries, int count,
                                    const char *email, const char *host,
                                    char *dir_out, size_t dir_out_size) {
    if (!email || !host || !entries || count <= 0) return 0;

    /* Local part of email (before '@') for fallback matching */
    const char *at = strchr(email, '@');
    size_t local_len = at ? (size_t)(at - email) : strlen(email);

    for (int i = 0; i < count; i++) {
        if (strcasecmp(entries[i].hostname, host) != 0) continue;
        const char *uname = entries[i].username;
        int match = (strcasecmp(uname, email) == 0) ||
                    (strlen(uname) == local_len &&
                     strncasecmp(uname, email, local_len) == 0);
        if (!match) continue;
        if (entries[i].dir[0]) {
            strncpy(dir_out, entries[i].dir, dir_out_size - 1);
            dir_out[dir_out_size - 1] = '\0';
            return 1;
        }
    }
    return 0;
}

/* ── Thunderbird scanner ─────────────────────────────────────────── */

/* Scan a single named subdirectory under ImapMail/ or Mail/ for rules. */
static int scan_tb_named_dir(const char *parent, const char *dir_name, MailRules **out) {
    char path[8300];
    snprintf(path, sizeof(path), "%s/%s/msgFilterRules.dat", parent, dir_name);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    printf("  Found: %s\n", path);
    int n = parse_tb_filter_file(path, out);
    return n > 0 ? n : 0;
}

/* ── Per-rule output helpers ─────────────────────────────────────── */

static void print_rule(const MailRule *r) {
    printf("[rule \"%s\"]\n", r->name ? r->name : "(unnamed)");
    if (r->when && r->when[0])
        printf("  when             = %s\n", r->when);
    for (int j = 0; j < r->then_add_count; j++)
        printf("  then-add-label    = %s\n", r->then_add_label[j]);
    for (int j = 0; j < r->then_rm_count; j++)
        printf("  then-remove-label = %s\n", r->then_rm_label[j]);
    if (r->then_move_folder)
        printf("  then-move-folder  = %s\n", r->then_move_folder);
    if (r->then_forward_to)
        printf("  then-forward-to   = %s\n", r->then_forward_to);
    printf("\n");
}

static int write_rules_to_file(const MailRules *rules, const char *path) {
    char *slash = strrchr(path, '/');
    if (slash) {
        char dir[4096];
        size_t dl = (size_t)(slash - path);
        if (dl < sizeof(dir)) {
            memcpy(dir, path, dl); dir[dl] = '\0';
            fs_mkdir_p(dir, 0700);
        }
    }
    FILE *fp = fopen(path, "w");
    if (!fp) { fprintf(stderr, "Error: Cannot write to %s\n", path); return -1; }
    for (int i = 0; i < rules->count; i++) {
        const MailRule *r = &rules->rules[i];
        fprintf(fp, "[rule \"%s\"]\n", r->name ? r->name : "");
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
        fprintf(fp, "\n");
    }
    fclose(fp);
    return 0;
}

/* ── Per-account processing ──────────────────────────────────────── */

/* Scan the Thunderbird directory for tb_dir_name (exact), print rules, save.
 * tb_dir_name: specific subdirectory name under ImapMail/ (from prefs.js lookup).
 * output: NULL → default rules.ini; non-NULL → write to that path.
 * Returns EXIT_SUCCESS / EXIT_FAILURE. */
static int process_account(const char *account_name, const char *tb_dir_name,
                            const char *tb_path, int dry_run, const char *output) {
    char imap_dir[8210], mail_dir[8210];
    snprintf(imap_dir, sizeof(imap_dir), "%s/ImapMail", tb_path);
    snprintf(mail_dir, sizeof(mail_dir), "%s/Mail",     tb_path);

    MailRules *rules = NULL;
    int total = 0;
    total += scan_tb_named_dir(imap_dir, tb_dir_name, &rules);
    total += scan_tb_named_dir(mail_dir, tb_dir_name, &rules);

    if (total == 0 || !rules || rules->count == 0) {
        printf("  No rules found.\n");
        mail_rules_free(rules);
        return EXIT_SUCCESS;
    }

    printf("Found %d rule(s):\n\n", rules->count);
    for (int i = 0; i < rules->count; i++)
        print_rule(&rules->rules[i]);

    if (dry_run) {
        printf("[dry-run] Rules NOT saved.\n");
        mail_rules_free(rules);
        return EXIT_SUCCESS;
    }

    int rc = 0;
    if (output) {
        rc = write_rules_to_file(rules, output);
        if (rc == 0) printf("Rules saved to: %s\n", output);
    } else {
        rc = mail_rules_save(account_name, rules);
        if (rc == 0)
            printf("Rules saved to ~/.config/email-cli/accounts/%s/rules.ini\n",
                   account_name);
        else
            fprintf(stderr, "Error: Failed to save rules for '%s'.\n", account_name);
    }

    mail_rules_free(rules);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* ── Help ────────────────────────────────────────────────────────── */

static void help(void) {
    printf(
        "Usage: email-import-rules [OPTIONS]\n"
        "\n"
        "Import mail sorting rules from Thunderbird into email-cli rules.ini format.\n"
        "\n"
        "Without --account: processes ALL configured email-cli accounts, importing\n"
        "only the Thunderbird filters that belong to each account's IMAP server.\n"
        "\n"
        "Options:\n"
        "  --thunderbird-path <dir>  Path to Thunderbird profile directory\n"
        "                            (auto-detected from ~/.thunderbird if omitted)\n"
        "  --account <email>         Import rules for this account only\n"
        "  --output <path>           Write rules to this file (requires --account)\n"
        "  --dry-run                 Print rules without saving\n"
        "  --version                 Show version\n"
        "  --help, -h                Show this help message\n"
        "\n"
        "Rule file location (default):\n"
        "  ~/.config/email-cli/accounts/<account>/rules.ini\n"
    );
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    const char *tb_path = NULL;
    const char *account = NULL;
    const char *output  = NULL;
    int         dry_run = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            help(); return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("email-import-rules %s\n", EMAIL_CLI_VERSION);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--thunderbird-path") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --thunderbird-path requires a path.\n");
                return EXIT_FAILURE;
            }
            tb_path = argv[++i]; continue;
        }
        if (strcmp(argv[i], "--account") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --account requires an email address.\n");
                return EXIT_FAILURE;
            }
            account = argv[++i]; continue;
        }
        if (strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --output requires a path.\n");
                return EXIT_FAILURE;
            }
            output = argv[++i]; continue;
        }
        if (strcmp(argv[i], "--dry-run") == 0) { dry_run = 1; continue; }
        fprintf(stderr, "Unknown option '%s'.\nRun 'email-import-rules --help'.\n",
                argv[i]);
        return EXIT_FAILURE;
    }

    /* --output without --account is ambiguous in multi-account mode */
    if (output && !account) {
        fprintf(stderr,
                "Error: --output requires --account when multiple accounts exist.\n"
                "Use: email-import-rules --account <email> --output <path>\n");
        return EXIT_FAILURE;
    }

    /* Auto-detect Thunderbird profile */
    RAII_STRING char *tb_auto = NULL;
    if (!tb_path) {
        tb_auto = find_thunderbird_profile();
        if (!tb_auto) {
            fprintf(stderr, "Error: No Thunderbird profile found at ~/.thunderbird.\n"
                            "Use --thunderbird-path to specify the profile directory.\n");
            return EXIT_FAILURE;
        }
        tb_path = tb_auto;
        printf("Thunderbird profile: %s\n", tb_path);
    }

    /* Parse prefs.js once for account→directory mapping */
    TBAccountEntry tb_entries[TB_PREFS_MAX];
    int tb_count = parse_tb_prefs(tb_path, tb_entries, TB_PREFS_MAX);

    if (account) {
        /* ── Single-account mode ── */
        Config *cfg = config_load_account(account);
        char host_buf[512] = "";
        if (cfg && cfg->host)
            extract_hostname(cfg->host, host_buf, sizeof(host_buf));
        config_free(cfg);

        char dir_buf[256] = "";
        if (tb_count > 0 && host_buf[0])
            find_tb_dir_for_account(tb_entries, tb_count, account, host_buf,
                                    dir_buf, sizeof(dir_buf));

        if (!dir_buf[0] && host_buf[0]) {
            /* prefs.js unavailable or no match: warn and skip */
            fprintf(stderr,
                    "Warning: No Thunderbird account found for '%s' (host: %s).\n"
                    "Check that Thunderbird is configured with this account.\n",
                    account, host_buf);
            return EXIT_FAILURE;
        }

        printf("Account: %s → Thunderbird dir: %s\n", account, dir_buf);
        printf("Scanning Thunderbird filters...\n");
        return process_account(account, dir_buf, tb_path, dry_run, output);
    }

    /* ── Multi-account mode ── */
    int count = 0;
    AccountEntry *accounts = config_list_accounts(&count);
    if (!accounts || count == 0) {
        fprintf(stderr, "Error: No account configured. Run the setup wizard first.\n");
        return EXIT_FAILURE;
    }

    int any_error = 0;
    for (int i = 0; i < count; i++) {
        char host_buf[512] = "";
        if (accounts[i].cfg && accounts[i].cfg->host)
            extract_hostname(accounts[i].cfg->host, host_buf, sizeof(host_buf));

        char dir_buf[256] = "";
        if (tb_count > 0 && host_buf[0])
            find_tb_dir_for_account(tb_entries, tb_count,
                                    accounts[i].name, host_buf,
                                    dir_buf, sizeof(dir_buf));

        printf("\n--- Account: %s ---\n", accounts[i].name);
        if (!dir_buf[0]) {
            printf("  No matching Thunderbird account found — skipping.\n");
            continue;
        }
        printf("Scanning Thunderbird filters (dir: %s)...\n", dir_buf);
        int rc = process_account(accounts[i].name, dir_buf,
                                 tb_path, dry_run, NULL);
        if (rc != EXIT_SUCCESS) any_error = 1;
    }
    config_free_account_list(accounts, count);
    return any_error ? EXIT_FAILURE : EXIT_SUCCESS;
}
