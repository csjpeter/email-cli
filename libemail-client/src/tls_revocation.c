/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* Revocation check (OCSP, then CRL) of a verified server certificate. */
#include "tls_revocation.h"
#include "fs_util.h"
#include "http_client.h"
#include "logger.h"
#include "path.h"

#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * @file tls_revocation.c
 * @brief OCSP and CRL revocation check, see tls_revocation.h.
 */

#define REV_FETCH_TOTAL_S    5L
#define REV_FETCH_MAX_BYTES  (8u * 1024u * 1024u)
#define REV_OCSP_SKEW_S      300L
#define REV_OCSP_CACHE_MAX_S 3600L
#define REV_OCSP_CACHE_SLOTS 32

/* ── State ──────────────────────────────────────────────────────────── */

static char g_state_dir[1024];          /* "" = default, resolved on use */
static char g_notice[512];

typedef struct {
    unsigned char fp[32];
    int           revoked;
    time_t        until;
} OcspCacheEntry;

static OcspCacheEntry g_ocsp_cache[REV_OCSP_CACHE_SLOTS];
static int            g_ocsp_cache_next;

void tls_revocation_set_state_dir(const char *dir)
{
    snprintf(g_state_dir, sizeof(g_state_dir), "%s", dir ? dir : "");
}

void tls_revocation_reset(void)
{
    memset(g_ocsp_cache, 0, sizeof(g_ocsp_cache));
    g_ocsp_cache_next = 0;
    g_notice[0] = '\0';
}

int tls_revocation_take_notice(char *buf, size_t size)
{
    if (!g_notice[0] || !buf || size == 0) return 0;
    snprintf(buf, size, "%s", g_notice);
    g_notice[0] = '\0';
    return 1;
}

/** The directory of the CRL cache and the signal record; "" when unknown. */
static const char *state_dir(void)
{
    static char dflt[1024];
    if (g_state_dir[0]) return g_state_dir;
    const char *cache = platform_cache_dir();
    if (!cache) return "";
    snprintf(dflt, sizeof(dflt), "%s/email-cli/revocation", cache);
    return dflt;
}

/* ── HTTP fetch (OCSP request, CRL download) ────────────────────────── */

typedef struct { unsigned char *data; size_t len; int overflow; } Body;

