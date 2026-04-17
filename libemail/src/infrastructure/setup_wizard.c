#include "setup_wizard.h"
#include "gmail_auth.h"
#include "platform/terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef GMAIL_DEFAULT_CLIENT_ID
#define GMAIL_DEFAULT_CLIENT_ID ""
#endif

static char* get_input(const char *prompt, int hide, FILE *stream) {
    int is_tty = isatty(fileno(stream));

    if (hide && stream == stdin) {
        /* Delegate password reading (with echo suppression) to the platform layer.
         * terminal_read_password() reads from stdin and handles TTY detection. */
        char buf[512];
        int n = terminal_read_password(prompt, buf, sizeof(buf));
        if (n < 0) return NULL;
        return strdup(buf);
    }

    if (stream == stdin && is_tty) {
        printf("%s: ", prompt);
        fflush(stdout);
    }

    char *line = NULL;
    size_t len = 0;
    if (getline(&line, &len, stream) == -1) {
        free(line);
        return NULL;
    }

    /* Remove newline */
    line[strcspn(line, "\r\n")] = 0;
    return line;
}

/**
 * Normalise a user-supplied IMAP host string.
 *
 * Rules:
 *  - No protocol (no "://"):  prepend "imaps://" automatically.
 *  - "imaps://…":             accepted as-is.
 *  - Anything else with "://": wrong / unsupported protocol → return NULL.
 *
 * Returns a newly-allocated string the caller must free(), or NULL on error.
 */
static char *normalize_imap_host(const char *input) {
    if (!input || !input[0]) return NULL;
    if (strstr(input, "://") == NULL) {
        /* Plain hostname (or host:port) — add imaps:// */
        size_t n = strlen(input);
        char *out = malloc(n + 9);
        if (!out) return NULL;
        memcpy(out, "imaps://", 8);
        memcpy(out + 8, input, n + 1);
        return out;
    }
    if (strncmp(input, "imaps://", 8) == 0)
        return strdup(input);
    return NULL;  /* explicit but unsupported protocol */
}

/**
 * Normalise a user-supplied SMTP host string.
 *
 * Rules:
 *  - No protocol (no "://"):  prepend "smtps://" automatically.
 *  - "smtps://…":             accepted as-is.
 *  - Anything else with "://": wrong / unsupported protocol → return NULL.
 */
static char *normalize_smtp_host(const char *input) {
    if (!input || !input[0]) return NULL;
    if (strstr(input, "://") == NULL) {
        size_t n = strlen(input);
        char *out = malloc(n + 9);
        if (!out) return NULL;
        memcpy(out, "smtps://", 8);
        memcpy(out + 8, input, n + 1);
        return out;
    }
    if (strncmp(input, "smtps://", 8) == 0)
        return strdup(input);
    return NULL;
}

/**
 * @brief Internal wizard implementation that can take any input stream.
 */
