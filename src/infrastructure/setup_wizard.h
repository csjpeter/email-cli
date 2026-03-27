#ifndef SETUP_WIZARD_H
#define SETUP_WIZARD_H

#include "config.h"

/**
 * @file setup_wizard.h
 * @brief Interactive configuration wizard.
 */

#include <stdio.h>

/**
 * @brief Runs an interactive CLI wizard to collect email settings.
 * @return Pointer to a newly allocated Config struct or NULL if aborted.
 */
Config* setup_wizard_run(void);

/**
 * @brief Internal wizard implementation that can take any input stream (for testing).
 */
Config* setup_wizard_run_internal(FILE *stream);

#endif // SETUP_WIZARD_H
