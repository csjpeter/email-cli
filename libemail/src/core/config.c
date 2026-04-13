#include "config.h"
#include <stdlib.h>

/**
 * @brief Frees all heap-allocated fields of cfg, then frees cfg itself.
 */
void config_free(Config *cfg) {
    if (!cfg) return;
    free(cfg->host);
    free(cfg->user);
    free(cfg->pass);
    free(cfg->folder);
    free(cfg);
}
