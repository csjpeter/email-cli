/**
 * Windows credential key derivation.
 *
 * Priority order for key source material:
 *   1. %USERPROFILE%\.ssh\id_ed25519
 *   2. %USERPROFILE%\.ssh\id_ecdsa
 *   3. %USERPROFILE%\.ssh\id_rsa
 *   4. HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid (registry)
 */
#include "../credential_key.h"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static ssize_t read_source(const char *path, char *buf, size_t bufsize) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    size_t n = fread(buf, 1, bufsize - 1, fp);
    fclose(fp);
    if (n == 0) return -1;
    buf[n] = '\0';
    return (ssize_t)n;
}

int platform_derive_credential_key(const char *email, unsigned char key_out[32]) {
    static const char *SSH_KEYS[] = {
        "\\.ssh\\id_ed25519",
        "\\.ssh\\id_ecdsa",
        "\\.ssh\\id_rsa",
        NULL
    };

    char source[65536];
    ssize_t source_len = -1;

    /* 1. Try SSH private keys */
    const char *userprofile = getenv("USERPROFILE");
    if (userprofile) {
        for (int i = 0; SSH_KEYS[i] && source_len < 0; i++) {
            char path[4096];
            snprintf(path, sizeof(path), "%s%s", userprofile, SSH_KEYS[i]);
            source_len = read_source(path, source, sizeof(source));
        }
    }

    /* 2. Windows MachineGuid from registry */
    if (source_len < 0) {
        HKEY key;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Microsoft\\Cryptography",
                          0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            DWORD type, sz = (DWORD)(sizeof(source) - 1);
            if (RegQueryValueExA(key, "MachineGuid", NULL, &type,
                                 (LPBYTE)source, &sz) == ERROR_SUCCESS) {
                source[sz] = '\0';
                source_len = (ssize_t)sz;
            }
            RegCloseKey(key);
        }
    }

    if (source_len <= 0)
        return -1;

    const char *username = getenv("USERNAME");
    char context[1024];
    snprintf(context, sizeof(context), "email-cli:%s:%s",
             username ? username : "unknown", email ? email : "");

    unsigned int out_len = 32;
    if (!HMAC(EVP_sha256(),
              source, (int)source_len,
              (unsigned char *)context, (int)strlen(context),
              key_out, &out_len))
        return -1;

    return 0;
}
