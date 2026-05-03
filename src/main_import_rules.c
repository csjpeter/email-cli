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
#include "config_store.h"
#include "fs_util.h"
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

/* Parse one Thunderbird filter file and append rules to *out.
 * Prints warnings to stderr for any rule elements that could not be converted.
 * Returns the number of rules added (may be 0 if none could be converted). */
static int parse_tb_filter_file(const char *path, MailRules **out) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    if (!*out) {
        *out = calloc(1, sizeof(MailRules));
        if (!*out) { fclose(fp); return -1; }
    }

    char line[4096];
    MailRule *cur = NULL;
    int rules_added = 0;
    /* Track per-rule conversion success for end-of-rule empty-rule warning */
    int cur_converted_cond  = 0;
    int cur_skipped_cond    = 0;
    int cur_converted_act   = 0;
    int cur_skipped_act     = 0;
    int cur_pending_label   = 0; /* set when action=Label seen, resolved by actionValue */
    int cur_pending_forward = 0; /* set when action=Forward seen, resolved by actionValue */

    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (!*p) {
            /* blank line = end of current rule block */
            if (cur && cur_converted_cond == 0 && cur_converted_act == 0
                    && (cur_skipped_cond > 0 || cur_skipped_act > 0)) {
                fprintf(stderr, "  [warn] Rule \"%s\": no conditions or actions could be "
                        "converted — rule will be empty\n",
                        cur->name ? cur->name : "(unnamed)");
            }
            cur = NULL;
            cur_converted_cond = cur_skipped_cond = 0;
            cur_converted_act  = cur_skipped_act  = 0;
            cur_pending_label   = 0;
            cur_pending_forward = 0;
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = trim_quotes(eq + 1);

        if (strcmp(key, "name") == 0) {
            /* End previous rule tracking before starting new one */
            if (cur && cur_converted_cond == 0 && cur_converted_act == 0
                    && (cur_skipped_cond > 0 || cur_skipped_act > 0)) {
                fprintf(stderr, "  [warn] Rule \"%s\": no conditions or actions could be "
                        "converted — rule will be empty\n",
                        cur->name ? cur->name : "(unnamed)");
            }
            cur_converted_cond = cur_skipped_cond = 0;
            cur_converted_act  = cur_skipped_act  = 0;
            cur_pending_label   = 0;
            cur_pending_forward = 0;

            /* Start new rule */
            cur = NULL;
            MailRule *nr = NULL;
            if ((*out)->count >= (*out)->cap) {
                int nc = (*out)->cap ? (*out)->cap * 2 : 8;
                MailRule *tmp = realloc((*out)->rules, (size_t)nc * sizeof(MailRule));
                if (!tmp) continue;
                (*out)->rules = tmp;
                (*out)->cap   = nc;
            }
            nr = &(*out)->rules[(*out)->count++];
            memset(nr, 0, sizeof(*nr));
            nr->name = strdup(val);
            cur = nr;
            rules_added++;
            continue;
        }

        if (!cur) continue;

        if (strcmp(key, "condition") == 0) {
            /* Parse: AND (from,contains,pattern) (subject,contains,pattern) ... */
            char *v = strdup(val);
            if (!v) continue;
            char *tok = strstr(v, "(");
            while (tok) {
                tok++;
                char *end = strchr(tok, ')');
                if (!end) break;
                *end = '\0';
                /* tok is now: "from,contains,pattern" or "subject,contains,*@github.com" */
                char *f1 = tok, *f2 = NULL, *f3 = NULL;
                char *c1 = strchr(f1, ',');
                if (c1) { *c1 = '\0'; f2 = c1 + 1; }
                char *c2 = f2 ? strchr(f2, ',') : NULL;
                if (c2) { *c2 = '\0'; f3 = c2 + 1; }

                if (f1 && f2 && f3) {
                    int is_body_field = (strcmp(f1, "body") == 0);
                    int is_age_field  = (strcmp(f1, "age")  == 0);
                    int supported_field = (strcmp(f1, "from") == 0 ||
                                           strcmp(f1, "subject") == 0 ||
                                           strcmp(f1, "to") == 0 ||
                                           is_body_field || is_age_field);
                    int is_negated = (strcmp(f2, "doesn't contain") == 0 ||
                                      strcmp(f2, "isn't") == 0);
                    /* BUG-001: exact comparisons prevent "isn't"/"doesn't contain"
                     * being treated as positive "is"/"contains". */
                    int supported_match = (strcmp(f2, "contains")      == 0 ||
                                           strcmp(f2, "is")            == 0 ||
                                           strcmp(f2, "begins with")   == 0 ||
                                           strcmp(f2, "ends with")     == 0 ||
                                           strcmp(f2, "greater than")  == 0 ||
                                           strcmp(f2, "less than")     == 0 ||
                                           is_negated);

                    if (!supported_field) {
                        fprintf(stderr, "  [warn] Rule \"%s\": condition field \"%s\" "
                                "is not supported, skipping term\n",
                                cur->name ? cur->name : "(unnamed)", f1);
                        cur_skipped_cond++;
                    } else if (!supported_match) {
                        fprintf(stderr, "  [warn] Rule \"%s\": match type \"%s\" "
                                "is not supported, skipping term\n",
                                cur->name ? cur->name : "(unnamed)", f2);
                        cur_skipped_cond++;
                    } else if (is_age_field) {
                        /* US-70: age condition */
                        int n = atoi(f3);
                        if (strcmp(f2, "greater than") == 0 && n > 0)
                            cur->if_age_gt = n;
                        else if (strcmp(f2, "less than") == 0 && n > 0)
                            cur->if_age_lt = n;
                        cur_converted_cond++;
                    } else {
                        /* Convert to glob pattern for header fields */
                        char glob[1024];
                        if (strcmp(f2, "contains") == 0 || strcmp(f2, "doesn't contain") == 0)
                            snprintf(glob, sizeof(glob), "*%s*", f3);
                        else if (strcmp(f2, "begins with") == 0)
                            snprintf(glob, sizeof(glob), "%s*", f3);
                        else if (strcmp(f2, "ends with") == 0)
                            snprintf(glob, sizeof(glob), "*%s", f3);
                        else /* is / isn't */
                            snprintf(glob, sizeof(glob), "%s", f3);

                        if (is_body_field) {
                            /* US-69: body condition */
                            if (!cur->if_body)
                                cur->if_body = strdup(glob);
                        } else if (!is_negated) {
                            if (strcmp(f1, "from")    == 0 && !cur->if_from)
                                cur->if_from    = strdup(glob);
                            else if (strcmp(f1, "subject") == 0 && !cur->if_subject)
                                cur->if_subject = strdup(glob);
                            else if (strcmp(f1, "to")      == 0 && !cur->if_to)
                                cur->if_to      = strdup(glob);
                        } else {
                            /* US-68: negated condition */
                            if (strcmp(f1, "from")    == 0 && !cur->if_not_from)
                                cur->if_not_from    = strdup(glob);
                            else if (strcmp(f1, "subject") == 0 && !cur->if_not_subject)
                                cur->if_not_subject = strdup(glob);
                            else if (strcmp(f1, "to")      == 0 && !cur->if_not_to)
                                cur->if_not_to      = strdup(glob);
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
            cur_pending_label   = 0;
            cur_pending_forward = 0;
            if (strstr(val, "Move")) {
                if (cur->then_move_folder == NULL)
                    cur->then_move_folder = strdup("(set by actionValue)");
                cur_converted_act++;
            } else if (strcmp(val, "Mark as read") == 0 ||
                       strcmp(val, "Mark read") == 0) {
                /* US-66 + TB shorthand: remove UNREAD label */
                if (cur->then_rm_count < MAIL_RULE_MAX_LABELS)
                    cur->then_rm_label[cur->then_rm_count++] = strdup("UNREAD");
                cur_converted_act++;
            } else if (strcmp(val, "Mark as unread") == 0 ||
                       strcmp(val, "Mark unread") == 0) {
                /* add UNREAD label */
                if (cur->then_add_count < MAIL_RULE_MAX_LABELS)
                    cur->then_add_label[cur->then_add_count++] = strdup("UNREAD");
                cur_converted_act++;
            } else if (strcmp(val, "Mark as starred") == 0 ||
                       strcmp(val, "Mark as flagged") == 0) {
                /* US-66: starred/flagged → add _flagged label */
                if (cur->then_add_count < MAIL_RULE_MAX_LABELS)
                    cur->then_add_label[cur->then_add_count++] = strdup("_flagged");
                cur_converted_act++;
            } else if (strcmp(val, "Mark as junk") == 0 ||
                       strcmp(val, "JunkScore") == 0) {
                /* US-66 + TB internal junk scoring → add _junk label */
                if (cur->then_add_count < MAIL_RULE_MAX_LABELS)
                    cur->then_add_label[cur->then_add_count++] = strdup("_junk");
                cur_converted_act++;
            } else if (strcmp(val, "Delete") == 0) {
                /* US-66: delete → add _trash label */
                if (cur->then_add_count < MAIL_RULE_MAX_LABELS)
                    cur->then_add_label[cur->then_add_count++] = strdup("_trash");
                cur_converted_act++;
            } else if (strcmp(val, "Forward") == 0) {
                /* Forward address resolved by actionValue */
                cur_pending_forward = 1;
                cur_converted_act++;
            } else if (strcmp(val, "Label") == 0) {
                /* US-67: Thunderbird colour label → custom label (resolved by actionValue) */
                cur_pending_label = 1;
                cur_converted_act++;
            } else {
                fprintf(stderr, "  [warn] Rule \"%s\": action \"%s\" "
                        "is not supported, skipping\n",
                        cur->name ? cur->name : "(unnamed)", val);
                cur_skipped_act++;
            }
            continue;
        }

        if (strcmp(key, "actionValue") == 0) {
            /* For move actions: extract folder name from IMAP URL */
            if (cur->then_move_folder &&
                strcmp(cur->then_move_folder, "(set by actionValue)") == 0) {
                free(cur->then_move_folder);
                /* Extract last path component from IMAP URL */
                const char *last_slash = strrchr(val, '/');
                cur->then_move_folder = strdup(last_slash ? last_slash + 1 : val);
            }
            /* Forward: store target address */
            if (cur_pending_forward) {
                free(cur->then_forward_to);
                cur->then_forward_to = strdup(val);
                cur_pending_forward  = 0;
            }
            /* US-67: resolve Label action → map $labelN to name */
            if (cur_pending_label) {
                static const char *tb_labels[] = {
                    NULL, "Important", "Work", "Personal", "TODO", "Later"
                };
                const char *lname = NULL;
                if (strncmp(val, "$label", 6) == 0) {
                    int n = atoi(val + 6);
                    if (n >= 1 && n <= 5)
                        lname = tb_labels[n];
                    else {
                        static char lbuf[32];
                        snprintf(lbuf, sizeof(lbuf), "Label%d", n);
                        lname = lbuf;
                    }
                } else {
                    lname = val; /* fallback: use raw value */
                }
                if (lname && cur->then_add_count < MAIL_RULE_MAX_LABELS)
                    cur->then_add_label[cur->then_add_count++] = strdup(lname);
                cur_pending_label = 0;
            }
            continue;
        }
    }

    /* Check final rule after EOF (no trailing blank line) */
    if (cur && cur_converted_cond == 0 && cur_converted_act == 0
            && (cur_skipped_cond > 0 || cur_skipped_act > 0)) {
        fprintf(stderr, "  [warn] Rule \"%s\": no conditions or actions could be "
                "converted — rule will be empty\n",
                cur->name ? cur->name : "(unnamed)");
    }

    fclose(fp);
    return rules_added;
}

/* ── Hostname helpers ────────────────────────────────────────────── */

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

/* Check if a Thunderbird server directory name corresponds to host.
 * Thunderbird inserts -N into the second-to-last domain component for
 * duplicate servers: "imap.gmail.com" → "imap.gmail-2.com". */
static int host_matches_tb_dir(const char *host, const char *tbdir) {
    if (!host || !tbdir) return 0;
    if (strcasecmp(host, tbdir) == 0) return 1;

    /* Find second-to-last dot in host */
    const char *last_dot = strrchr(host, '.');
    if (!last_dot) return 0;
    const char *prev_dot = NULL;
    for (const char *p = host; p < last_dot; p++)
        if (*p == '.') prev_dot = p;
    if (!prev_dot) return 0;

    /* host  = <prefix> <mid>       <tld>       e.g. "imap." "gmail"  ".com"
     * tbdir = <prefix> <mid> -N    <tld>       e.g. "imap." "gmail-2" ".com" */
    size_t prefix_and_mid = (size_t)(last_dot - host); /* "imap.gmail" */
    if (strncasecmp(tbdir, host, prefix_and_mid) != 0) return 0;
    const char *rest = tbdir + prefix_and_mid;
    if (rest[0] != '-') return 0;
    rest++;
    if (*rest < '0' || *rest > '9') return 0;
    while (*rest >= '0' && *rest <= '9') rest++;
    return strcasecmp(rest, last_dot) == 0; /* ".com" == ".com" */
}

/* ── Thunderbird scanner ─────────────────────────────────────────── */

/* Scan one Thunderbird server parent dir (ImapMail/ or Mail/).
 * Only includes subdirectories whose name matches host (NULL = all). */
static int scan_tb_server_dir(const char *dir, const char *host, MailRules **out) {
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    int total = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (host && !host_matches_tb_dir(host, de->d_name)) continue;
        char path[8300];
        snprintf(path, sizeof(path), "%s/%s/msgFilterRules.dat", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            printf("  Found: %s\n", path);
            int n = parse_tb_filter_file(path, out);
            if (n > 0) total += n;
        }
    }
    closedir(dp);
    return total;
}

/* ── Per-rule output helpers ─────────────────────────────────────── */

static void print_rule(const MailRule *r) {
    printf("[rule \"%s\"]\n", r->name ? r->name : "(unnamed)");
    if (r->if_from)        printf("  if-from        = %s\n", r->if_from);
    if (r->if_not_from)    printf("  if-not-from    = %s\n", r->if_not_from);
    if (r->if_subject)     printf("  if-subject     = %s\n", r->if_subject);
    if (r->if_not_subject) printf("  if-not-subject = %s\n", r->if_not_subject);
    if (r->if_to)          printf("  if-to          = %s\n", r->if_to);
    if (r->if_not_to)      printf("  if-not-to      = %s\n", r->if_not_to);
    if (r->if_body)        printf("  if-body        = %s\n", r->if_body);
    if (r->if_age_gt > 0)  printf("  if-age-gt      = %d\n", r->if_age_gt);
    if (r->if_age_lt > 0)  printf("  if-age-lt      = %d\n", r->if_age_lt);
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
        if (r->if_from)        fprintf(fp, "if-from        = %s\n", r->if_from);
        if (r->if_not_from)    fprintf(fp, "if-not-from    = %s\n", r->if_not_from);
        if (r->if_subject)     fprintf(fp, "if-subject     = %s\n", r->if_subject);
        if (r->if_not_subject) fprintf(fp, "if-not-subject = %s\n", r->if_not_subject);
        if (r->if_to)          fprintf(fp, "if-to          = %s\n", r->if_to);
        if (r->if_not_to)      fprintf(fp, "if-not-to      = %s\n", r->if_not_to);
        if (r->if_body)        fprintf(fp, "if-body        = %s\n", r->if_body);
        if (r->if_age_gt > 0)  fprintf(fp, "if-age-gt      = %d\n", r->if_age_gt);
        if (r->if_age_lt > 0)  fprintf(fp, "if-age-lt      = %d\n", r->if_age_lt);
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

/* Scan Thunderbird dirs matching host, print rules, optionally save.
 * output: NULL → save to default rules.ini; non-NULL → write to that file.
 * Returns EXIT_SUCCESS / EXIT_FAILURE. */
static int process_account(const char *account_name, const char *host,
                            const char *tb_path, int dry_run, const char *output) {
    char imap_dir[8210], mail_dir[8210];
    snprintf(imap_dir, sizeof(imap_dir), "%s/ImapMail", tb_path);
    snprintf(mail_dir, sizeof(mail_dir), "%s/Mail",     tb_path);

    MailRules *rules = NULL;
    int total = 0;
    total += scan_tb_server_dir(imap_dir, host, &rules);
    total += scan_tb_server_dir(mail_dir, host, &rules);

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

    if (account) {
        /* ── Single-account mode ── */
        Config *cfg = config_load_account(account);
        char host_buf[512] = "";
        if (cfg && cfg->host)
            extract_hostname(cfg->host, host_buf, sizeof(host_buf));
        config_free(cfg);

        printf("Account: %s (host: %s)\n", account,
               host_buf[0] ? host_buf : "unknown");
        printf("Scanning Thunderbird filters...\n");
        return process_account(account, host_buf[0] ? host_buf : NULL,
                               tb_path, dry_run, output);
    }

    /* ── Multi-account mode: process each account with its own host filter ── */
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

        printf("\n--- Account: %s (host: %s) ---\n",
               accounts[i].name, host_buf[0] ? host_buf : "unknown");
        printf("Scanning Thunderbird filters...\n");

        int rc = process_account(accounts[i].name,
                                 host_buf[0] ? host_buf : NULL,
                                 tb_path, dry_run, NULL);
        if (rc != EXIT_SUCCESS) any_error = 1;
    }
    config_free_account_list(accounts, count);
    return any_error ? EXIT_FAILURE : EXIT_SUCCESS;
}
