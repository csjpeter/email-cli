#ifndef EMAIL_SERVICE_H
#define EMAIL_SERVICE_H

#include "config_store.h"

/**
 * High-level service for fetching email data.
 */

/**
 * Fetches the raw content of the 5 most recent emails.
 */
int email_service_fetch_recent(const Config *cfg);

#endif // EMAIL_SERVICE_H
