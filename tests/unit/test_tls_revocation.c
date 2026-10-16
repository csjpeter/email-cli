/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* Unit tests of the revocation check of server certificates (EMAIL-102). */
#include "test_helpers.h"
#include "tls_revocation.h"
#include "imap_client.h"
#include "email-client.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * @file test_tls_revocation.c
 * @brief OCSP first, CRL as fallback, an expired CRL counts as none, a revoked
 *        certificate is refused, an unavailable check lets the connection go on
 *        with one signal per certificate and 6 hours.  Real certificates are
 *        generated here; the responder and the CRL server are forked mocks.
 */

/* ── A small PKI ────────────────────────────────────────────────────── */

typedef struct {
    EVP_PKEY *ca_key, *leaf_key;
    X509     *ca, *leaf;
    X509_STORE *store;
    STACK_OF(X509) *chain;
} Pki;

static EVP_PKEY *gen_key(void)
{
    return EVP_EC_gen("P-256");
}

static void add_ext(X509 *c, X509 *issuer, int nid, const char *value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer ? issuer : c, c, NULL, NULL, 0);
    X509_EXTENSION *e = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
    if (e) { X509_add_ext(c, e, -1); X509_EXTENSION_free(e); }
}

static X509 *make_cert(EVP_PKEY *key, const char *cn, X509 *issuer,
                       EVP_PKEY *issuer_key, long serial, int is_ca,
                       const char *aia, const char *crl_dp)
{
    X509 *c = X509_new();
    X509_set_version(c, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(c), serial);
    X509_gmtime_adj(X509_getm_notBefore(c), -86400);
    X509_gmtime_adj(X509_getm_notAfter(c), 30 * 86400);
    X509_NAME *n = X509_get_subject_name(c);
    X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *)cn, -1, -1, 0);
    X509_set_issuer_name(c, issuer ? X509_get_subject_name(issuer) : n);
    X509_set_pubkey(c, key);
    if (is_ca) add_ext(c, issuer, NID_basic_constraints, "critical,CA:TRUE");
    else {
        add_ext(c, issuer, NID_subject_alt_name, "IP:127.0.0.1");
        if (aia) add_ext(c, issuer, NID_info_access, aia);
        if (crl_dp) add_ext(c, issuer, NID_crl_distribution_points, crl_dp);
    }
    X509_sign(c, issuer_key ? issuer_key : key, EVP_sha256());
    return c;
}

static void pki_make(Pki *p, const char *aia, const char *crl_dp)
{
    memset(p, 0, sizeof(*p));
    p->ca_key = gen_key();
    p->leaf_key = gen_key();
    p->ca = make_cert(p->ca_key, "Test CA", NULL, NULL, 1, 1, NULL, NULL);
    p->leaf = make_cert(p->leaf_key, "server.example.org", p->ca, p->ca_key, 4242, 0,
                        aia, crl_dp);
    p->store = X509_STORE_new();
    X509_STORE_add_cert(p->store, p->ca);
    p->chain = sk_X509_new_null();
    X509_up_ref(p->leaf); sk_X509_push(p->chain, p->leaf);
    X509_up_ref(p->ca);   sk_X509_push(p->chain, p->ca);
}

static void pki_free(Pki *p)
{
    sk_X509_pop_free(p->chain, X509_free);
    X509_STORE_free(p->store);
    X509_free(p->leaf); X509_free(p->ca);
    EVP_PKEY_free(p->leaf_key); EVP_PKEY_free(p->ca_key);
}

/** DER of an OCSP response about the leaf; @p status is V_OCSP_CERTSTATUS_*.
 *  @p sign_with_foreign_key makes the signature untrusted. */
