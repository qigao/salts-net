#ifndef PASSWORD_HASH_H
#define PASSWORD_HASH_H

#include <stddef.h>

/* OpenSSL-based password hashing to replace libsodium */

#define PASSWORD_HASH_STRBYTES 128

/**
 * @brief Hash a password using PBKDF2-SHA256
 * @param out Output buffer (must be at least PASSWORD_HASH_STRBYTES bytes)
 * @param password Password to hash
 * @param password_len Length of password
 * @return 0 on success, -1 on failure
 */
int password_hash(char *out, const char *password, size_t password_len);

/**
 * @brief Verify a password against a hash
 * @param hash The stored hash string
 * @param password Password to verify
 * @param password_len Length of password
 * @return 0 if password matches, -1 if not
 */
int password_verify(const char *hash, const char *password, size_t password_len);

#endif /* PASSWORD_HASH_H */
