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
            cur_pending_label  = 0;
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
            cur_pending_label  = 0;

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
            cur_pending_label = 0;
            if (strstr(val, "Move")) {
                if (cur->then_move_folder == NULL)
                    cur->then_move_folder = strdup("(set by actionValue)");
                cur_converted_act++;
            } else if (strcmp(val, "Mark as read") == 0) {
                /* US-66: Mark as read → remove UNREAD label */
                if (cur->then_rm_count < MAIL_RULE_MAX_LABELS)
                    cur->then_rm_label[cur->then_rm_count++] = strdup("UNREAD");
                cur_converted_act++;
            } else if (strcmp(val, "Mark as starred") == 0 ||
                       strcmp(val, "Mark as flagged") == 0) {
                /* US-66: starred/flagged → add _flagged label */
                if (cur->then_add_count < MAIL_RULE_MAX_LABELS)
                    cur->then_add_label[cur->then_add_count++] = strdup("_flagged");
                cur_converted_act++;
            } else if (strcmp(val, "Mark as junk") == 0) {
                /* US-66: junk → add _junk label */
                if (cur->then_add_count < MAIL_RULE_MAX_LABELS)
                    cur->then_add_label[cur->then_add_count++] = strdup("_junk");
                cur_converted_act++;
            } else if (strcmp(val, "Delete") == 0) {
                /* US-66: delete → add _trash label */
                if (cur->then_add_count < MAIL_RULE_MAX_LABELS)
                    cur->then_add_label[cur->then_add_count++] = strdup("_trash");
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

/* Scan a Thunderbird server directory for msgFilterRules.dat files */
static int scan_tb_server_dir(const char *dir, MailRules **out) {
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    int total = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
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

/* ── Help ────────────────────────────────────────────────────────── */

static void help(void) {
    printf(
        "Usage: email-import-rules [OPTIONS]\n"
        "\n"
        "Import mail sorting rules from Thunderbird into email-cli rules.ini format.\n"
        "\n"
        "Options:\n"
        "  --thunderbird-path <dir>  Path to Thunderbird profile directory\n"
        "                            (auto-detected from ~/.thunderbird if omitted)\n"
        "  --account <email>         Target account name in email-cli config\n"
        "                            (uses first configured account if omitted)\n"
        "  --output <path>           Write rules to this file instead of the\n"
        "                            default account rules.ini\n"
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

    const char *tb_path    = NULL;
    const char *account    = NULL;
    const char *output     = NULL;
    int         dry_run    = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            help();
            return EXIT_SUCCESS;
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
            tb_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--account") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --account requires an email address.\n");
                return EXIT_FAILURE;
            }
            account = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --output requires a path.\n");
                return EXIT_FAILURE;
            }
            output = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
            continue;
        }
        fprintf(stderr, "Unknown option '%s'.\nRun 'email-import-rules --help'.\n",
                argv[i]);
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

    /* Determine target account name */
    if (!account) {
        int count = 0;
        AccountEntry *accounts = config_list_accounts(&count);
        if (accounts && count > 0) {
            account = accounts[0].name;   /* Use first account as default */
            printf("Target account: %s\n", account);
        }
        config_free_account_list(accounts, count);
    }
    if (!account) {
        fprintf(stderr, "Error: No account configured. Run the setup wizard first.\n");
        return EXIT_FAILURE;
    }

    /* Scan Thunderbird filter files */
    MailRules *rules = NULL;
    int total = 0;

    char imap_dir[8210], mail_dir[8210];
    snprintf(imap_dir, sizeof(imap_dir), "%s/ImapMail", tb_path);
    snprintf(mail_dir, sizeof(mail_dir), "%s/Mail",     tb_path);

    printf("Scanning Thunderbird filters...\n");
    total += scan_tb_server_dir(imap_dir, &rules);
    total += scan_tb_server_dir(mail_dir, &rules);

    if (total == 0 || !rules || rules->count == 0) {
        printf("No rules found in Thunderbird profile.\n");
        mail_rules_free(rules);
        return EXIT_SUCCESS;
    }

    printf("Found %d rule(s):\n\n", rules->count);

    /* Print rules summary */
    for (int i = 0; i < rules->count; i++) {
        const MailRule *r = &rules->rules[i];
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
        printf("\n");
    }

    if (dry_run) {
        printf("[dry-run] Rules NOT saved.\n");
        mail_rules_free(rules);
        return EXIT_SUCCESS;
    }

    /* Save to output path or default account rules.ini */
    int rc = 0;
    if (output) {
        /* Write directly to the specified path */
        char *slash = strrchr(output, '/');
        if (slash) {
            char dir[4096];
            size_t dl = (size_t)(slash - output);
            if (dl < sizeof(dir)) {
                memcpy(dir, output, dl); dir[dl] = '\0';
                fs_mkdir_p(dir, 0700);
            }
        }
        FILE *fp = fopen(output, "w");
        if (!fp) {
            fprintf(stderr, "Error: Cannot write to %s\n", output);
            mail_rules_free(rules);
            return EXIT_FAILURE;
        }
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
            fprintf(fp, "\n");
        }
        fclose(fp);
        printf("Rules saved to: %s\n", output);
    } else {
        rc = mail_rules_save(account, rules);
        if (rc == 0)
            printf("Rules saved to ~/.config/email-cli/accounts/%s/rules.ini\n", account);
        else
            fprintf(stderr, "Error: Failed to save rules for account '%s'.\n", account);
    }

    mail_rules_free(rules);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
