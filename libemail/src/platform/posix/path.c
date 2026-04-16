/**
 * POSIX path implementation.
 * Uses $HOME / $XDG_CACHE_HOME / $XDG_CONFIG_HOME / $XDG_DATA_HOME,
 * falling back to getpwuid(getuid()) for the home directory.
 */
#include "../path.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static char g_home[4096];
static char g_cache[8192];
static char g_config[8192];
static char g_data[8192];

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

const char *platform_data_dir(void) {
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        snprintf(g_data, sizeof(g_data), "%s", xdg);
        return g_data;
    }
    const char *home = platform_home_dir();
    if (!home) return NULL;
    snprintf(g_data, sizeof(g_data), "%s/.local/share", home);
    return g_data;
}

int platform_executable_path(char *buf, size_t size) {
    if (!buf || size == 0) return -1;
#ifdef __linux__
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n > 0) { buf[n] = '\0'; return 0; }
    return -1;
#elif defined(__APPLE__)
    uint32_t len = (uint32_t)size;
    if (_NSGetExecutablePath(buf, &len) == 0) return 0;
    return -1;
#else
    /* Generic POSIX fallback: try /proc/curproc/file (FreeBSD) */
    ssize_t n = readlink("/proc/curproc/file", buf, size - 1);
    if (n > 0) { buf[n] = '\0'; return 0; }
    return -1;
#endif
}
