#ifndef SMTP_ADAPTER_H
#define SMTP_ADAPTER_H

#include "config.h"
#include <stddef.h>

/**
 * @file smtp_adapter.h
 * @brief Send a pre-built RFC 2822 message via SMTP using libcurl.
 *
 * **TLS is mandatory.** Only smtps:// (implicit TLS, port 465) is accepted.
 * smtp:// is rejected at runtime to prevent credentials being sent in plaintext.
 *
 * URL resolution:
 *   cfg->smtp_host = "smtps://…"  → used as-is (implicit TLS, port 465)
 *   cfg->smtp_host = NULL         → derived from cfg->host:
 *                                   imaps://host → smtps://host
 *
 * Credentials: cfg->smtp_user / cfg->smtp_pass; falls back to cfg->user / cfg->pass.
 */

/**
 * @brief Send a pre-built RFC 2822 message via SMTP.
 *
 * @param cfg          Account configuration.
 * @param from         Envelope From address (e.g. "user@example.com").
 * @param to           Envelope To address.
 * @param message      Complete RFC 2822 wire bytes (headers + blank line + body).
 * @param message_len  Byte length of message.
 * @return 0 on success, -1 on error (details written to stderr and logger).
 */
int smtp_send(const Config *cfg,
              const char *from,
              const char *to,
              const char *message,
              size_t message_len);

#endif /* SMTP_ADAPTER_H */
