/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-16 */
/* Unit tests of the platform process and credential-key helpers. */
#include "test_helpers.h"
#include "platform/process.h"
#include "platform/credential_key.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * @file test_platform_identity.c
 * @brief platform_getpid(), platform_pid_is_program() and
 *        platform_derive_credential_key() (EMAIL-114).
 */

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fputs(text, f);
    return fclose(f);
}

/** Key for @p email with HOME = @p home. */
static int key_for(const char *home, const char *email, unsigned char key[32])
{
    setenv("HOME", home, 1);
    return platform_derive_credential_key(email, key);
}

/** The key the specification gives: HMAC-SHA256(source, "email-cli:<user>:<email>"). */
static void expected_key(const char *source, const char *user, const char *email,
                         unsigned char key[32])
{
    char ctx[1024];
    snprintf(ctx, sizeof(ctx), "email-cli:%s:%s", user, email);
    unsigned int n = 32;
    HMAC(EVP_sha256(), source, (int)strlen(source),
         (unsigned char *)ctx, strlen(ctx), key, &n);
}

static void test_process_identity(void)
{
    ASSERT(platform_getpid() == getpid(), "platform_getpid is the process id");

    char comm[64] = "";
    FILE *f = fopen("/proc/self/comm", "r");
    ASSERT(f != NULL, "/proc/self/comm is readable");
    ASSERT(fgets(comm, sizeof(comm), f) != NULL, "comm has a name");
    fclose(f);
    comm[strcspn(comm, "\r\n")] = '\0';

    ASSERT(platform_pid_is_program(getpid(), comm) == 1,
           "a live process is recognised by its name (newline stripped)");
    ASSERT(platform_pid_is_program(getpid(), "no-such-program") == 0,
           "a live process under another name is not the program");
    ASSERT(platform_pid_is_program(getpid(), "") == 0,
           "an empty name matches nothing");

    pid_t child = fork();
    if (child == 0) _exit(0);
    int st = 0;
    waitpid(child, &st, 0);
    ASSERT(platform_pid_is_program(child, comm) == 0,
           "a process that has exited is not the program");
    ASSERT(platform_pid_is_program((pid_t)0x3ffffff0, comm) == 0,
           "a pid nobody has is not the program");
    if (geteuid() != 0)
        ASSERT(platform_pid_is_program(1, "init") == 0,
               "a process we may not signal is not reported as ours");
}

static void test_comm_file_check(void)
{
    char dir[512];
    ASSERT(test_mkdtemp(dir, sizeof(dir), "comm") != NULL, "scratch dir");
    char path[640];
    snprintf(path, sizeof(path), "%s/comm", dir);

    write_file(path, "email-sync\n");
    ASSERT(platform_comm_file_is_program(path, "email-sync") == 1, "the name matches");
    ASSERT(platform_comm_file_is_program(path, "email-tui") == 0, "another name does not");
    write_file(path, "email-sync\r\n");
    ASSERT(platform_comm_file_is_program(path, "email-sync") == 1, "CR LF is stripped");
    write_file(path, "email-sync");
    ASSERT(platform_comm_file_is_program(path, "email-sync") == 1, "no line end is fine");
    write_file(path, "");
    ASSERT(platform_comm_file_is_program(path, "email-sync") == 1,
           "an empty file cannot verify: assume it matches");
    snprintf(path, sizeof(path), "%s/missing", dir);
    ASSERT(platform_comm_file_is_program(path, "email-sync") == 1,
           "a missing file cannot verify: assume it matches");
}

