#ifndef SETUP_WIZARD_H
#define SETUP_WIZARD_H

#include "config_store.h"

/**
 * @file setup_wizard.h
 * @brief Interactive configuration wizard.
 */

/**
 * @brief Runs an interactive CLI wizard to collect email settings.
 * @return Pointer to a newly allocated Config struct or NULL if aborted.
 */
Config* setup_wizard_run(void);

#endif // SETUP_WIZARD_H
