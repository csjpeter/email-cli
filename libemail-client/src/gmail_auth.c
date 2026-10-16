/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-04-17 */
/* OAuth2 authorisation and token refresh for the Gmail API. */
#include "gmail_auth.h"
#include "json_util.h"
#include "logger.h"
#include "raii.h"
#include "http_client.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Built-in OAuth2 credentials (set via CMake -D flags) ─────────── */

#ifndef GMAIL_DEFAULT_CLIENT_ID
#define GMAIL_DEFAULT_CLIENT_ID ""
#endif
#ifndef GMAIL_DEFAULT_CLIENT_SECRET
#define GMAIL_DEFAULT_CLIENT_SECRET ""
#endif

/** 1 when the test hook asks us to behave as a build with no built-in
 *  credentials.  Whether GMAIL_DEFAULT_CLIENT_ID is compiled in is a property
 *  of the build (gmail_credentials.cmake), so without this the "credentials
 *  are not configured" path is unreachable — and untestable — in any build
 *  that ships them. */
static int no_builtin_credentials(void) {
    const char *v = getenv("GMAIL_TEST_NO_CLIENT_ID");
    return (v && v[0]) ? 1 : 0;
}

static const char *get_client_id(const Config *cfg) {
    if (cfg->gmail_client_id && cfg->gmail_client_id[0])
        return cfg->gmail_client_id;
    return no_builtin_credentials() ? "" : GMAIL_DEFAULT_CLIENT_ID;
}

static const char *get_client_secret(const Config *cfg) {
    if (cfg->gmail_client_secret && cfg->gmail_client_secret[0])
        return cfg->gmail_client_secret;
    return no_builtin_credentials() ? "" : GMAIL_DEFAULT_CLIENT_SECRET;
}

/* ── HTTP POST helper ─────────────────────────────────────────────── */

static char *http_post(const char *url, const char *postdata, long *http_code) {
    const char *headers[] = { "Content-Type: application/x-www-form-urlencoded", NULL };
    HttpRequest req = {
        .method = "POST", .url = url, .headers = headers,
        .body = postdata, .body_len = postdata ? strlen(postdata) : 0,
        .timeout_s = 30,
    };
    char *resp = NULL;
    char err[160];
    if (http_request(&req, http_code, &resp, NULL, err, sizeof(err)) != 0) {
        logger_log(LOG_ERROR, "gmail_auth: HTTP POST %s failed: %s", url, err);
        return NULL;
    }
    return resp;
}

/* ── Reasons ─────────────────────────────────────────────────────── */

/** Record why the authorization failed (the library prints nothing). */
static void ui_error(GmailAuthUi *ui, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ui_error(GmailAuthUi *ui, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->error, sizeof(ui->error), fmt, ap);
    va_end(ap);
    logger_log(LOG_ERROR, "gmail_auth: %s", ui->error);
}

/* ── Localhost redirect listener ─────────────────────────────────── */

#define LOOPBACK_PORT_START 8089
#define LOOPBACK_PORT_END   8099

/**
 * @brief Open a TCP listener on localhost and return the port.
 * Tries ports 8089–8099 until one is available.
 * @param listen_fd  On success, set to the listening socket fd.
 * @return The port number, or -1 on failure.
 */
static int open_listener(int *listen_fd) {
    for (int port = LOOPBACK_PORT_START; port <= LOOPBACK_PORT_END; port++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)port);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
            listen(fd, 1) == 0) {
            *listen_fd = fd;
            return port;
        }
        close(fd);
    }
    return -1;
}

/** Percent-decode @p s in place ('+' is left alone: codes never carry spaces). */
static void percent_decode(char *s) {
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], '\0' };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/** The value of query parameter @p name in @p text (heap, decoded), or NULL. */
static char *query_param(const char *text, const char *name) {
    size_t nl = strlen(name);
    for (const char *p = text; (p = strstr(p, name)) != NULL; p += nl) {
        int boundary = p == text || p[-1] == '?' || p[-1] == '&' || p[-1] == ' ';
        if (!boundary || p[nl] != '=') continue;
        const char *v = p + nl + 1;
        const char *e = v;
        while (*e && *e != '&' && *e != ' ' && *e != '\r' && *e != '\n' && *e != '#') e++;
        char *val = strndup(v, (size_t)(e - v));
        if (val) percent_decode(val);
        return val;
    }
    return NULL;
}

/**
 * @brief The authorization code in what the user's browser was redirected to.
 *
 * @p text is an HTTP request line ("GET /callback?code=... HTTP/1.1"), the
 * redirect URL a person pasted, its query string, or the bare code.
 * @return heap code, or NULL with the reason in @p ui->error.
 */
static char *auth_code_from(const char *text, GmailAuthUi *ui) {
    char *code = query_param(text, "code");
    if (code && code[0]) return code;
    free(code);
    char *error = query_param(text, "error");
    if (error) {
        ui_error(ui, "Authorization denied: %s", error[0] ? error : "unknown");
        free(error);
        return NULL;
    }
    /* Not a URL: the bare code, if it looks like one. */
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' ||
                 text[n - 1] == '\r' || text[n - 1] == '\n')) n--;
    if (n == 0 || memchr(text, ' ', n) || memchr(text, '?', n) || memchr(text, '=', n) ||
        strncmp(text, "http", 4) == 0 || text[0] == '/') {
        ui_error(ui, "No authorization code in the pasted text");
        return NULL;
    }
    code = strndup(text, n);
    if (code) percent_decode(code);
    return code;
}

