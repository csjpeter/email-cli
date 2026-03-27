/**
 * POSIX path implementation.
 * Uses $HOME / $XDG_CACHE_HOME / $XDG_CONFIG_HOME, falling back to
 * getpwuid(getuid()) for the home directory.
 */
#include "../path.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>

static char g_home[4096];
static char g_cache[4096];
static char g_config[4096];

const char *platform_home_dir(void) {
    const char *h = getenv("HOME");
    if (!h || !*h) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) h = pw->pw_dir;
    }
    if (!h) return NULL;
    snprintf(g_home, sizeof(g_home), "%s", h);
    return g_home;
}

const char *platform_cache_dir(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        snprintf(g_cache, sizeof(g_cache), "%s", xdg);
        return g_cache;
    }
    const char *home = platform_home_dir();
    if (!home) return NULL;
    snprintf(g_cache, sizeof(g_cache), "%s/.cache", home);
    return g_cache;
}

const char *platform_config_dir(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(g_config, sizeof(g_config), "%s", xdg);
        return g_config;
    }
    const char *home = platform_home_dir();
    if (!home) return NULL;
    snprintf(g_config, sizeof(g_config), "%s/.config", home);
    return g_config;
}