static unsigned char *ocsp_der(Pki *p, int status, int sign_with_foreign_key, int *len)
{
    OCSP_BASICRESP *bs = OCSP_BASICRESP_new();
    OCSP_CERTID *id = OCSP_cert_to_id(NULL, p->leaf, p->ca);
    ASN1_TIME *this_u = X509_gmtime_adj(NULL, -60);
    ASN1_TIME *next_u = X509_gmtime_adj(NULL, 3600);
    ASN1_TIME *rev = status == V_OCSP_CERTSTATUS_REVOKED ? X509_gmtime_adj(NULL, -600) : NULL;
    OCSP_basic_add1_status(bs, id, status, 0, rev, this_u, next_u);
    EVP_PKEY *fk = sign_with_foreign_key ? gen_key() : NULL;
    X509 *fc = sign_with_foreign_key
        ? make_cert(fk, "Foreign CA", NULL, NULL, 9, 1, NULL, NULL) : NULL;
    OCSP_basic_sign(bs, fc ? fc : p->ca, fk ? fk : p->ca_key, EVP_sha256(), NULL, 0);
    OCSP_RESPONSE *r = OCSP_response_create(OCSP_RESPONSE_STATUS_SUCCESSFUL, bs);
    unsigned char *der = NULL;
    *len = i2d_OCSP_RESPONSE(r, &der);
    OCSP_RESPONSE_free(r); OCSP_BASICRESP_free(bs); OCSP_CERTID_free(id);
    ASN1_TIME_free(this_u); ASN1_TIME_free(next_u); ASN1_TIME_free(rev);
    X509_free(fc); EVP_PKEY_free(fk);
    return der;
}

/** DER of a CRL of the CA; lists the leaf when @p revoke; @p expired makes
 *  nextUpdate lie in the past. */
static unsigned char *crl_der(Pki *p, int revoke, int expired, int *len)
{
    X509_CRL *crl = X509_CRL_new();
    X509_CRL_set_version(crl, 1);
    X509_CRL_set_issuer_name(crl, X509_get_subject_name(p->ca));
    ASN1_TIME *lu = X509_gmtime_adj(NULL, expired ? -7200 : -60);
    ASN1_TIME *nu = X509_gmtime_adj(NULL, expired ? -3600 : 86400);
    X509_CRL_set1_lastUpdate(crl, lu);
    X509_CRL_set1_nextUpdate(crl, nu);
    if (revoke) {
        X509_REVOKED *r = X509_REVOKED_new();
        ASN1_INTEGER *sn = ASN1_INTEGER_new();
        ASN1_INTEGER_set(sn, 4242);
        X509_REVOKED_set_serialNumber(r, sn);
        ASN1_TIME *rt = X509_gmtime_adj(NULL, -600);
        X509_REVOKED_set_revocationDate(r, rt);
        X509_CRL_add0_revoked(crl, r);
        ASN1_INTEGER_free(sn); ASN1_TIME_free(rt);
    }
    X509_CRL_sort(crl);
    X509_CRL_sign(crl, p->ca_key, EVP_sha256());
    unsigned char *der = NULL;
    *len = i2d_X509_CRL(crl, &der);
    X509_CRL_free(crl); ASN1_TIME_free(lu); ASN1_TIME_free(nu);
    return der;
}

/* ── Forked mocks ───────────────────────────────────────────────────── */

static int listen_any(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    socklen_t l = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &l);
    *port = ntohs(a.sin_port);
    return fd;
}

/** A port nobody listens on. */
static int dead_port(void)
{
    int port = 0;
    int fd = listen_any(&port);
    close(fd);
    return port;
}

typedef struct { int lfd, port; pid_t pid; } Mock;

/** HTTP mock: POST /ocsp -> @p ocsp, GET /crl -> @p crl (either may be NULL = 500). */
static Mock http_mock_start(int lfd, int port, const unsigned char *ocsp, int ocsp_len,
                            const unsigned char *crl, int crl_len)
{
    Mock m = {lfd, port, 0};
    m.pid = fork();
    if (m.pid != 0) { close(lfd); return m; }
    alarm(60);
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) break;
        char req[8192];
        int total = 0;
        while (total < (int)sizeof(req) - 1) {
            ssize_t n = read(c, req + total, sizeof(req) - 1 - (size_t)total);
            if (n <= 0) break;
            total += (int)n;
            req[total] = '\0';
            char *he = strstr(req, "\r\n\r\n");
            if (he) {
                char *cl = strcasestr(req, "Content-Length:");
                long want = cl ? strtol(cl + 15, NULL, 10) : 0;
                if (total >= (int)(he - req) + 4 + want) break;
            }
        }
        const unsigned char *body = NULL;
        int blen = 0;
        if (strncmp(req, "POST /ocsp", 10) == 0) { body = ocsp; blen = ocsp_len; }
        else if (strncmp(req, "GET /crl", 8) == 0) { body = crl; blen = crl_len; }
        char head[200];
        if (body) {
            snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", blen);
            ssize_t w = write(c, head, strlen(head)); (void)w;
            w = write(c, body, (size_t)blen); (void)w;
        } else {
            const char *e = "HTTP/1.1 500 No\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            ssize_t w = write(c, e, strlen(e)); (void)w;
        }
        close(c);
    }
    _exit(0);
}