Config* setup_wizard_run_internal(FILE *stream) {
    int is_tty = isatty(fileno(stream));
    if (stream == stdin && is_tty) {
        printf("\n--- email-cli Configuration Wizard ---\n");
        printf("Please enter your email account details.\n\n");
    }

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    /* ── Account type selection ──────────────────────────────────────── */
    if (stream == stdin && is_tty)
        printf("Account type:\n  [1] IMAP (standard e-mail server)\n"
               "  [2] Gmail (Google account — uses Gmail API, not IMAP)\n");

    char *type_str = get_input("Choice [1]", 0, stream);
    int account_type = 1;
    if (type_str && type_str[0] == '2') account_type = 2;
    free(type_str);

    /* ── Gmail flow ──────────────────────────────────────────────────── */
    if (account_type == 2) {
        cfg->gmail_mode = 1;

        cfg->user = get_input("Email address", 0, stream);
        if (!cfg->user || !cfg->user[0]) { config_free(cfg); return NULL; }

        if (stream == stdin && is_tty) {
            /* Check if OAuth2 credentials are available (compiled-in or config) */
            const char *cid = cfg->gmail_client_id;
            int has_builtin = (GMAIL_DEFAULT_CLIENT_ID[0] != '\0');
            if (!has_builtin && (!cid || !cid[0])) {
                printf("\n"
                    "  Gmail requires OAuth2 credentials (client_id and client_secret)\n"
                    "  from a Google Cloud project. See docs/dev/gmail-oauth2-setup.md\n"
                    "  for a step-by-step guide on how to create them.\n\n");
                cfg->gmail_client_id = get_input("GMAIL_CLIENT_ID", 0, stream);
                if (!cfg->gmail_client_id || !cfg->gmail_client_id[0]) {
                    config_free(cfg);
                    return NULL;
                }
                cfg->gmail_client_secret = get_input("GMAIL_CLIENT_SECRET", 0, stream);
                if (!cfg->gmail_client_secret || !cfg->gmail_client_secret[0]) {
                    config_free(cfg);
                    return NULL;
                }
            }

            printf("\nOpening Gmail authorization...\n");
            if (gmail_auth_device_flow(cfg) != 0) {
                fprintf(stderr, "Press Enter to continue...");
                getc(stdin);
                config_free(cfg);
                return NULL;
            }
            printf("Configuration collected.\n");
        }
        return cfg;
    }

    /* ── IMAP flow (existing) ────────────────────────────────────────── */
    if (stream == stdin && is_tty)
        printf("\n");

    for (;;) {
        char *input = get_input("IMAP Host (e.g. imap.example.com)", 0, stream);
        if (!input) { config_free(cfg); return NULL; }
        char *host = normalize_imap_host(input);
        if (!host) {
            fprintf(stderr,
                    "Error: '%s' uses an unsupported protocol"
                    " (only imaps:// is supported).\n", input);
            free(input);
            continue;
        }
        if (strstr(input, "://") == NULL && stream == stdin && is_tty)
            printf("  → using imaps://%s\n", input);
        free(input);
        cfg->host = host;
        break;
    }

    cfg->user = get_input("Email Username", 0, stream);
    if (!cfg->user) { config_free(cfg); return NULL; }

    cfg->pass = get_input("Email Password", 1, stream);
    if (!cfg->pass) { config_free(cfg); return NULL; }

    cfg->folder = get_input("Default Folder [INBOX]", 0, stream);
    if (!cfg->folder || strlen(cfg->folder) == 0) {
        if (cfg->folder) free(cfg->folder);
        cfg->folder = strdup("INBOX");
    }

    /* SMTP configuration (optional) */
    if (stream == stdin && is_tty)
        printf("\n--- SMTP (outgoing mail) — press Enter to skip ---\n");

    char *smtp_host = NULL;
    for (;;) {
        char *input = get_input("SMTP Host [Enter=skip] (e.g. smtp.example.com)", 0, stream);
        if (!input || !input[0]) { free(input); break; }
        char *h = normalize_smtp_host(input);
        if (!h) {
            fprintf(stderr,
                    "Error: '%s' uses an unsupported protocol"
                    " (only smtps:// is supported).\n", input);
            free(input);
            continue;
        }
        if (strstr(input, "://") == NULL && stream == stdin && is_tty)
            printf("  → using smtps://%s\n", input);
        free(input);
        smtp_host = h;
        break;
    }
    if (smtp_host) {
        cfg->smtp_host = smtp_host;

        char *port_str = get_input("SMTP Port [587]", 0, stream);
        if (port_str && port_str[0] != '\0')
            cfg->smtp_port = atoi(port_str);
        else
            cfg->smtp_port = 587;
        free(port_str);

        char *su = get_input("SMTP Username [Enter=same as IMAP]", 0, stream);
        if (su && su[0] != '\0')
            cfg->smtp_user = su;
        else
            free(su);

        char *sp = get_input("SMTP Password [Enter=same as IMAP]", 1, stream);
        if (sp && sp[0] != '\0')
            cfg->smtp_pass = sp;
        else
            free(sp);
    }

    if (stream == stdin && is_tty)
        printf("\nConfiguration collected. Checking connection...\n");

    return cfg;
}

Config* setup_wizard_run(void) {
    return setup_wizard_run_internal(stdin);
}

/* ── SMTP sub-wizard ─────────────────────────────────────────────────── */

/**
 * Derive a plausible default SMTP URL from the IMAP URL in cfg->host.
 * Writes into buf (size bytes).  buf[0] == 0 if derivation is not possible.
 */
static void derive_smtp_url(const Config *cfg, char *buf, size_t size) {
    buf[0] = '\0';
    if (!cfg->host) return;
    if (strncmp(cfg->host, "imaps://", 8) == 0)
        snprintf(buf, size, "smtps://%s", cfg->host + 8);
    /* imap:// is no longer accepted; no derivation for it */
}

