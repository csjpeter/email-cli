#ifndef PLATFORM_CREDENTIAL_KEY_H
#define PLATFORM_CREDENTIAL_KEY_H

/**
 * @file credential_key.h
 * @brief Derives a per-account obfuscation key from stable system-specific data.
 *
 * The key is derived from the first available source in priority order:
 *   1. User's SSH private key (~/.ssh/id_ed25519, id_ecdsa, id_rsa)
 *   2. Machine-specific stable identifier (platform-dependent)
 *
 * The 32-byte output is computed as:
 *   HMAC-SHA256(key=<source bytes>, msg="email-cli:<username>:<email>")
 *
 * This means each account's key is unique, and the key is bound to the
 * installation — config files copied to another machine cannot be decrypted.
 */

/**
 * @brief Derive a 32-byte AES key for credential obfuscation.
 *
 * @param email    Account email address; used to make each account's key
 *                 unique even when the underlying source is the same.
 * @param key_out  Output buffer of exactly 32 bytes.
 * @return 0 on success, -1 if no suitable key source was found (caller
 *         should fall back to storing credentials as plaintext).
 */
int platform_derive_credential_key(const char *email, unsigned char key_out[32]);

#endif /* PLATFORM_CREDENTIAL_KEY_H */