static void mock_stop(Mock *m)
{
    if (m->pid > 0) {
        kill(m->pid, SIGKILL);
        waitpid(m->pid, NULL, 0);
        m->pid = 0;
    }
}

/** The leaf names @p ocsp_port and @p crl_port (0 = names none of that kind). */
static void pki_for_ports(Pki *p, int ocsp_port, int crl_port)
{
    char aia[160], dp[160];
    snprintf(aia, sizeof(aia), "OCSP;URI:http://127.0.0.1:%d/ocsp", ocsp_port);
    snprintf(dp, sizeof(dp), "URI:http://127.0.0.1:%d/crl", crl_port);
    pki_make(p, ocsp_port ? aia : NULL, crl_port ? dp : NULL);
}

static char g_state_name[64];

static void fresh(void)
{
    static int n;
    tls_revocation_reset();
    snprintf(g_state_name, sizeof(g_state_name), "rev-state-%d", ++n);
    tls_revocation_set_state_dir(TP(g_state_name));
}

/* ── The check itself ───────────────────────────────────────────────── */

static TlsRevStatus run_check(Pki *p, char *why, size_t wl)
{
    return tls_revocation_check(p->leaf, p->chain, p->store, why, wl);
}

static void test_rev_ocsp_good(void)
{
    fresh();
    int op = 0; int ol = listen_any(&op);
    Pki p; pki_for_ports(&p, op, 0);
    int len; unsigned char *o = ocsp_der(&p, V_OCSP_CERTSTATUS_GOOD, 0, &len);
    Mock m = http_mock_start(ol, op, o, len, NULL, 0);
    char why[256];
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_GOOD, "OCSP good -> GOOD");
    ASSERT(strcmp(why, "OCSP") == 0, "decided by OCSP");
    mock_stop(&m); OPENSSL_free(o); pki_free(&p);
}

static void test_rev_ocsp_revoked(void)
{
    fresh();
    int op = 0; int ol = listen_any(&op);
    Pki p; pki_for_ports(&p, op, 0);
    int len; unsigned char *o = ocsp_der(&p, V_OCSP_CERTSTATUS_REVOKED, 0, &len);
    Mock m = http_mock_start(ol, op, o, len, NULL, 0);
    char why[256];
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_REVOKED, "OCSP revoked -> REVOKED");
    mock_stop(&m); OPENSSL_free(o); pki_free(&p);
}

static void test_rev_ocsp_answer_is_cached(void)
{
    fresh();
    int op = 0; int ol = listen_any(&op);
    Pki p; pki_for_ports(&p, op, 0);
    int len; unsigned char *o = ocsp_der(&p, V_OCSP_CERTSTATUS_GOOD, 0, &len);
    Mock m = http_mock_start(ol, op, o, len, NULL, 0);
    char why[256];
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_GOOD, "first check asks the responder");
    mock_stop(&m);
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_GOOD,
           "the answer is served from the cache while valid");
    OPENSSL_free(o); pki_free(&p);
}

static void test_rev_ocsp_untrusted_signature_is_not_used(void)
{
    fresh();
    int op = 0; int ol = listen_any(&op);
    Pki p; pki_for_ports(&p, op, 0);
    int len; unsigned char *o = ocsp_der(&p, V_OCSP_CERTSTATUS_GOOD, 1, &len);
    Mock m = http_mock_start(ol, op, o, len, NULL, 0);
    char why[256];
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_UNKNOWN,
           "an answer signed by a stranger decides nothing");
    ASSERT(strstr(why, "not signed by a trusted party") != NULL, "and says why");
    mock_stop(&m); OPENSSL_free(o); pki_free(&p);
}

static void test_rev_crl_when_ocsp_unreachable(void)
{
    fresh();
    int cp = 0; int cl = listen_any(&cp);
    Pki p; pki_for_ports(&p, dead_port(), cp);
    int len; unsigned char *c = crl_der(&p, 0, 0, &len);
    Mock m = http_mock_start(cl, cp, NULL, 0, c, len);
    char why[256];
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_GOOD, "OCSP down, CRL clean -> GOOD");
    ASSERT(strcmp(why, "CRL") == 0, "decided by the CRL");
    mock_stop(&m); OPENSSL_free(c); pki_free(&p);
}

