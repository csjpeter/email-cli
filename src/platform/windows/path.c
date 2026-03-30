/* Windows path implementation — to be implemented for MinGW-w64 */
/* Uses %USERPROFILE%, %APPDATA%, %LOCALAPPDATA% */
#include "../path.h"
#include <stdlib.h>
#include <stdio.h>

static char g_home[4096], g_cache[4096], g_config[4096], g_data[4096];

const char *platform_home_dir(void) {
    char *h = getenv("USERPROFILE");
    if (h) { snprintf(g_home, sizeof(g_home), "%s", h); return g_home; }
    return NULL;
}

const char *platform_cache_dir(void) {
    char *h = getenv("LOCALAPPDATA");
    if (h) { snprintf(g_cache, sizeof(g_cache), "%s", h); return g_cache; }
    return NULL;
}

const char *platform_config_dir(void) {
    char *h = getenv("APPDATA");
    if (h) { snprintf(g_config, sizeof(g_config), "%s", h); return g_config; }
    return NULL;
}

const char *platform_data_dir(void) {
    /* Windows has no separate "data" dir; %APPDATA% is the closest equivalent */
    char *h = getenv("APPDATA");
    if (h) { snprintf(g_data, sizeof(g_data), "%s", h); return g_data; }
    return NULL;
}