int setup_wizard_smtp(Config *cfg) {
    printf("\n--- SMTP (outgoing mail) configuration ---\n");
    printf("Press Enter on any field to keep the shown default.\n\n");

    /* ── SMTP Host ────────────────────────────────────────────────── */
    char derived[512];
    derive_smtp_url(cfg, derived, sizeof(derived));

    for (;;) {
        char host_prompt[1024];
        if (cfg->smtp_host)
            snprintf(host_prompt, sizeof(host_prompt),
                     "SMTP Host [current: %s]", cfg->smtp_host);
        else if (derived[0])
            snprintf(host_prompt, sizeof(host_prompt),
                     "SMTP Host [Enter = %s] (e.g. smtp.example.com)", derived);
        else
            snprintf(host_prompt, sizeof(host_prompt),
                     "SMTP Host (e.g. smtp.example.com)");

        char *input = get_input(host_prompt, 0, stdin);
        if (!input) return -1;   /* EOF / Ctrl-D → abort */
        if (!input[0]) {
            /* Empty → keep current or use derived default */
            free(input);
            if (!cfg->smtp_host && derived[0])
                cfg->smtp_host = strdup(derived);
            break;
        }
        char *host = normalize_smtp_host(input);
        if (!host) {
            fprintf(stderr,
                    "Error: '%s' uses an unsupported protocol"
                    " (only smtps:// is supported).\n", input);
            free(input);
            continue;
        }
        if (strstr(input, "://") == NULL)
            printf("  → using smtps://%s\n", input);
        free(input);
        free(cfg->smtp_host);
        cfg->smtp_host = host;
        break;
    }

    /* ── SMTP Port ────────────────────────────────────────────────── */
    int cur_port = cfg->smtp_port ? cfg->smtp_port : 587;
    char port_prompt[64];
    snprintf(port_prompt, sizeof(port_prompt), "SMTP Port [%d]", cur_port);
    char *port_str = get_input(port_prompt, 0, stdin);
    if (port_str && port_str[0])
        cfg->smtp_port = atoi(port_str);
    else
        cfg->smtp_port = cur_port;
    free(port_str);

    /* ── SMTP Username ────────────────────────────────────────────── */
    char user_prompt[768];
    if (cfg->smtp_user)
        snprintf(user_prompt, sizeof(user_prompt),
                 "SMTP Username [current: %s]", cfg->smtp_user);
    else
        snprintf(user_prompt, sizeof(user_prompt),
                 "SMTP Username [Enter = same as IMAP (%s)]",
                 cfg->user ? cfg->user : "");
    char *su = get_input(user_prompt, 0, stdin);
    if (su && su[0]) {
        free(cfg->smtp_user);
        cfg->smtp_user = su;
    } else {
        free(su);
        /* NULL means "use IMAP username" — keep as-is */
    }

    /* ── SMTP Password ────────────────────────────────────────────── */
    char pass_prompt[64];
    snprintf(pass_prompt, sizeof(pass_prompt), "%s",
             cfg->smtp_pass ? "SMTP Password [Enter = keep current]"
                            : "SMTP Password [Enter = same as IMAP]");
    char *sp = get_input(pass_prompt, 1, stdin);
    if (sp && sp[0]) {
        free(cfg->smtp_pass);
        cfg->smtp_pass = sp;
    } else {
        free(sp);
        /* NULL means "use IMAP password" — keep as-is */
    }

    printf("\nSMTP configuration updated.\n");
    return 0;
}

/* ── IMAP sub-wizard ─────────────────────────────────────────────────── */

int setup_wizard_imap(Config *cfg) {
    printf("\n--- IMAP (incoming mail) configuration ---\n");
    printf("Press Enter on any field to keep the shown default.\n\n");

    /* ── IMAP Host ────────────────────────────────────────────────── */
    for (;;) {
        char host_prompt[1024];
        if (cfg->host)
            snprintf(host_prompt, sizeof(host_prompt),
                     "IMAP Host [current: %s] (e.g. imap.example.com)", cfg->host);
        else
            snprintf(host_prompt, sizeof(host_prompt),
                     "IMAP Host (e.g. imap.example.com)");

        char *input = get_input(host_prompt, 0, stdin);
        if (!input) return -1;   /* EOF / Ctrl-D → abort */
        if (!input[0]) {
            free(input);
            break;  /* keep current */
        }
        char *host = normalize_imap_host(input);
        if (!host) {
            fprintf(stderr,
                    "Error: '%s' uses an unsupported protocol"
                    " (only imaps:// is supported).\n", input);
            free(input);
            continue;  /* re-prompt */
        }
        if (strstr(input, "://") == NULL)
            printf("  → using imaps://%s\n", input);
        free(input);
        free(cfg->host);
        cfg->host = host;
        break;
    }

    /* ── IMAP Username ────────────────────────────────────────────── */
    char user_prompt[768];
    if (cfg->user)
        snprintf(user_prompt, sizeof(user_prompt),
                 "IMAP Username [current: %s]", cfg->user);
    else
        snprintf(user_prompt, sizeof(user_prompt), "IMAP Username");

    char *user = get_input(user_prompt, 0, stdin);
    if (user && user[0]) {
        free(cfg->user);
        cfg->user = user;
    } else {
        free(user);
    }

    /* ── IMAP Password ────────────────────────────────────────────── */
    const char *pass_prompt = cfg->pass
        ? "IMAP Password [Enter=keep current]"
        : "IMAP Password";
    char *pass = get_input(pass_prompt, 1, stdin);
    if (pass && pass[0]) {
        free(cfg->pass);
        cfg->pass = pass;
    } else {
        free(pass);
    }

    /* ── Default Folder ───────────────────────────────────────────── */
    char folder_prompt[768];
    const char *cur_folder = cfg->folder ? cfg->folder : "INBOX";
    snprintf(folder_prompt, sizeof(folder_prompt),
             "Default Folder [current: %s]", cur_folder);

    char *folder = get_input(folder_prompt, 0, stdin);
    if (folder && folder[0]) {
        free(cfg->folder);
        cfg->folder = folder;
    } else {
        free(folder);
        if (!cfg->folder)
            cfg->folder = strdup("INBOX");
    }

    printf("\nIMAP configuration updated.\n");
    return 0;
}