static void test_rev_crl_lists_certificate(void)
{
    fresh();
    int cp = 0; int cl = listen_any(&cp);
    Pki p; pki_for_ports(&p, dead_port(), cp);
    int len; unsigned char *c = crl_der(&p, 1, 0, &len);
    Mock m = http_mock_start(cl, cp, NULL, 0, c, len);
    char why[256];
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_REVOKED, "CRL lists the certificate");
    mock_stop(&m); OPENSSL_free(c); pki_free(&p);
}

static void test_rev_crl_is_cached_with_its_validity(void)
{
    fresh();
    int cp = 0; int cl = listen_any(&cp);
    Pki p; pki_for_ports(&p, dead_port(), cp);
    int len; unsigned char *c = crl_der(&p, 0, 0, &len);
    Mock m = http_mock_start(cl, cp, NULL, 0, c, len);
    char why[256];
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_GOOD, "first check downloads the CRL");
    mock_stop(&m);
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_GOOD,
           "with the server gone the cached CRL is used");
    ASSERT(strcmp(why, "CRL") == 0, "still decided by the CRL");
    OPENSSL_free(c); pki_free(&p);
}

static void test_rev_expired_crl_counts_as_none(void)
{
    fresh();
    int cp = 0; int cl = listen_any(&cp);
    Pki p; pki_for_ports(&p, dead_port(), cp);
    int len; unsigned char *c = crl_der(&p, 1, 1, &len);   /* lists it, but expired */
    Mock m = http_mock_start(cl, cp, NULL, 0, c, len);
    char why[256];
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_UNKNOWN,
           "an expired CRL is treated as no CRL");
    ASSERT(strstr(why, "CRL has expired") != NULL,
           "the message says the CRL has expired, not that it is missing");
    mock_stop(&m); OPENSSL_free(c); pki_free(&p);
}

static void test_rev_nothing_reachable(void)
{
    fresh();
    Pki p; pki_for_ports(&p, dead_port(), dead_port());
    char why[256];
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_UNKNOWN, "neither source reachable");
    ASSERT(strstr(why, "no OCSP responder could be reached") != NULL, "names OCSP");
    ASSERT(strstr(why, "no CRL could be fetched") != NULL, "names the CRL");
    pki_free(&p);
}

static void test_rev_certificate_names_no_source(void)
{
    fresh();
    Pki p; pki_for_ports(&p, 0, 0);
    char why[256];
    ASSERT(run_check(&p, why, sizeof(why)) == TLS_REV_UNKNOWN, "no source named");
    ASSERT(strstr(why, "names no OCSP responder") != NULL, "says so for OCSP");
    ASSERT(strstr(why, "names no CRL") != NULL, "says so for the CRL");
    pki_free(&p);
}

static void test_rev_issuer_missing(void)
{
    fresh();
    Pki p; pki_for_ports(&p, 0, 0);
    STACK_OF(X509) *only = sk_X509_new_null();
    sk_X509_push(only, p.leaf);
    char why[256];
    ASSERT(tls_revocation_check(p.leaf, only, p.store, why, sizeof(why)) == TLS_REV_UNKNOWN,
           "without the issuer nothing can be asked");
    ASSERT(strstr(why, "issuer") != NULL, "says the issuer is missing");
    sk_X509_free(only);
    ASSERT(tls_revocation_check(p.leaf, p.chain, p.store, NULL, 0) == TLS_REV_UNKNOWN,
           "a NULL reason buffer is accepted");
    pki_free(&p);
}

/* ── Through real TLS connections ───────────────────────────────────── */

typedef enum { TLS_IMAP, TLS_HTTP } TlsKind;

/** TLS server presenting the leaf; serves @p rounds connections. */
static Mock tls_server_start(Pki *p, TlsKind kind, int rounds)
{
    int port = 0;
    int lfd = listen_any(&port);
    Mock m = {lfd, port, 0};
    m.pid = fork();
    if (m.pid != 0) { close(lfd); return m; }
    alarm(60);
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate(ctx, p->leaf);
    SSL_CTX_use_PrivateKey(ctx, p->leaf_key);
    for (int i = 0; i < rounds; i++) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) break;
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, c);
        if (SSL_accept(ssl) == 1) {
            char buf[1024];
            if (kind == TLS_IMAP) {
                SSL_write(ssl, "* OK hello\r\n", 12);
                int n = SSL_read(ssl, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    char tag[16] = "";
                    sscanf(buf, "%15s", tag);
                    char reply[64];
                    snprintf(reply, sizeof(reply), "%s OK done\r\n", tag);
                    SSL_write(ssl, reply, (int)strlen(reply));
                    SSL_read(ssl, buf, sizeof(buf) - 1);     /* LOGOUT */
                }
            } else {
                int n = SSL_read(ssl, buf, sizeof(buf) - 1);
                if (n > 0) {
                    const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                                    "Connection: close\r\n\r\nok";
                    SSL_write(ssl, r, (int)strlen(r));
                }
            }
            SSL_shutdown(ssl);
        }
        SSL_free(ssl);
        close(c);
    }
    _exit(0);
}