/**
 * @brief Wait for Google's redirect and extract the authorization code.
 *
 * Accepts one HTTP connection, reads the GET request, extracts the code,
 * sends a simple HTML response and closes the connection.
 *
 * @param listen_fd  Listening socket fd (will NOT be closed by this function).
 * @return Heap-allocated authorization code, or NULL (reason in @p ui->error).
 */
static char *wait_for_auth_code(int listen_fd, GmailAuthUi *ui) {
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    int conn = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
    if (conn < 0) {
        ui_error(ui, "Did not receive the authorization code (timed out)");
        return NULL;
    }

    /* Read the HTTP request (small — fits in 4K) */
    char req[4096] = {0};
    ssize_t n = read(conn, req, sizeof(req) - 1);
    if (n <= 0) {
        close(conn);
        ui_error(ui, "Did not receive the authorization code");
        return NULL;
    }
    /* Only the request line matters; the rest is headers. */
    char *eol = strpbrk(req, "\r\n");
    if (eol) *eol = '\0';

    char *auth_code = auth_code_from(req, ui);

    /* Send a simple response to the browser */
    const char *html = auth_code
        ? "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
          "<html><body><h2>Authorization successful!</h2>"
          "<p>You can close this tab and return to email-cli.</p></body></html>"
        : "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
          "<html><body><h2>Authorization failed.</h2>"
          "<p>Please try again.</p></body></html>";
    ssize_t wr = write(conn, html, strlen(html)); (void)wr;
    close(conn);

    if (!auth_code && !ui->error[0])
        ui_error(ui, "Did not receive the authorization code");
    return auth_code;
}

/* ── Authorization Code Flow (installed application, loopback redirect) ── */

#define AUTH_URL    "https://accounts.google.com/o/oauth2/v2/auth"
#define TOKEN_URL   "https://oauth2.googleapis.com/token"
#define GMAIL_SCOPE "https://mail.google.com/"

int gmail_auth_authorize(Config *cfg, GmailAuthUi *ui) {
    if (!ui) return -1;
    ui->error[0] = '\0';
    if (!cfg || !ui->show_url || (ui->paste && !ui->read_redirect)) {
        ui_error(ui, "The authorization needs a way to show the URL%s",
                 ui->paste ? " and to read the pasted redirect" : "");
        return -1;
    }
    const char *client_id = get_client_id(cfg);
    if (!client_id[0]) {
        ui_error(ui,
            "Gmail OAuth2 credentials are not configured yet.\n"
            "Add them to the account config file\n"
            "(~/.config/email-cli/accounts/<email>/config.ini):\n"
            "  GMAIL_CLIENT_ID=<your-client-id>.apps.googleusercontent.com\n"
            "  GMAIL_CLIENT_SECRET=<your-client-secret>\n"
            "Run 'email-cli help gmail' for the step-by-step setup guide.");
        return -1;
    }

    const char *client_secret = get_client_secret(cfg);

    /* Step 1: the redirect target.  Loopback mode listens on it; paste mode
     * only names it, and the person copies the address the browser ends up
     * on (which fails to load, and does not matter). */
    int listen_fd = -1;
    int port;
    if (ui->paste) {
        port = LOOPBACK_PORT_START;
    } else {
        port = open_listener(&listen_fd);
        if (port < 0) {
            ui_error(ui, "Could not open a localhost listener (ports %d-%d busy)",
                     LOOPBACK_PORT_START, LOOPBACK_PORT_END);
            return -1;
        }
        /* 5-minute timeout so wait_for_auth_code() can't block forever. */
        struct timeval _tv = {300, 0};
        setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &_tv, sizeof(_tv));
    }

    char redirect_uri[64];
    snprintf(redirect_uri, sizeof(redirect_uri), "http://localhost:%d/callback", port);

    /* Step 2: Build the authorization URL and hand it to the program */
    RAII_STRING char *auth_url = NULL;
    char *escaped_scope = http_url_escape(GMAIL_SCOPE);
    char *escaped_redirect = http_url_escape(redirect_uri);
    if (asprintf(&auth_url,
                 "%s?client_id=%s&redirect_uri=%s&response_type=code"
                 "&scope=%s&access_type=offline&prompt=consent",
                 AUTH_URL, client_id, escaped_redirect, escaped_scope) == -1) {
        free(escaped_scope);
        free(escaped_redirect);
        if (listen_fd >= 0) close(listen_fd);
        ui_error(ui, "Out of memory");
        return -1;
    }
    free(escaped_scope);
    free(escaped_redirect);

    ui->show_url(auth_url, port, ui->user);

    /* Step 3: the redirect with the auth code */
    char *auth_code = NULL;
    if (ui->paste) {
        char *text = ui->read_redirect(ui->user);
        if (!text) ui_error(ui, "No redirect address was given");
        else {
            auth_code = auth_code_from(text, ui);
            free(text);
        }
    } else {
        auth_code = wait_for_auth_code(listen_fd, ui);
        close(listen_fd);
    }
    if (!auth_code) return -1;

    /* Step 4: Exchange the auth code for tokens */
    RAII_STRING char *post = NULL;
    char *escaped_code = http_url_escape(auth_code);
    char *escaped_id = http_url_escape(client_id);
    char *escaped_secret = http_url_escape(client_secret);
    char *escaped_uri = http_url_escape(redirect_uri);
    int pr = (escaped_code && escaped_id && escaped_secret && escaped_uri)
           ? asprintf(&post,
                      "code=%s&client_id=%s&client_secret=%s"
                      "&redirect_uri=%s&grant_type=authorization_code",
                      escaped_code, escaped_id, escaped_secret, escaped_uri)
           : -1;
    free(escaped_code);
    free(escaped_id);
    free(escaped_secret);
    free(escaped_uri);
    free(auth_code);
    if (pr == -1) {
        ui_error(ui, "Out of memory");
        return -1;
    }

    const char *url_override2 = getenv("GMAIL_TEST_TOKEN_URL");
    const char *token_url2 = (url_override2 && url_override2[0]) ? url_override2 : TOKEN_URL;

    long tcode = 0;
    RAII_STRING char *tresp = http_post(token_url2, post, &tcode);
    if (!tresp || tcode != 200) {
        RAII_STRING char *why = tresp ? json_get_string(tresp, "error_description") : NULL;
        ui_error(ui, "Token exchange failed (HTTP %ld)%s%s", tcode,
                 why ? ": " : "", why ? why : "");
        return -1;
    }

    char *access  = json_get_string(tresp, "access_token");
    char *refresh = json_get_string(tresp, "refresh_token");

    if (!access) {
        free(refresh);
        ui_error(ui, "Token response missing access_token");
        return -1;
    }
    free(access);  /* We only persist the refresh_token */

    if (refresh) {
        free(cfg->gmail_refresh_token);
        cfg->gmail_refresh_token = refresh;
    }

    logger_log(LOG_INFO, "gmail_auth: authorization completed for %s",
               cfg->user ? cfg->user : "(unknown)");
    return 0;
}