static size_t body_append(void *ptr, size_t size, size_t nmemb, void *ud)
{
    Body *b = ud;
    size_t n = size * nmemb;
    if (b->len + n > REV_FETCH_MAX_BYTES) { b->overflow = 1; return 0; }
    unsigned char *t = realloc(b->data, b->len + n + 1);
    if (!t) return 0;
    b->data = t;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/** GET (post == NULL) or POST @p url over plain HTTP; 0 on HTTP 200. */
static int http_fetch(const char *url, const unsigned char *post, size_t post_len,
                      Body *out)
{
    if (strncmp(url, "http://", 7) != 0) return -1;   /* OCSP and CRLs are plain HTTP */
    const char *hdr[] = { "Content-Type: application/ocsp-request", NULL };
    HttpRequest req = {
        .method = post ? "POST" : "GET", .url = url,
        .headers = post ? hdr : NULL,
        .body = post, .body_len = post_len,
        .timeout_s = (int)REV_FETCH_TOTAL_S, .max_response = REV_FETCH_MAX_BYTES,
    };
    long code = 0;
    char *data = NULL;
    size_t len = 0;
    char err[128];
    if (http_request(&req, &code, &data, &len, err, sizeof(err)) != 0 || code != 200) {
        free(data);
        return -1;
    }
    free(out->data);
    out->data = (unsigned char *)data;
    out->len = len;
    return 0;
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static void fingerprint(X509 *cert, unsigned char fp[32])
{
    unsigned int n = 32;
    if (X509_digest(cert, EVP_sha256(), fp, &n) != 1) memset(fp, 0, 32);
}

static void hex32(const unsigned char fp[32], char out[65])
{
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", fp[i]);
}

static X509 *find_issuer(X509 *cert, STACK_OF(X509) *chain)
{
    for (int i = 0; chain && i < sk_X509_num(chain); i++) {
        X509 *c = sk_X509_value(chain, i);
        if (c != cert && X509_check_issued(c, cert) == X509_V_OK) return c;
    }
    return NULL;
}

/* ── OCSP ───────────────────────────────────────────────────────────── */

/**
 * Ask the OCSP responders the certificate names.
 * @return 1 good, 0 revoked, -1 no usable answer.
 */
static int ocsp_check(X509 *cert, X509 *issuer, STACK_OF(X509) *chain,
                      X509_STORE *store, const unsigned char fp[32],
                      char *why, size_t why_len)
{
    STACK_OF(OPENSSL_STRING) *urls = X509_get1_ocsp(cert);
    if (!urls || sk_OPENSSL_STRING_num(urls) == 0) {
        snprintf(why, why_len, "the certificate names no OCSP responder");
        if (urls) X509_email_free(urls);
        return -1;
    }
    time_t now = time(NULL);
    for (int i = 0; i < REV_OCSP_CACHE_SLOTS; i++) {
        if (g_ocsp_cache[i].until > now &&
            memcmp(g_ocsp_cache[i].fp, fp, 32) == 0) {
            int rv = g_ocsp_cache[i].revoked ? 0 : 1;
            X509_email_free(urls);
            return rv;
        }
    }
    int result = -1;
    snprintf(why, why_len, "no OCSP responder could be reached");
    for (int i = 0; i < sk_OPENSSL_STRING_num(urls) && result < 0; i++) {
        const char *url = sk_OPENSSL_STRING_value(urls, i);
        OCSP_CERTID *id = OCSP_cert_to_id(NULL, cert, issuer);
        OCSP_REQUEST *req = OCSP_REQUEST_new();
        if (!id || !req) { OCSP_CERTID_free(id); OCSP_REQUEST_free(req); continue; }
        OCSP_request_add0_id(req, id);   /* req owns id now */
        unsigned char *der = NULL;
        int der_len = i2d_OCSP_REQUEST(req, &der);
        Body body = {0};
        int frc = der_len > 0 ? http_fetch(url, der, (size_t)der_len, &body) : -1;
        OPENSSL_free(der);
        OCSP_REQUEST_free(req);
        if (frc != 0) continue;
        const unsigned char *p = body.data;
        OCSP_RESPONSE *resp = d2i_OCSP_RESPONSE(NULL, &p, (long)body.len);
        free(body.data);
        if (!resp) { snprintf(why, why_len, "unreadable OCSP answer"); continue; }
        if (OCSP_response_status(resp) != OCSP_RESPONSE_STATUS_SUCCESSFUL) {
            OCSP_RESPONSE_free(resp);
            snprintf(why, why_len, "the OCSP responder gave no answer");
            continue;
        }
        OCSP_BASICRESP *bs = OCSP_response_get1_basic(resp);
        OCSP_RESPONSE_free(resp);
        if (!bs) continue;
        if (OCSP_basic_verify(bs, chain, store, 0) <= 0) {
            OCSP_BASICRESP_free(bs);
            snprintf(why, why_len, "the OCSP answer is not signed by a trusted party");
            continue;
        }
        OCSP_CERTID *want = OCSP_cert_to_id(NULL, cert, issuer);
        int status = -1, reason = 0;
        ASN1_GENERALIZEDTIME *rev = NULL, *thisupd = NULL, *nextupd = NULL;
        int found = want && OCSP_resp_find_status(bs, want, &status, &reason, &rev,
                                                  &thisupd, &nextupd);
        if (found && OCSP_check_validity(thisupd, nextupd, REV_OCSP_SKEW_S, -1) &&
            (status == V_OCSP_CERTSTATUS_GOOD || status == V_OCSP_CERTSTATUS_REVOKED)) {
            result = status == V_OCSP_CERTSTATUS_GOOD ? 1 : 0;
            time_t until = now + REV_OCSP_CACHE_MAX_S;
            if (nextupd) {
                struct tm tmv;
                if (ASN1_TIME_to_tm(nextupd, &tmv) == 1) {
                    time_t nu = timegm(&tmv);
                    if (nu < until) until = nu;
                }
            }
            OcspCacheEntry *e = &g_ocsp_cache[g_ocsp_cache_next];
            g_ocsp_cache_next = (g_ocsp_cache_next + 1) % REV_OCSP_CACHE_SLOTS;
            memcpy(e->fp, fp, 32);
            e->revoked = result == 0;
            e->until = until;
        } else {
            snprintf(why, why_len, "the OCSP answer does not decide for this certificate");
        }
        OCSP_CERTID_free(want);
        OCSP_BASICRESP_free(bs);
    }
    X509_email_free(urls);
    return result;
}

/* ── CRL ────────────────────────────────────────────────────────────────── */

static void crl_cache_path(const char *url, char *out, size_t n)
{
    unsigned char md[32];
    unsigned int mdl = 32;
    EVP_Digest(url, strlen(url), md, &mdl, EVP_sha256(), NULL);
    char hex[65];
    hex32(md, hex);
    snprintf(out, n, "%s/crl-%s.der", state_dir(), hex);
}

static X509_CRL *crl_from_bytes(const unsigned char *d, size_t len)
{
    const unsigned char *p = d;
    X509_CRL *crl = d2i_X509_CRL(NULL, &p, (long)len);
    if (crl) return crl;
    BIO *bio = BIO_new_mem_buf(d, (int)len);       /* PEM, as some CAs serve it */
    if (!bio) return NULL;
    crl = PEM_read_bio_X509_CRL(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return crl;
}

static int read_file(const char *path, Body *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        if ((size_t)body_append(buf, 1, n, out) != n) { fclose(f); return -1; }
    fclose(f);
    return out->len ? 0 : -1;
}

static void write_cache(const char *path, const unsigned char *d, size_t len)
{
    if (!state_dir()[0]) return;
    if (fs_mkdir_p(state_dir(), 0700) != 0) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (fwrite(d, 1, len, f) != len)
        logger_log(LOG_WARN, "CRL cache write failed: %s", path);
    fclose(f);
}

/** 1 = valid now, 0 = expired (or not yet valid). */
static int crl_current(X509_CRL *crl)
{
    const ASN1_TIME *nu = X509_CRL_get0_nextUpdate(crl);
    const ASN1_TIME *lu = X509_CRL_get0_lastUpdate(crl);
    if (lu && X509_cmp_current_time(lu) > 0) return 0;
    if (!nu) return 0;
    return X509_cmp_current_time(nu) > 0;
}

/**
 * Check the CRLs the certificate names.
 * @return 1 good, 0 revoked, -1 no usable CRL.
 */
static int crl_check(X509 *cert, X509 *issuer, char *why, size_t why_len)
{
    STACK_OF(DIST_POINT) *dps = X509_get_ext_d2i(cert, NID_crl_distribution_points,
                                                  NULL, NULL);
    if (!dps) {
        snprintf(why, why_len, "the certificate names no CRL");
        return -1;
    }
    int result = -1, expired = 0, seen = 0;
    EVP_PKEY *ikey = issuer ? X509_get0_pubkey(issuer) : NULL;
    for (int i = 0; i < sk_DIST_POINT_num(dps) && result < 0; i++) {
        DIST_POINT *dp = sk_DIST_POINT_value(dps, i);
        if (!dp->distpoint || dp->distpoint->type != 0) continue;
        GENERAL_NAMES *names = dp->distpoint->name.fullname;
        for (int j = 0; j < sk_GENERAL_NAME_num(names) && result < 0; j++) {
            GENERAL_NAME *gn = sk_GENERAL_NAME_value(names, j);
            if (gn->type != GEN_URI) continue;
            const char *url = (const char *)ASN1_STRING_get0_data(gn->d.uniformResourceIdentifier);
            char path[1200];
            crl_cache_path(url, path, sizeof(path));
            X509_CRL *crl = NULL;
            Body cached = {0};
            if (state_dir()[0] && read_file(path, &cached) == 0) {
                crl = crl_from_bytes(cached.data, cached.len);
                if (crl && !crl_current(crl)) { expired = 1; X509_CRL_free(crl); crl = NULL; }
            }
            free(cached.data);
            if (!crl) {
                Body body = {0};
                if (http_fetch(url, NULL, 0, &body) == 0) {
                    crl = crl_from_bytes(body.data, body.len);
                    if (crl && ikey && X509_CRL_verify(crl, ikey) == 1) {
                        if (crl_current(crl)) write_cache(path, body.data, body.len);
                        else { expired = 1; X509_CRL_free(crl); crl = NULL; }
                    } else if (crl) {
                        X509_CRL_free(crl); crl = NULL;
                    }
                }
                free(body.data);
            }
            if (!crl) continue;
            seen = 1;
            X509_REVOKED *rv = NULL;
            int r = X509_CRL_get0_by_cert(crl, &rv, cert);
            result = (r == 1 && rv) ? 0 : 1;
            X509_CRL_free(crl);
        }
    }
    sk_DIST_POINT_pop_free(dps, DIST_POINT_free);
    if (result < 0)
        snprintf(why, why_len, expired ? "the CRL has expired"
                                       : (seen ? "the CRL could not be used"
                                               : "no CRL could be fetched"));
    return result;
}

/* ── The check and its signal ───────────────────────────────────────── */

TlsRevStatus tls_revocation_check(X509 *cert, STACK_OF(X509) *chain,
                                  X509_STORE *store, char *why, size_t why_len)
{
    char w_ocsp[160] = "", w_crl[160] = "";
    char local[8];
    if (!why) { why = local; why_len = sizeof(local); }
    why[0] = '\0';
    X509 *issuer = find_issuer(cert, chain);
    if (!issuer) {
        snprintf(why, why_len, "the issuer of the certificate is not in the chain");
        return TLS_REV_UNKNOWN;
    }
    unsigned char fp[32];
    fingerprint(cert, fp);

    int o = ocsp_check(cert, issuer, chain, store, fp, w_ocsp, sizeof(w_ocsp));
    if (o >= 0) {
        snprintf(why, why_len, "OCSP");
        return o ? TLS_REV_GOOD : TLS_REV_REVOKED;
    }
    int c = crl_check(cert, issuer, w_crl, sizeof(w_crl));
    if (c >= 0) {
        snprintf(why, why_len, "CRL");
        return c ? TLS_REV_GOOD : TLS_REV_REVOKED;
    }
    snprintf(why, why_len, "%s; %s", w_ocsp, w_crl);
    return TLS_REV_UNKNOWN;
}

/** True when a signal for @p fp was given less than 6 hours ago; records it
 *  when not. */
static int signalled_recently(const unsigned char fp[32])
{
    char hex[65];
    hex32(fp, hex);
    time_t now = time(NULL);
    if (!state_dir()[0]) return 0;
    char path[1200];
    snprintf(path, sizeof(path), "%s/signals", state_dir());

    /* keep the entries that are still within the interval, drop the rest */
    char lines[64][96];
    int n = 0, recent = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char l[160];
        while (n < 63 && fgets(l, sizeof(l), f)) {
            char h[80];
            long long t;
            if (sscanf(l, "%79s %lld", h, &t) != 2) continue;
            if ((long long)now - t >= TLS_REV_SIGNAL_INTERVAL) continue;
            if (strcmp(h, hex) == 0) { recent = 1; }
            snprintf(lines[n++], sizeof(lines[0]), "%s %lld\n", h, t);
        }
        fclose(f);
    }
    if (recent) return 1;
    snprintf(lines[n++], sizeof(lines[0]), "%s %lld\n", hex, (long long)now);
    if (fs_mkdir_p(state_dir(), 0700) != 0) return 0;
    f = fopen(path, "w");
    if (!f) return 0;
    for (int i = 0; i < n; i++) fputs(lines[i], f);
    fclose(f);
    return 0;
}

static void signal_unknown(X509 *cert, const char *why)
{
    unsigned char fp[32];
    fingerprint(cert, fp);
    if (signalled_recently(fp)) return;
    char subj[200] = "?";
    X509_NAME_oneline(X509_get_subject_name(cert), subj, sizeof(subj));
    logger_log(LOG_ERROR,
               "revocation of the certificate %s could not be checked (%s); "
               "the connection goes on", subj, why);
    snprintf(g_notice, sizeof(g_notice),
             "Could not check whether the server certificate %s is revoked (%s). "
             "The connection goes on.", subj, why);
}

int tls_revocation_verify_cb(int preverify_ok, X509_STORE_CTX *sctx)
{
    if (!preverify_ok) return preverify_ok;
    if (X509_STORE_CTX_get_error_depth(sctx) != 0) return preverify_ok;
    X509 *cert = X509_STORE_CTX_get_current_cert(sctx);
    if (!cert) return preverify_ok;
    char why[320];
    TlsRevStatus st = tls_revocation_check(cert, X509_STORE_CTX_get0_chain(sctx),
                                           X509_STORE_CTX_get0_store(sctx),
                                           why, sizeof(why));
    if (st == TLS_REV_REVOKED) {
        char subj[200] = "?";
        X509_NAME_oneline(X509_get_subject_name(cert), subj, sizeof(subj));
        logger_log(LOG_ERROR, "server certificate %s is revoked (%s): refused",
                   subj, why);
        X509_STORE_CTX_set_error(sctx, X509_V_ERR_CERT_REVOKED);
        return 0;
    }
    if (st == TLS_REV_UNKNOWN) signal_unknown(cert, why);
    return preverify_ok;
}

void tls_revocation_attach(SSL_CTX *ctx)
{
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, tls_revocation_verify_cb);
}
