#include "smtp_adapter.h"
#include "logger.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @file smtp_adapter.c
 * @brief libcurl-based SMTP adapter for sending RFC 2822 messages.
 */

/* ── Read callback for libcurl upload ───────────────────────────────── */

typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
} ReadCtx;

static size_t read_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    ReadCtx *ctx = (ReadCtx *)userdata;
    size_t room = size * nmemb;
    size_t left = ctx->len - ctx->pos;
    if (left == 0) return 0;
    size_t n = left < room ? left : room;
    memcpy(ptr, ctx->data + ctx->pos, n);
    ctx->pos += n;
    return n;
}

/* ── URL construction ─────────────────────────────────────────────── */

/**
 * Build the SMTP URL from config.
 * Returns heap-allocated string; caller must free().
 */
static char *build_smtp_url(const Config *cfg) {
    char *url = NULL;

    if (cfg->smtp_host && cfg->smtp_host[0]) {
        /* Use configured SMTP host; append port if specified */
        if (cfg->smtp_port) {
            /* Check if port is already embedded in the URL */
            const char *after_scheme = strstr(cfg->smtp_host, "://");
            int has_port = after_scheme && strchr(after_scheme + 3, ':') != NULL;
            if (has_port) {
                url = strdup(cfg->smtp_host);
            } else {
                if (asprintf(&url, "%s:%d", cfg->smtp_host, cfg->smtp_port) < 0)
                    url = NULL;
            }
        } else {
            url = strdup(cfg->smtp_host);
        }
    } else if (cfg->host) {
        /* Derive SMTP URL from IMAP URL */
        if (strncmp(cfg->host, "imaps://", 8) == 0) {
            if (asprintf(&url, "smtps://%s", cfg->host + 8) < 0) url = NULL;
        } else if (strncmp(cfg->host, "imap://", 7) == 0) {
            if (asprintf(&url, "smtp://%s", cfg->host + 7) < 0) url = NULL;
        }
        else
            url = strdup(cfg->host);
    } else {
        url = strdup("smtp://localhost");
    }
    return url;
}

/* ── Public API ──────────────────────────────────────────────────── */

int smtp_send(const Config *cfg,
              const char *from,
              const char *to,
              const char *message,
              size_t message_len) {
    if (!cfg || !from || !to || !message) return -1;

    const char *smtp_user = cfg->smtp_user ? cfg->smtp_user : cfg->user;
    const char *smtp_pass = cfg->smtp_pass ? cfg->smtp_pass : cfg->pass;

    char *url = build_smtp_url(cfg);
    if (!url) {
        fprintf(stderr, "smtp_send: failed to build SMTP URL.\n");
        return -1;
    }

    /* Hard enforcement: only smtps:// (implicit TLS) is allowed.
     * Exception: cfg->ssl_no_verify=1 permits smtp:// for test environments. */
    if (strncmp(url, "smtps://", 8) != 0 && !cfg->ssl_no_verify) {
        fprintf(stderr,
                "smtp_send: refused to send via %s — "
                "only smtps:// is allowed (TLS required).\n"
                "Set SMTP_HOST=smtps://smtp.example.com in your config.\n"
                "For test environments only: add SSL_NO_VERIFY=1 to config.\n", url);
        logger_log(LOG_ERROR,
                   "smtp_send: rejected insecure SMTP URL: %s", url);
        free(url);
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "smtp_send: curl_easy_init() failed.\n");
        free(url);
        return -1;
    }

    /* Envelope From: strip display name, extract bare address */
    char from_env[512];
    const char *lt = strchr(from, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt && gt > lt) {
        size_t alen = (size_t)(gt - lt - 1);
        if (alen >= sizeof(from_env)) alen = sizeof(from_env) - 1;
        snprintf(from_env, sizeof(from_env), "<%.*s>", (int)alen, lt + 1);
    } else {
        snprintf(from_env, sizeof(from_env), "<%s>", from);
    }

    char to_env[512];
    const char *lt2 = strchr(to, '<');
    const char *gt2 = lt2 ? strchr(lt2, '>') : NULL;
    if (lt2 && gt2 && gt2 > lt2) {
        size_t alen = (size_t)(gt2 - lt2 - 1);
        if (alen >= sizeof(to_env)) alen = sizeof(to_env) - 1;
        snprintf(to_env, sizeof(to_env), "<%.*s>", (int)alen, lt2 + 1);
    } else {
        snprintf(to_env, sizeof(to_env), "<%s>", to);
    }

    struct curl_slist *rcpt = curl_slist_append(NULL, to_env);

    ReadCtx rctx = {message, message_len, 0};

    curl_easy_setopt(curl, CURLOPT_URL,          url);
    curl_easy_setopt(curl, CURLOPT_USERNAME,      smtp_user ? smtp_user : "");
    curl_easy_setopt(curl, CURLOPT_PASSWORD,      smtp_pass ? smtp_pass : "");
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM,     from_env);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT,     rcpt);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,  read_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA,      &rctx);
    curl_easy_setopt(curl, CURLOPT_UPLOAD,        1L);

    /* smtps:// = implicit TLS (port 465); enforce TLS on the connection */
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);

    /* Honour ssl_no_verify for self-signed certs in test environments */
    if (cfg->ssl_no_verify) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode res = curl_easy_perform(curl);
    int rc = 0;
    if (res != CURLE_OK) {
        fprintf(stderr, "smtp_send: %s\n", curl_easy_strerror(res));
        logger_log(LOG_ERROR, "smtp_send failed: %s", curl_easy_strerror(res));
        rc = -1;
    } else {
        logger_log(LOG_INFO, "smtp_send: message sent from %s to %s", from, to);
    }

    curl_slist_free_all(rcpt);
    curl_easy_cleanup(curl);
    free(url);
    return rc;
}
