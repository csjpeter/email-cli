#include "gmail_auth.h"
#include "json_util.h"
#include "logger.h"
#include "raii.h"
#include <curl/curl.h>
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

static const char *get_client_id(const Config *cfg) {
    return (cfg->gmail_client_id && cfg->gmail_client_id[0])
        ? cfg->gmail_client_id : GMAIL_DEFAULT_CLIENT_ID;
}

static const char *get_client_secret(const Config *cfg) {
    return (cfg->gmail_client_secret && cfg->gmail_client_secret[0])
        ? cfg->gmail_client_secret : GMAIL_DEFAULT_CLIENT_SECRET;
}

/* ── libcurl write callback ───────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} CurlBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuf *buf = userdata;
    size_t bytes = size * nmemb;
    if (buf->len + bytes + 1 > buf->cap) {
        size_t newcap = (buf->cap ? buf->cap * 2 : 4096);
        while (newcap < buf->len + bytes + 1) newcap *= 2;
        char *tmp = realloc(buf->data, newcap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap = newcap;
    }
    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    buf->data[buf->len] = '\0';
    return bytes;
}

/* ── HTTP POST helper ─────────────────────────────────────────────── */

static char *http_post(const char *url, const char *postdata, long *http_code) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    CurlBuf buf = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postdata);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        logger_log(LOG_ERROR, "gmail_auth: HTTP POST %s failed: %s",
                   url, curl_easy_strerror(res));
        free(buf.data);
        curl_easy_cleanup(curl);
        return NULL;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
    curl_easy_cleanup(curl);
    return buf.data;
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

/**
 * @brief Wait for Google's redirect and extract the authorization code.
 *
 * Accepts one HTTP connection, reads the GET request, extracts the
 * ?code= parameter, sends a simple HTML "success" response, and
 * closes the connection.
 *
 * @param listen_fd  Listening socket fd (will NOT be closed by this function).
 * @return Heap-allocated authorization code, or NULL on error/denial.
 */
static char *wait_for_auth_code(int listen_fd) {
    struct sockaddr_in cli = {0};
    socklen_t cli_len = sizeof(cli);
    int conn = accept(listen_fd, (struct sockaddr *)&cli, &cli_len);
    if (conn < 0) return NULL;

    /* Read the HTTP request (small — fits in 4K) */
    char req[4096] = {0};
    ssize_t n = read(conn, req, sizeof(req) - 1);
    if (n <= 0) { close(conn); return NULL; }

    /* Extract ?code=... from "GET /callback?code=XXXX&scope=... HTTP/1.1" */
    char *code_start = strstr(req, "code=");
    char *auth_code = NULL;
    if (code_start) {
        code_start += 5;
        char *code_end = code_start;
        while (*code_end && *code_end != '&' && *code_end != ' ' && *code_end != '\r')
            code_end++;
        auth_code = strndup(code_start, (size_t)(code_end - code_start));
    }

    /* Check for error=access_denied */
    char *error_start = strstr(req, "error=");
    if (error_start && !auth_code) {
        error_start += 6;
        char *error_end = error_start;
        while (*error_end && *error_end != '&' && *error_end != ' ') error_end++;
        char *err = strndup(error_start, (size_t)(error_end - error_start));
        fprintf(stderr, "  Authorization denied: %s\n", err ? err : "unknown");
        free(err);
    }

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

    return auth_code;
}

/* ── Authorization Code Flow (Desktop App) ───────────────────────── */

#define AUTH_URL    "https://accounts.google.com/o/oauth2/v2/auth"
#define TOKEN_URL   "https://oauth2.googleapis.com/token"
#define GMAIL_SCOPE "https://mail.google.com/"