static const char *write_ca(Pki *p, const char *name)
{
    const char *path = TP(name);
    FILE *f = fopen(path, "w");
    if (f) { PEM_write_X509(f, p->ca); fclose(f); }
    return path;
}

static void test_rev_imap_refuses_a_revoked_certificate(void)
{
    fresh();
    int op = 0; int ol = listen_any(&op);
    Pki p; pki_for_ports(&p, op, 0);
    int len; unsigned char *o = ocsp_der(&p, V_OCSP_CERTSTATUS_REVOKED, 0, &len);
    Mock http = http_mock_start(ol, op, o, len, NULL, 0);
    setenv("SSL_CERT_FILE", write_ca(&p, "rev-ca-1.pem"), 1);
    Mock srv = tls_server_start(&p, TLS_IMAP, 2);
    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", srv.port);
    ImapClient *c = imap_connect(url, "u@example.org", "pw", 1);
    ASSERT(c == NULL, "a certificate the responder calls revoked is refused");
    mock_stop(&srv); mock_stop(&http); OPENSSL_free(o); pki_free(&p);
}

static void test_rev_imap_accepts_a_good_certificate(void)
{
    fresh();
    int op = 0; int ol = listen_any(&op);
    Pki p; pki_for_ports(&p, op, 0);
    int len; unsigned char *o = ocsp_der(&p, V_OCSP_CERTSTATUS_GOOD, 0, &len);
    Mock http = http_mock_start(ol, op, o, len, NULL, 0);
    setenv("SSL_CERT_FILE", write_ca(&p, "rev-ca-2.pem"), 1);
    Mock srv = tls_server_start(&p, TLS_IMAP, 2);
    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", srv.port);
    ImapClient *c = imap_connect(url, "u@example.org", "pw", 1);
    ASSERT(c != NULL, "a certificate the responder calls good is accepted");
    char note[512];
    ASSERT(emc_take_security_notice(note, sizeof(note)) == 0, "and no notice is raised");
    imap_disconnect(c);
    mock_stop(&srv); mock_stop(&http); OPENSSL_free(o); pki_free(&p);
}

static void test_rev_unavailable_check_lets_the_connection_go_on_with_one_signal(void)
{
    fresh();
    Pki p; pki_for_ports(&p, dead_port(), dead_port());
    setenv("SSL_CERT_FILE", write_ca(&p, "rev-ca-3.pem"), 1);
    Mock srv = tls_server_start(&p, TLS_IMAP, 3);
    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", srv.port);
    char note[512];

    ImapClient *c = imap_connect(url, "u@example.org", "pw", 1);
    ASSERT(c != NULL, "no revocation source: availability is preferred, the connection goes on");
    ASSERT(emc_take_security_notice(note, sizeof(note)) == 1, "but the caller gets a notice");
    ASSERT(strstr(note, "server.example.org") != NULL, "naming the certificate");
    ASSERT(strstr(note, "no OCSP responder could be reached") != NULL, "and the reason");
    imap_disconnect(c);

    c = imap_connect(url, "u@example.org", "pw", 1);
    ASSERT(c != NULL, "the next connection goes on too");
    ASSERT(emc_take_security_notice(note, sizeof(note)) == 0,
           "and does not repeat the signal within 6 hours");
    imap_disconnect(c);

    /* age the record by more than 6 hours: the signal is due again */
    char rec[1024];
    snprintf(rec, sizeof(rec), "%s/signals", TP(g_state_name));
    FILE *f = fopen(rec, "r");
    char line[200] = "";
    if (f) { if (!fgets(line, sizeof(line), f)) line[0] = '\0'; fclose(f); }
    ASSERT(line[0] != '\0', "the signal is recorded per certificate");
    char hex[80]; long long t;
    ASSERT(sscanf(line, "%79s %lld", hex, &t) == 2, "as fingerprint and time");
    f = fopen(rec, "w");
    fprintf(f, "%s %lld\n", hex, t - TLS_REV_SIGNAL_INTERVAL - 10);
    fclose(f);
    c = imap_connect(url, "u@example.org", "pw", 1);
    ASSERT(c != NULL, "still connects");
    ASSERT(emc_take_security_notice(note, sizeof(note)) == 1,
           "after 6 hours the signal is given again");
    imap_disconnect(c);
    mock_stop(&srv); pki_free(&p);
}

