/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* The programs' side of the Gmail authorization: what is shown and asked. */
#include "gmail_login.h"
#include "mail_account.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file gmail_login.c
 * @brief See gmail_login.h.
 */

typedef struct {
    GmailLoginMode mode;
    FILE          *in;
} LoginCtx;

static void login_show_url(const char *url, int port, void *user)
{
    LoginCtx *c = user;
    if (c->mode == GMAIL_LOGIN_PASTE) {
        fprintf(stderr,
            "\n"
            "  Open this address in a browser, on any machine, and allow access:\n\n"
            "  %s\n\n"
            "  The browser then goes to an address that begins with\n"
            "  http://localhost:%d/callback?code=... and fails to load it; that is\n"
            "  expected.  Copy that whole address (or just the code) and paste it\n"
            "  here.\n\n", url, port);
        return;
    }
    fprintf(stderr,
        "\n"
        "  %s\n"
        "  If the browser doesn't open, visit this URL manually:\n\n"
        "  %s\n\n"
        "  Waiting for authorization... (^C to cancel)\n\n"
        "  No browser on this machine?  Open the URL on another one and carry the\n"
        "  redirect here with an SSH forward, run on that machine:\n"
        "    ssh -L %d:localhost:%d <this-host>\n"
        "  or use 'email-cli config reauth --paste'.\n\n",
        c->mode == GMAIL_LOGIN_BROWSER ? "Opening browser for Gmail authorization..."
                                       : "Gmail authorization:",
        url, port, port);
    if (c->mode == GMAIL_LOGIN_BROWSER) {
        /* In the background so system() returns at once; on a host without a
         * desktop it exits quickly and the URL above is what is used. */
        char *cmd = NULL;
        if (asprintf(&cmd, "xdg-open '%s' >/dev/null 2>&1 &", url) != -1) {
            int rc = system(cmd);
            (void)rc;
            free(cmd);
        }
    }
}

static char *login_read_redirect(void *user)
{
    LoginCtx *c = user;
    fprintf(stderr, "  Paste the address (or the code) and press Enter: ");
    fflush(stderr);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, c->in);
    if (n <= 0) {
        free(line);
        return NULL;
    }
    return line;
}

int gmail_login(Config *cfg, GmailLoginMode mode, FILE *in)
{
    emc_account a;
    char *token = NULL;
    LoginCtx ctx = { mode, in ? in : stdin };
    emc_gmail_auth auth;
    memset(&auth, 0, sizeof(auth));
    auth.mode = mode == GMAIL_LOGIN_PASTE ? EMC_GMAIL_AUTH_PASTE : EMC_GMAIL_AUTH_LOOPBACK;
    auth.show_url = login_show_url;
    auth.read_redirect = login_read_redirect;
    auth.user = &ctx;
    mail_account_from_config(cfg, &a);
    if (emc_gmail_authorize_with(&a, &auth, &token) != EMC_OK) {
        fprintf(stderr, "Error: %s\n", auth.error[0] ? auth.error : "authorization failed");
        return -1;
    }
    free(cfg->gmail_refresh_token);
    cfg->gmail_refresh_token = token;
    fprintf(stderr, "  Authorization successful.\n\n");
    return 0;
}

int gmail_login_import_token(Config *cfg, FILE *in)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, in ? in : stdin);
    if (n <= 0) {
        free(line);
        return -1;
    }
    char *b = line;
    while (*b && isspace((unsigned char)*b)) b++;
    char *e = b + strlen(b);
    while (e > b && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    int bad = !*b;
    for (const char *p = b; *p && !bad; p++) if (isspace((unsigned char)*p)) bad = 1;
    if (bad) {
        free(line);
        return -1;
    }
    char *tok = strdup(b);
    free(line);
    if (!tok) return -1;
    free(cfg->gmail_refresh_token);
    cfg->gmail_refresh_token = tok;
    return 0;
}
