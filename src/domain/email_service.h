#ifndef EMAIL_SERVICE_H
#define EMAIL_SERVICE_H

#include "config_store.h"

/**
 * @file email_service.h
 * @brief High-level service for fetching email data.
 */

/**
 * @brief Fetches the raw content of recent emails.
 * @param cfg Pointer to the connection configuration.
 * @return 0 on success, -1 on failure.
 */
int email_service_fetch_recent(const Config *cfg);

#endif // EMAIL_SERVICE_H
