#include "gmail_auth.h"
#include "json_util.h"
#include "logger.h"
#include "raii.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/**
 * @brief POST form-encoded data and return the response body.
 *
 * @param url       Target URL.
 * @param postdata  URL-encoded form body.
 * @param http_code On success, set to the HTTP response status code.
 * @return Heap-allocated response body, or NULL on network error.
 *         Caller must free().
 */
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

/* ── Device Authorization Grant (RFC 8628) ────────────────────────── */

#define DEVICE_CODE_URL "https://oauth2.googleapis.com/device/code"
#define TOKEN_URL       "https://oauth2.googleapis.com/token"
#define GMAIL_SCOPE     "https://mail.google.com/"

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
            "  See docs/dev/gmail-oauth2-setup.md for a step-by-step guide\n"
            "  on creating credentials in the Google Cloud Console.\n"
            "\n");
        return -1;
    }

    /* Step 1: Request device code */
    RAII_STRING char *post1 = NULL;
    if (asprintf(&post1, "client_id=%s&scope=%s", client_id, GMAIL_SCOPE) == -1)
        return -1;

    long code = 0;
    RAII_STRING char *resp1 = http_post(DEVICE_CODE_URL, post1, &code);
    if (!resp1 || code != 200) {
        fprintf(stderr, "Error: Failed to start Gmail authorization (HTTP %ld).\n", code);
        if (resp1) {
            RAII_STRING char *err = json_get_string(resp1, "error_description");
            if (err) fprintf(stderr, "  %s\n", err);
        }
        return -1;
    }

    RAII_STRING char *device_code      = json_get_string(resp1, "device_code");
    RAII_STRING char *user_code        = json_get_string(resp1, "user_code");
    RAII_STRING char *verification_url = json_get_string(resp1, "verification_url");
    int expires_in = 0, interval = 5;
    json_get_int(resp1, "expires_in", &expires_in);
    json_get_int(resp1, "interval", &interval);

    if (!device_code || !user_code || !verification_url) {
        fprintf(stderr, "Error: Unexpected device code response.\n");
        return -1;
    }
    if (interval < 1) interval = 5;
    if (expires_in < 1) expires_in = 300;

    /* Step 2: Display instructions */
    fprintf(stderr, "\n  Go to: %s\n", verification_url);
    fprintf(stderr, "  Enter code: %s\n", user_code);
    fprintf(stderr, "  (Waiting for authorization... ^C to cancel)\n\n");

    /* Step 3: Poll for token */
    const char *client_secret = get_client_secret(cfg);
    RAII_STRING char *post2 = NULL;
    if (asprintf(&post2,
                 "client_id=%s&client_secret=%s&device_code=%s"
                 "&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
                 client_id, client_secret, device_code) == -1)
        return -1;

    int elapsed = 0;
    while (elapsed < expires_in) {
        sleep((unsigned)interval);
        elapsed += interval;

        long tcode = 0;
        char *tresp = http_post(TOKEN_URL, post2, &tcode);
        if (!tresp) continue;  /* Network error — retry silently */

        if (tcode == 200) {
            /* Success */
            char *access  = json_get_string(tresp, "access_token");
            char *refresh = json_get_string(tresp, "refresh_token");
            free(tresp);

            if (!access) {
                free(refresh);
                fprintf(stderr, "Error: Token response missing access_token.\n");
                return -1;
            }
            free(access);  /* We only need refresh_token long-term */

            if (refresh) {
                free(cfg->gmail_refresh_token);
                cfg->gmail_refresh_token = refresh;
            }

            fprintf(stderr, "  Authorization successful.\n\n");
            logger_log(LOG_INFO, "gmail_auth: device flow completed for %s",
                       cfg->user ? cfg->user : "(unknown)");
            return 0;
        }

        /* Check error */
        RAII_STRING char *error = json_get_string(tresp, "error");
        free(tresp);

        if (error && strcmp(error, "authorization_pending") == 0)
            continue;  /* User hasn't authorised yet */
        if (error && strcmp(error, "slow_down") == 0) {
            interval += 5;  /* Server asked us to slow down */
            continue;
        }

        /* Any other error is fatal */
        fprintf(stderr, "Error: Authorization failed (%s).\n", error ? error : "unknown");
        return -1;
    }

    fprintf(stderr, "Error: Authorization timed out.\n");
    return -1;
}

/* ── Token Refresh ────────────────────────────────────────────────── */

char *gmail_auth_refresh(const Config *cfg) {
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

    long code = 0;
    RAII_STRING char *resp = http_post(TOKEN_URL, post, &code);
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