static void test_credential_key_sources(void)
{
    char home[512];
    ASSERT(test_mkdtemp(home, sizeof(home), "ckey") != NULL, "scratch home");
    char ssh[600], ed[640], ec[640], rsa[640];
    snprintf(ssh, sizeof(ssh), "%s/.ssh", home);
    snprintf(ed, sizeof(ed), "%s/id_ed25519", ssh);
    snprintf(ec, sizeof(ec), "%s/id_ecdsa", ssh);
    snprintf(rsa, sizeof(rsa), "%s/id_rsa", ssh);
    ASSERT(mkdir(ssh, 0700) == 0, "scratch .ssh");

    const char *saved_user = getenv("USER");
    char *keep_user = saved_user ? strdup(saved_user) : NULL;
    const char *saved_home = getenv("HOME");
    char *keep_home = saved_home ? strdup(saved_home) : NULL;
    setenv("USER", "keyuser", 1);

    unsigned char k[32], want[32], other[32];

    /* the ed25519 key is the first source, and the result is the HMAC of it */
    write_file(ed, "ED-KEY");
    write_file(ec, "EC-KEY");
    write_file(rsa, "RSA-KEY");
    int r1 = key_for(home, "a@example.org", k);
    expected_key("ED-KEY", "keyuser", "a@example.org", want);
    int ok1 = r1 == 0 && memcmp(k, want, 32) == 0;

    /* same inputs, same key; another address, another key; NULL = "" */
    int r2 = key_for(home, "a@example.org", other);
    int ok2 = r2 == 0 && memcmp(k, other, 32) == 0;
    int r3 = key_for(home, "b@example.org", other);
    int ok3 = r3 == 0 && memcmp(k, other, 32) != 0;
    int r4 = key_for(home, NULL, other);
    expected_key("ED-KEY", "keyuser", "", want);
    int ok4 = r4 == 0 && memcmp(other, want, 32) == 0;

    /* another user name, another key */
    setenv("USER", "someone-else", 1);
    int r5 = key_for(home, "a@example.org", other);
    int ok5 = r5 == 0 && memcmp(k, other, 32) != 0;
    setenv("USER", "keyuser", 1);

    /* ecdsa when there is no ed25519; an empty ed25519 file does not count */
    write_file(ed, "");
    int r6 = key_for(home, "a@example.org", k);
    expected_key("EC-KEY", "keyuser", "a@example.org", want);
    int ok6 = r6 == 0 && memcmp(k, want, 32) == 0;
    unlink(ed);
    int r7 = key_for(home, "a@example.org", k);
    int ok7 = r7 == 0 && memcmp(k, want, 32) == 0;

    /* rsa last */
    unlink(ec);
    int r8 = key_for(home, "a@example.org", k);
    expected_key("RSA-KEY", "keyuser", "a@example.org", want);
    int ok8 = r8 == 0 && memcmp(k, want, 32) == 0;

    /* no SSH key: the machine id, if the machine has one */
    unlink(rsa);
    char mid[256] = "";
    FILE *f = fopen("/etc/machine-id", "rb");
    size_t mn = f ? fread(mid, 1, sizeof(mid) - 1, f) : 0;
    if (f) fclose(f);
    mid[mn] = '\0';
    int r9 = key_for(home, "a@example.org", k);
    int ok9 = 1;
    if (mn > 0) {
        expected_key(mid, "keyuser", "a@example.org", want);
        ok9 = r9 == 0 && memcmp(k, want, 32) == 0;
    } else {
        ok9 = r9 == -1;
    }

    /* HOME unset: the home of the account is used (no crash, same rules) */
    unsetenv("HOME");
    int r10 = platform_derive_credential_key("a@example.org", other);
    int ok10 = r10 == 0 || r10 == -1;
    /* USER unset: the account name is used */
    unsetenv("USER");
    setenv("HOME", home, 1);
    struct passwd *pw = getpwuid(getuid());
    int r11 = platform_derive_credential_key("a@example.org", other);
    int ok11 = 1;
    if (r11 == 0 && mn > 0 && pw) {
        expected_key(mid, pw->pw_name, "a@example.org", want);
        ok11 = memcmp(other, want, 32) == 0;
    }

    if (keep_user) { setenv("USER", keep_user, 1); free(keep_user); }
    if (keep_home) { setenv("HOME", keep_home, 1); free(keep_home); }

    ASSERT(ok1, "ed25519 key: the key is HMAC-SHA256(key file, \"email-cli:user:email\")");
    ASSERT(ok2, "the same inputs give the same key");
    ASSERT(ok3, "another address gives another key");
    ASSERT(ok4, "no address is the empty address");
    ASSERT(ok5, "another user name gives another key");
    ASSERT(ok6, "an empty ed25519 file is skipped for the ecdsa key");
    ASSERT(ok7, "ecdsa is used when there is no ed25519 file");
    ASSERT(ok8, "rsa is the last SSH key");
    ASSERT(ok9, "without SSH keys the machine id is the source (none: failure)");
    ASSERT(ok10, "HOME unset does not break the derivation");
    ASSERT(ok11, "USER unset: the account's name goes into the key");
}

void test_platform_identity(void)
{
    test_process_identity();
    test_comm_file_check();
    test_credential_key_sources();
}
