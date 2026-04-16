#include "setup_wizard.h"
#include "platform/terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
 * @brief Internal wizard implementation that can take any input stream.
 */
Config* setup_wizard_run_internal(FILE *stream) {
    int is_tty = isatty(fileno(stream));
    if (stream == stdin && is_tty) {
        printf("\n--- email-cli Configuration Wizard ---\n");
        printf("Please enter your email server details.\n\n");
    }

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    for (;;) {
        char *host = get_input("IMAP Host (e.g. imaps://imap.example.com)", 0, stream);
        if (!host) { config_free(cfg); return NULL; }
        if (strncmp(host, "imaps://", 8) != 0) {
            fprintf(stderr,
                    "Error: IMAP host must start with 'imaps://' (got '%s').\n",
                    host);
            free(host);
            continue;
        }
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

    char *smtp_host = get_input("SMTP Host (e.g. smtp://smtp.example.com) [Enter=skip]", 0, stream);
    if (smtp_host && smtp_host[0] != '\0') {
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
    } else {
        free(smtp_host);
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

    char host_prompt[1024];
    if (cfg->smtp_host)
        snprintf(host_prompt, sizeof(host_prompt),
                 "SMTP Host [current: %s]", cfg->smtp_host);
    else if (derived[0])
        snprintf(host_prompt, sizeof(host_prompt),
                 "SMTP Host [Enter = %s]", derived);
    else
        snprintf(host_prompt, sizeof(host_prompt),
                 "SMTP Host (e.g. smtps://smtp.example.com)");

    char *host = get_input(host_prompt, 0, stdin);
    if (!host) return -1;   /* EOF / Ctrl-D → abort */
    if (host[0]) {
        free(cfg->smtp_host);
        cfg->smtp_host = host;
    } else {
        free(host);
        if (!cfg->smtp_host && derived[0])
            cfg->smtp_host = strdup(derived);
        /* else keep whatever was there */
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
                     "IMAP Host [current: %s]", cfg->host);
        else
            snprintf(host_prompt, sizeof(host_prompt),
                     "IMAP Host (e.g. imaps://imap.example.com)");

        char *host = get_input(host_prompt, 0, stdin);
        if (!host) return -1;   /* EOF / Ctrl-D → abort */
        if (host[0] == '\0') {
            free(host);
            break;  /* keep current */
        }
        if (strncmp(host, "imaps://", 8) != 0) {
            fprintf(stderr,
                    "Error: IMAP host must start with 'imaps://' (got '%s').\n",
                    host);
            free(host);
            continue;  /* re-prompt */
        }
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