/* ── Token Refresh ────────────────────────────────────────────────── */

char *gmail_auth_refresh(const Config *cfg) {
    /* Test hook: if GMAIL_TEST_TOKEN is set, skip real OAuth and return it directly */
    const char *test_token = getenv("GMAIL_TEST_TOKEN");
    if (test_token && test_token[0])
        return strdup(test_token);

    if (!cfg->gmail_refresh_token || !cfg->gmail_refresh_token[0]) {
        logger_log(LOG_ERROR, "gmail_auth: no refresh_token available");
        return NULL;
    }

    const char *client_id     = get_client_id(cfg);
    const char *client_secret = get_client_secret(cfg);

    RAII_STRING char *post = NULL;
    if (asprintf(&post,
                 "grant_type=refresh_token&client_id=%s&client_secret=%s"
                 "&refresh_token=%s",
                 client_id, client_secret, cfg->gmail_refresh_token) == -1)
        return NULL;

    /* Test hook: GMAIL_TEST_TOKEN_URL overrides the token endpoint for unit tests */
    const char *url_override = getenv("GMAIL_TEST_TOKEN_URL");
    const char *token_url = (url_override && url_override[0]) ? url_override : TOKEN_URL;

    long code = 0;
    RAII_STRING char *resp = http_post(token_url, post, &code);
    if (!resp) return NULL;

    if (code == 200) {
        char *token = json_get_string(resp, "access_token");
        if (token)
            logger_log(LOG_DEBUG, "gmail_auth: access token refreshed");
        return token;
    }

    /* Error handling */
    RAII_STRING char *error = json_get_string(resp, "error");
    if (error && strcmp(error, "invalid_grant") == 0) {
        logger_log(LOG_WARN, "gmail_auth: refresh token expired or revoked");
        /* Name the cause and the cure.  "Re-authorization needed" was true and
         * useless: there was no command to do it with, and nothing said that a
         * changed Google password is what revokes the token. */
        fprintf(stderr,
                "ERROR: Google has revoked the authorization for %s.\n"
                "  Changing the Google account password revokes it; so does\n"
                "  withdrawing access at myaccount.google.com.\n"
                "  Renew it with: email-cli config reauth\n",
                (cfg->user && cfg->user[0]) ? cfg->user : "this account");
    } else if (error && strcmp(error, "invalid_client") == 0) {
        fprintf(stderr, "OAuth2 client credentials are invalid. "
                        "Check GMAIL_CLIENT_ID/SECRET in config.ini.\n"
                        "  'email-cli help gmail' walks through creating them.\n");
    } else {
        logger_log(LOG_ERROR, "gmail_auth: token refresh failed (HTTP %ld): %s",
                   code, error ? error : "unknown");
    }

    return NULL;
}
