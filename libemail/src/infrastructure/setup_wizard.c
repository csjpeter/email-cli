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

    cfg->host = get_input("IMAP Host — include protocol (e.g., imaps://imap.example.com)", 0, stream);
    if (!cfg->host) { config_free(cfg); return NULL; }

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
