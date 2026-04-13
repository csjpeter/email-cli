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

/**
 * @brief Interactively configure (or reconfigure) just the SMTP fields of an
 *        existing Config.
 *
 * Prompts the user for SMTP Host, Port, Username, and Password.  Pressing
 * Enter on any field keeps the current value (or accepts the shown default).
 * The caller is responsible for saving the updated Config via
 * config_save_to_store() if desired.
 *
 * @param cfg  Existing configuration to update in-place.  Must not be NULL.
 * @return 0 on success, -1 if the user aborted (EOF / Ctrl-D on host prompt).
 */
int setup_wizard_smtp(Config *cfg);

#endif // SETUP_WIZARD_H
