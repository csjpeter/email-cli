/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* Revocation check (OCSP, then CRL) of a verified server certificate. */
#ifndef TLS_REVOCATION_H
#define TLS_REVOCATION_H

#include <stddef.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

/**
 * @file tls_revocation.h
 * @brief Revocation check of the server certificate every TLS connection of
 *        the library verifies (IMAP, SMTP and the HTTPS calls, all on OpenSSL).
 *
 * Policy (network-and-config-model.md): after the chain is verified, the leaf
 * certificate is checked, OCSP first at the responder it names, then the CRL
 * it names (cached with its validity; an expired CRL counts as none).  A
 * certificate known to be revoked is refused.  When neither source can be
 * used the connection goes on, with an error-level log entry and a notice for
 * the caller, at most once per certificate and 6 hours.
 */

/** Result of a revocation check. */
typedef enum {
    TLS_REV_GOOD,     /**< OCSP or the CRL says the certificate is not revoked. */
    TLS_REV_REVOKED,  /**< OCSP or the CRL says the certificate is revoked. */
    TLS_REV_UNKNOWN   /**< Neither source could be used (see @c why). */
} TlsRevStatus;

/** Minimum seconds between two signals about the same certificate. */
#define TLS_REV_SIGNAL_INTERVAL (6 * 60 * 60)

/**
 * @brief Check one leaf certificate.
 * @param cert   Verified leaf certificate.
 * @param chain  Verified chain (leaf first); the issuer is looked up in it.
 * @param store  Trust store used to verify OCSP responses; may be NULL.
 * @param why    Receives a short reason when the result is not GOOD/REVOKED
 *               or says which source decided; may be NULL.
 * @param why_len Size of @p why.
 */
TlsRevStatus tls_revocation_check(X509 *cert, STACK_OF(X509) *chain,
                                  X509_STORE *store, char *why, size_t why_len);

/**
 * @brief Verify callback of an SSL_CTX that verifies the peer: runs the
 *        revocation check at the leaf, refuses a revoked certificate and
 *        records the signal when the check could not be made.
 */
int tls_revocation_verify_cb(int preverify_ok, X509_STORE_CTX *sctx);

/** @brief Install tls_revocation_verify_cb on a context that verifies the peer. */
void tls_revocation_attach(SSL_CTX *ctx);

/**
 * @brief Take the pending notice (text for the user) set by the last check
 *        that could not be made.
 * @return 1 and the text in @p buf when there was one, 0 when not.
 */
int tls_revocation_take_notice(char *buf, size_t size);

/** @brief Set the directory of the CRL cache and the signal record (tests). */
void tls_revocation_set_state_dir(const char *dir);

/** @brief Forget cached OCSP answers and the pending notice (tests). */
void tls_revocation_reset(void);

#endif /* TLS_REVOCATION_H */
