#include "setup_wizard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

static char* get_input(const char *prompt, int hide, FILE *stream) {
    if (stream == stdin) {
        printf("%s: ", prompt);
        fflush(stdout);
    }

    struct termios oldt, newt;
    int is_tty = isatty(fileno(stream));

    if (hide && is_tty) {
        tcgetattr(fileno(stream), &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ECHO);
        tcsetattr(fileno(stream), TCSANOW, &newt);
    }

    char *line = NULL;
    size_t len = 0;
    if (getline(&line, &len, stream) == -1) {
        if (hide && is_tty) tcsetattr(fileno(stream), TCSANOW, &oldt);
        return NULL;
    }

    if (hide && is_tty) {
        tcsetattr(fileno(stream), TCSANOW, &oldt);
        printf("\n");
    }

    // Remove newline
    line[strcspn(line, "\r\n")] = 0;
    return line;
}

/**
 * @brief Internal wizard implementation that can take any input stream.
 */
Config* setup_wizard_run_internal(FILE *stream) {
    if (stream == stdin) {
        printf("\n--- email-cli Configuration Wizard ---\n");
        printf("Please enter your email server details.\n\n");
    }

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    cfg->host = get_input("IMAP Host (e.g., imaps://imap.example.com)", 0, stream);
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

    if (stream == stdin) {
        printf("\nConfiguration collected. Checking connection...\n");
    }

    return cfg;
}

Config* setup_wizard_run(void) {
    return setup_wizard_run_internal(stdin);
}
