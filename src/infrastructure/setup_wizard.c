#include "setup_wizard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

static char* get_input(const char *prompt, int hide) {
    printf("%s: ", prompt);
    fflush(stdout);

    struct termios oldt, newt;
    if (hide) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }

    char *line = NULL;
    size_t len = 0;
    if (getline(&line, &len, stdin) == -1) {
        if (hide) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        return NULL;
    }

    if (hide) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        printf("\n");
    }

    // Remove newline
    line[strcspn(line, "\r\n")] = 0;
    return line;
}

Config* setup_wizard_run(void) {
    printf("\n--- email-cli Configuration Wizard ---\n");
    printf("Please enter your email server details.\n\n");

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    cfg->host = get_input("IMAP Host (e.g., imaps://imap.example.com)", 0);
    if (!cfg->host) { config_free(cfg); return NULL; }

    cfg->user = get_input("Email Username", 0);
    if (!cfg->user) { config_free(cfg); return NULL; }

    cfg->pass = get_input("Email Password", 1);
    if (!cfg->pass) { config_free(cfg); return NULL; }

    cfg->folder = get_input("Default Folder [INBOX]", 0);
    if (!cfg->folder || strlen(cfg->folder) == 0) {
        if (cfg->folder) free(cfg->folder);
        cfg->folder = strdup("INBOX");
    }

    printf("\nConfiguration collected. Checking connection...\n");
    
    // In Phase 2, we would test connection via CURL here.
    // For now, we'll assume success or add it in integration.

    return cfg;
}