int gmail_auth_device_flow(Config *cfg) {
    const char *client_id = get_client_id(cfg);
    if (!client_id[0]) {
        fprintf(stderr,
            "\n"
            "  Gmail OAuth2 credentials are not configured yet.\n"
            "\n"
            "  To use Gmail, you need a Google Cloud OAuth2 client_id and\n"
            "  client_secret. Add them to your account config file:\n"
            "\n"
            "    ~/.config/email-cli/accounts/<email>/config.ini\n"
            "\n"
            "  Add these two lines:\n"
            "    GMAIL_CLIENT_ID=<your-client-id>.apps.googleusercontent.com\n"
            "    GMAIL_CLIENT_SECRET=<your-client-secret>\n"
            "\n"
            "  Run 'email-cli help gmail' for the step-by-step setup guide.\n"
            "\n");
        return -1;
    }

    const char *client_secret = get_client_secret(cfg);

    /* Step 1: Open a localhost listener for the OAuth redirect */
    int listen_fd = -1;
    int port = open_listener(&listen_fd);
    if (port < 0) {
        fprintf(stderr, "Error: Could not open localhost listener (ports %d-%d busy).\n",
                LOOPBACK_PORT_START, LOOPBACK_PORT_END);
        return -1;
    }

    char redirect_uri[64];
    snprintf(redirect_uri, sizeof(redirect_uri), "http://localhost:%d/callback", port);

    /* Step 2: Build the authorization URL and open in browser */
    RAII_STRING char *auth_url = NULL;
    char *escaped_scope = curl_easy_escape(NULL, GMAIL_SCOPE, 0);
    char *escaped_redirect = curl_easy_escape(NULL, redirect_uri, 0);
    if (asprintf(&auth_url,
                 "%s?client_id=%s&redirect_uri=%s&response_type=code"
                 "&scope=%s&access_type=offline&prompt=consent",
                 AUTH_URL, client_id, escaped_redirect, escaped_scope) == -1) {
        curl_free(escaped_scope);
        curl_free(escaped_redirect);
        close(listen_fd);
        return -1;
    }
    curl_free(escaped_scope);
    curl_free(escaped_redirect);

    fprintf(stderr,
        "\n"
        "  Opening browser for Gmail authorization...\n"
        "  If the browser doesn't open, visit this URL manually:\n\n"
        "  %s\n\n"
        "  Waiting for authorization... (^C to cancel)\n\n",
        auth_url);

    /* Try to open the browser */
    RAII_STRING char *browser_cmd = NULL;
    if (asprintf(&browser_cmd, "xdg-open '%s' 2>/dev/null || "
                 "open '%s' 2>/dev/null || "
                 "start '%s' 2>/dev/null",
                 auth_url, auth_url, auth_url) != -1) {
        int rc = system(browser_cmd);
        (void)rc;
    }

    /* Step 3: Wait for the redirect with the auth code */
    char *auth_code = wait_for_auth_code(listen_fd);
    close(listen_fd);

    if (!auth_code) {
        fprintf(stderr, "Error: Did not receive authorization code.\n");
        return -1;
    }

    /* Step 4: Exchange the auth code for tokens */
    RAII_STRING char *post = NULL;
    char *escaped_code = curl_easy_escape(NULL, auth_code, 0);
    if (asprintf(&post,
                 "code=%s&client_id=%s&client_secret=%s"
                 "&redirect_uri=%s&grant_type=authorization_code",
                 escaped_code, client_id, client_secret, redirect_uri) == -1) {
        curl_free(escaped_code);
        free(auth_code);
        return -1;
    }
    curl_free(escaped_code);
    free(auth_code);

    const char *url_override2 = getenv("GMAIL_TEST_TOKEN_URL");
    const char *token_url2 = (url_override2 && url_override2[0]) ? url_override2 : TOKEN_URL;

    long tcode = 0;
    RAII_STRING char *tresp = http_post(token_url2, post, &tcode);
    if (!tresp || tcode != 200) {
        fprintf(stderr, "Error: Token exchange failed (HTTP %ld).\n", tcode);
        if (tresp) {
            RAII_STRING char *err = json_get_string(tresp, "error_description");
            if (err) fprintf(stderr, "  %s\n", err);
        }
        return -1;
    }

    char *access  = json_get_string(tresp, "access_token");
    char *refresh = json_get_string(tresp, "refresh_token");

    if (!access) {
        free(refresh);
        fprintf(stderr, "Error: Token response missing access_token.\n");
        return -1;
    }
    free(access);  /* We only persist the refresh_token */

    if (refresh) {
        free(cfg->gmail_refresh_token);
        cfg->gmail_refresh_token = refresh;
    }

    fprintf(stderr, "  Authorization successful.\n\n");
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
        fprintf(stderr, "Gmail refresh token expired. Re-authorization needed.\n");
    } else if (error && strcmp(error, "invalid_client") == 0) {
        fprintf(stderr, "OAuth2 client credentials are invalid. "
                        "Check GMAIL_CLIENT_ID/SECRET in config.ini.\n");
    } else {
        logger_log(LOG_ERROR, "gmail_auth: token refresh failed (HTTP %ld): %s",
                   code, error ? error : "unknown");
    }

    return NULL;
}