static void test_rev_insecure_connection_skips_the_check(void)
{
    fresh();
    Pki p; pki_for_ports(&p, dead_port(), dead_port());
    Mock srv = tls_server_start(&p, TLS_IMAP, 1);
    char url[64];
    snprintf(url, sizeof(url), "imaps://127.0.0.1:%d", srv.port);
    ImapClient *c = imap_connect(url, "u@example.org", "pw", 0);
    ASSERT(c != NULL, "verification off: connects");
    char note[512];
    ASSERT(emc_take_security_notice(note, sizeof(note)) == 0, "and no revocation check is made");
    imap_disconnect(c);
    mock_stop(&srv); pki_free(&p);
}

static size_t sink(void *p, size_t s, size_t n, void *u) { (void)p; (void)u; return s * n; }

static CURLcode curl_get(int port)
{
    char url[64];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/", port);
    CURL *curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    tls_revocation_prepare_curl(curl);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return rc;
}

static void test_rev_curl_connections_are_checked_too(void)
{
    fresh();
    int op = 0; int ol = listen_any(&op);
    Pki p; pki_for_ports(&p, op, 0);
    int len; unsigned char *o = ocsp_der(&p, V_OCSP_CERTSTATUS_REVOKED, 0, &len);
    Mock http = http_mock_start(ol, op, o, len, NULL, 0);
    setenv("SSL_CERT_FILE", write_ca(&p, "rev-ca-4.pem"), 1);
    Mock srv = tls_server_start(&p, TLS_HTTP, 2);
    ASSERT(curl_get(srv.port) == CURLE_PEER_FAILED_VERIFICATION,
           "libcurl (SMTP, Gmail) refuses a revoked certificate");
    mock_stop(&srv); mock_stop(&http); OPENSSL_free(o); pki_free(&p);

    fresh();
    ol = listen_any(&op);
    pki_for_ports(&p, op, 0);
    o = ocsp_der(&p, V_OCSP_CERTSTATUS_GOOD, 0, &len);
    http = http_mock_start(ol, op, o, len, NULL, 0);
    setenv("SSL_CERT_FILE", write_ca(&p, "rev-ca-5.pem"), 1);
    srv = tls_server_start(&p, TLS_HTTP, 2);
    ASSERT(curl_get(srv.port) == CURLE_OK, "and accepts a good one");
    mock_stop(&srv); mock_stop(&http); OPENSSL_free(o); pki_free(&p);
}

static void test_rev_notice_api(void)
{
    char buf[16];
    tls_revocation_reset();
    ASSERT(emc_take_security_notice(buf, sizeof(buf)) == 0, "no notice at first");
    ASSERT(emc_take_security_notice(NULL, 0) == 0, "NULL buffer is harmless");
}

void test_tls_revocation(void)
{
    signal(SIGPIPE, SIG_IGN);
    RUN_TEST(test_rev_ocsp_good);
    RUN_TEST(test_rev_ocsp_revoked);
    RUN_TEST(test_rev_ocsp_answer_is_cached);
    RUN_TEST(test_rev_ocsp_untrusted_signature_is_not_used);
    RUN_TEST(test_rev_crl_when_ocsp_unreachable);
    RUN_TEST(test_rev_crl_lists_certificate);
    RUN_TEST(test_rev_crl_is_cached_with_its_validity);
    RUN_TEST(test_rev_expired_crl_counts_as_none);
    RUN_TEST(test_rev_nothing_reachable);
    RUN_TEST(test_rev_certificate_names_no_source);
    RUN_TEST(test_rev_issuer_missing);
    RUN_TEST(test_rev_imap_refuses_a_revoked_certificate);
    RUN_TEST(test_rev_imap_accepts_a_good_certificate);
    RUN_TEST(test_rev_unavailable_check_lets_the_connection_go_on_with_one_signal);
    RUN_TEST(test_rev_insecure_connection_skips_the_check);
    RUN_TEST(test_rev_curl_connections_are_checked_too);
    RUN_TEST(test_rev_notice_api);
    unsetenv("SSL_CERT_FILE");
}
