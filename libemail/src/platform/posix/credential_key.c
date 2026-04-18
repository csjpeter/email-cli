/**
 * POSIX credential key derivation.
 *
 * Priority order for key source material:
 *   1. ~/.ssh/id_ed25519
 *   2. ~/.ssh/id_ecdsa
 *   3. ~/.ssh/id_rsa
 *   4. /etc/machine-id         (Linux)
 *   4. kern.uuid sysctl        (macOS)
 *
 * Key derivation: HMAC-SHA256(key=<source bytes>, msg="email-cli:<user>:<email>")
 */
#include "../credential_key.h"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

/* Read up to bufsize-1 bytes from path into buf. Returns bytes read, -1 on error. */
static ssize_t read_source(const char *path, char *buf, size_t bufsize) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    size_t n = fread(buf, 1, bufsize - 1, fp);
    fclose(fp);
    if (n == 0) return -1;
    buf[n] = '\0';
    return (ssize_t)n;
}

static const char *get_username(void) {
    const char *u = getenv("USER");
    if (u && *u) return u;
    struct passwd *pw = getpwuid(getuid());
    return (pw && pw->pw_name) ? pw->pw_name : "unknown";
}

static const char *get_home(void) {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    return (pw && pw->pw_dir) ? pw->pw_dir : NULL;
}

int platform_derive_credential_key(const char *email, unsigned char key_out[32]) {
    static const char *SSH_KEYS[] = {
        "/.ssh/id_ed25519",
        "/.ssh/id_ecdsa",
        "/.ssh/id_rsa",
        NULL
    };

    char source[65536]; /* large enough for any SSH private key */
    ssize_t source_len = -1;

    /* 1. Try SSH private keys */
    const char *home = get_home();
    if (home) {
        for (int i = 0; SSH_KEYS[i] && source_len < 0; i++) {
            char path[4096];
            snprintf(path, sizeof(path), "%s%s", home, SSH_KEYS[i]);
            source_len = read_source(path, source, sizeof(source));
        }
    }

    /* 2. Machine-specific fallback */
#ifdef __APPLE__
    if (source_len < 0) {
        /* macOS: hardware UUID via kern.uuid sysctl (stable across reboots/updates) */
        size_t len = sizeof(source) - 1;
        if (sysctlbyname("kern.uuid", source, &len, NULL, 0) == 0) {
            source[len] = '\0';
            source_len = (ssize_t)len;
        }
    }
#else
    if (source_len < 0)
        source_len = read_source("/etc/machine-id", source, sizeof(source));
#endif

    if (source_len <= 0)
        return -1;

    /* Build HMAC message: "email-cli:<username>:<email>" */
    const char *username = get_username();
    char context[1024];
    snprintf(context, sizeof(context), "email-cli:%s:%s",
             username, email ? email : "");

    /* HMAC-SHA256(key=source, msg=context) → 32 bytes */
    unsigned int out_len = 32;
    if (!HMAC(EVP_sha256(),
              source, (int)source_len,
              (unsigned char *)context, (int)strlen(context),
              key_out, &out_len))
        return -1;

    return 0;
}
